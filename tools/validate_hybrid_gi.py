"""Reproduce hybrid GI fixture captures and check closed-surface reconstruction.

Uses the real raster receivers and imported frame histories. Restores engine.cfg
transactionally. Numerical reports use linear captures, never display images.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import numpy as np
from hybrid_gi_fixtures import generate

ROOT=Path(__file__).resolve().parents[1]


def environment_integral(prefix, metadata, normal):
    """Independent unoccluded Lambert integral: quadrature over the captured cubemap."""
    size = metadata['environment_cube_size']
    cube = np.fromfile(str(prefix)+'.environment.bin','<f4').reshape(6,size,size,4)
    xy = (np.arange(size)+.5)*2/size-1
    x,y = np.meshgrid(xy,xy)
    one = np.ones_like(x)
    directions = np.stack([np.stack(face,axis=-1) for face in
                           ((one,-y,-x),(-one,-y,x),(x,one,y),(x,-one,-y),(x,-y,one),(-x,-y,-one))])
    length = np.linalg.norm(directions,axis=-1)
    directions /= length[...,None]
    angle = metadata['environment_intensity_rotation_enabled_visible'][1]
    c,s = np.cos(angle),np.sin(angle)
    normal = np.array([c*normal[0]-s*normal[2],normal[1],s*normal[0]+c*normal[2]])
    q = np.array(metadata['environment_orientation'])
    normal += 2*np.cross(q[:3],np.cross(q[:3],normal)+q[3]*normal)
    weight = np.maximum(directions@normal,0)*4/(size*size*np.pi*length**3)
    return np.sum(cube[...,:3]*weight[...,None],axis=(0,1,2))


def run(exe,folder,names,frames=64,provider="voxel",allow_present_baseline=False,rt=False,samples=4,indirect=0,gpu=None,reference_samples=0):
    fixtures=folder/'fixtures'
    generate(fixtures)
    config=ROOT/'Data/engine.cfg'
    original=config.read_bytes()
    active=original
    environment=os.environ.copy()
    environment.update(VK_LOADER_LAYERS_DISABLE='~implicit~',DISABLE_RTSS_LAYER='1',VK_LAYER_VALIDATE_SYNC='1')
    results={}
    try:
        for name in names:
            settings=dict(default_model_path=(fixtures/(name+'.gltf')).as_posix(),environment_texture=(fixtures/'constant.hdr').as_posix(),
                          environment_lighting='true',environment_intensity=1,skybox_visible='false',
                          scene_lighting_override='true',light_count=0,voxel_resolution=64,voxel_gi_indirect_intensity=indirect,
                          voxel_gi_shadow_enabled='false',voxel_gi_ray_provider=provider,voxel_gi_samples=samples,
                          voxel_gi_reference_samples=reference_samples,
                          voxel_gi_temporal='true',voxel_gi_filter='true',voxel_gi_specular_occlusion='true',
                          async_compute='auto')
            settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false'})
            settings['environment_rotation_degrees'] = 0
            if name in ('bright_environment_plane','rotated_environment_plane','black_environment_plane') or name.startswith('bright_edge_'):
                settings['environment_texture'] = (fixtures/('black.hdr' if name=='black_environment_plane' else 'bright.hdr')).as_posix()
                settings['environment_rotation_degrees'] = 73 if name=='rotated_environment_plane' else 0
            if name in ('point_light_room','thin_wall_light','glossy_floor','moving_occluder_light'):
                settings['scene_lighting_override']='false'
            assert config.read_bytes()==active,'External config edit; aborting'
            active=original+b'\n'+''.join(f'{key}={value}\n' for key,value in settings.items()).encode()
            config.write_bytes(active)
            prefix=folder/name
            assert frames >= 2
            command=[str(exe),'--no-ui','--fixed-step',f'--frames={frames-1}',
                     '--mode=3','--width=320','--height=180',f'--capture-lighting={prefix}',f'--profile={prefix}']
            if not rt: command.append('--disable-rt')
            if gpu: command.append('--gpu='+gpu)
            with prefix.with_suffix('.log').open('w') as log:
                result=subprocess.run(command,cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=180)
            log=prefix.with_suffix('.log').read_text(errors='replace')
            errors=[line for line in log.splitlines() if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
            baseline=[line for line in errors if 'SYNC-HAZARD-PRESENT-AFTER-WRITE' in line]
            if allow_present_baseline:
                errors=[line for line in errors if line not in baseline]
            assert result.returncode==0 and not errors,(name,result.returncode,errors[:5])
            metadata=json.loads(Path(str(prefix)+'.lighting.json').read_text())
            expected_provider = 'hardware' if rt and provider in ('hardware','auto') else 'legacy' if provider == 'legacy' else 'voxel'
            assert metadata['ray_provider'] == expected_provider, metadata
            if expected_provider == 'hardware':
                assert metadata['ray_scene_ready'] and metadata['ray_scene_generation'] > 0, metadata
            if provider == 'legacy':
                results[name] = {'legacy':True}
                continue
            capture=np.fromfile(str(prefix)+'.hybrid.bin','<f4').reshape(180,320,13,4)
            mask=capture[:,:,8,3]>0
            assert mask.any() and np.isfinite(capture).all()
            sky=capture[:,:,1,:3][mask];specular=capture[:,:,5,3][mask]
            # Component 11: NDC motion (previous - current) and previous w; a static view must not move.
            motion=capture[:,:,11,:][mask]
            assert np.all(motion[:,2]>0),(name,'receiver behind the previous camera')
            motion_error=float(np.linalg.norm(motion[:,:2]*.5*[320,180],axis=1).max())
            if name != 'moving_occluder_light':
                assert motion_error < .01, (name,'static reprojection',motion_error)
            lighting=np.fromfile(str(prefix)+'.lighting.bin','<f4').reshape(180,320,7,4)
            composition_error=float(np.max(abs(lighting[:,:,0,:3]-lighting[:,:,1:5,:3].sum(2))))
            split_error=float(np.max(abs(lighting[:,:,2,:3]-lighting[:,:,5:7,:3].sum(2))))
            assert composition_error < 1e-5 and split_error < 1e-5,(name,composition_error,split_error)
            report=dict(preexisting_present_hazards=len(baseline),sky_mean=float(sky.mean()),sky_max=float(sky.max()),S_mean=float(specular.mean()),
                        history_min=float(capture[:,:,7,0][mask].min()),history_max=float(capture[:,:,7,0][mask].max()),
                        covered_pixels=int(mask.sum()),motion_error_pixels=motion_error,composition_error=composition_error,
                        diffuse_split_error=split_error,reset_reason=metadata['reset_reason'])
            if name in ('open_plane','forward_open_plane'):
                assert abs(sky.mean()-1) <= .005,report
                assert np.min(capture[:,:,10,1][mask]) > .9999
                # No normal map: shading uses the vertex normal. The 8-bit default normal texel
                # once tilted it by 0.0039 in x and z (0.32 degrees); G-buffer encoding error is ~1e-5.
                assert np.abs(capture[:,:,9,[0,2]][mask]).max() < 1e-4,('shading normal tilted',float(np.abs(capture[:,:,9,[0,2]][mask]).max()))
                # Below-horizon lobe directions are outside the specular integral, so S stays at 1.
                assert abs(specular.mean()-1) <= .005,report
            if name in ('closed_box','forward_closed_box','single_sided_closed_box'):
                assert sky.max()<=1e-6 and specular.max()==0,report
            if name in ('bright_environment_plane','rotated_environment_plane'):
                expected = environment_integral(prefix,metadata,capture[:,:,9,:3][mask][0])
                error = sky-expected
                scale = expected.mean()
                report.update(reference_rgb=expected.tolist(),bias=float(error.mean()/scale),
                              rms=float(np.sqrt(np.mean(error**2))/scale),p99=float(np.quantile(abs(error),.99)/scale))
                assert abs(report['bias'])<=.02 and report['rms']<=.08 and report['p99']<=.2,report
            if name == 'black_environment_plane':
                assert sky.max()==0,report
            if name == 'moving_occluder_light':
                assert metadata['scene_geometry_generation'] > 1,metadata
                assert report['history_max'] == 1,report
            results[name]=report
            print(name,report,flush=True)
    finally:
        if config.read_bytes()==active: config.write_bytes(original)
        else: raise RuntimeError('External config edit; leaving engine.cfg untouched')
    (folder/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    return results


def sky_cache_check(exe,folder,frames=64,allow_present_baseline=False,gpu=None):
    """Compare the hardware voxel sky cache with the voxel-provider cache through cone bounce.

    The sky cache only reaches the image through injected radiance, so floor receivers
    beside the half-wall read the wall's cached sky light. Self-intersecting cache rays
    darken the hardware result; the voxel start bias makes the voxel cache slightly
    darker, so the accepted ratio is asymmetric."""
    bounce={}
    for provider in ('voxel','hardware'):
        run(exe,folder/provider,['half_wall'],frames,provider,allow_present_baseline,provider=='hardware',indirect=1,gpu=gpu)
        prefix=folder/provider/'half_wall'
        metadata=json.loads(Path(str(prefix)+'.lighting.json').read_text())
        capture=np.fromfile(str(prefix)+'.hybrid.bin','<f4').reshape(metadata['height'],metadata['width'],13,4)
        near=(capture[:,:,8,3]>0)&(capture[:,:,10,1]>.99)&(np.abs(capture[:,:,8,0])<.15)
        assert near.sum()>1000,(provider,int(near.sum()))
        bounce[provider]=float(capture[:,:,3,:3][near].mean())
    report=dict(bounce,ratio=bounce['hardware']/bounce['voxel'])
    print('sky_cache',report,flush=True)
    # Healthy RTX 5080 ratio 1.037; starting cache rays on the surface (self-hits) gives 0.903.
    assert bounce['voxel']>0 and .97<=report['ratio']<=1.10,report
    (folder/'sky_cache.json').write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'build/hybrid-gi')
    parser.add_argument('--fixtures',nargs='+',default=['open_plane','closed_box','single_sided_closed_box','narrow_slot','thin_pole','alpha_mask','mirrored_two_sided'])
    parser.add_argument('--frames',type=int,default=64)
    parser.add_argument('--samples',type=int,choices=(1,2,4),default=4)
    parser.add_argument('--provider',choices=['voxel','legacy','hardware','auto'],default='voxel')
    parser.add_argument('--rt',action='store_true')
    parser.add_argument('--gpu',help='Device name substring passed to the native demo')
    parser.add_argument('--reference-samples',type=int,choices=(0,1024,4096),default=0)
    parser.add_argument('--sky-cache-check',action='store_true',help='Compare hardware and voxel sky caches through half-wall bounce (needs RT)')
    parser.add_argument('--allow-present-baseline',action='store_true',help='Report the separately reproduced legacy presentation hazard without blocking image checks')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    if args.sky_cache_check:
        sky_cache_check(args.exe.resolve(),args.output.resolve(),args.frames,args.allow_present_baseline,args.gpu)
    else:
        run(args.exe.resolve(),args.output.resolve(),args.fixtures,args.frames,args.provider,args.allow_present_baseline,args.rt,args.samples,gpu=args.gpu,reference_samples=args.reference_samples)
