"""Reproduce P0-P3 fixture captures and check closed-surface reconstruction.

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


def run(exe,folder,names,frames=64,provider="voxel",allow_present_baseline=False):
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
                          scene_lighting_override='true',light_count=0,voxel_resolution=64,voxel_gi_indirect_intensity=0,
                          voxel_gi_shadow_enabled='false',voxel_gi_ray_provider=provider,voxel_gi_samples=4,
                          voxel_gi_temporal='true',voxel_gi_filter='true',voxel_gi_specular_occlusion='true',
                          async_compute='auto')
            settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false'})
            if name in ('point_light_room','thin_wall_light','glossy_floor','moving_occluder_light'):
                settings['scene_lighting_override']='false'
            assert config.read_bytes()==active,'External config edit; aborting'
            active=original+b'\n'+''.join(f'{key}={value}\n' for key,value in settings.items()).encode()
            config.write_bytes(active)
            prefix=folder/name
            assert frames >= 2
            command=[str(exe),'--disable-rt','--no-ui','--fixed-step',f'--frames={frames-1}',
                     '--mode=3','--width=320','--height=180',f'--capture-lighting={prefix}',f'--profile={prefix}']
            with prefix.with_suffix('.log').open('w') as log:
                result=subprocess.run(command,cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=180)
            log=prefix.with_suffix('.log').read_text(errors='replace')
            errors=[line for line in log.splitlines() if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
            baseline=[line for line in errors if 'SYNC-HAZARD-PRESENT-AFTER-WRITE' in line]
            if allow_present_baseline:
                errors=[line for line in errors if line not in baseline]
            assert result.returncode==0 and not errors,(name,result.returncode,errors[:5])
            metadata=json.loads(Path(str(prefix)+'.lighting.json').read_text())
            if provider == 'legacy':
                results[name] = {'legacy':True}
                continue
            capture=np.fromfile(str(prefix)+'.hybrid.bin','<f4').reshape(180,320,13,4)
            mask=capture[:,:,8,3]>0
            assert mask.any() and np.isfinite(capture).all()
            sky=capture[:,:,1,:3][mask];specular=capture[:,:,5,3][mask]
            clip=capture[:,:,11,:][mask]
            previous=(clip[:,:2]/clip[:,3:4]*.5+.5)*[320,180]-.5
            yy,xx=np.indices(mask.shape)
            motion_error=float(np.linalg.norm(previous-np.stack((xx,yy),axis=-1)[mask],axis=1).max())
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
                # Below-horizon lobe directions are outside the specular integral, so S stays at 1.
                assert abs(specular.mean()-1) <= .005,report
            if name in ('closed_box','forward_closed_box'):
                assert sky.max()<=1e-6 and specular.max()==0,report
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


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'build/hybrid-gi')
    parser.add_argument('--fixtures',nargs='+',default=['open_plane','closed_box','narrow_slot','thin_pole','alpha_mask','mirrored_two_sided'])
    parser.add_argument('--frames',type=int,default=64)
    parser.add_argument('--provider',choices=['voxel','legacy'],default='voxel')
    parser.add_argument('--allow-present-baseline',action='store_true',help='Report the separately reproduced legacy presentation hazard without blocking image checks')
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    run(args.exe.resolve(),args.output.resolve(),args.fixtures,args.frames,args.provider,args.allow_present_baseline)
