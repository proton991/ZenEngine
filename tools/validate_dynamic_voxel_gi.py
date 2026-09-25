"""M0 float lighting baselines. This does not exercise a dynamic GI provider yet."""
import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import validate_voxel_gi as legacy

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/dynamic-voxel-m0/captures'
# Locked before reading captures. These are numeric capture checks, not future DDA quality gates.
CONTRACT=dict(component_abs=2e-5,component_rel=2e-5,
    sum_components=[1,2,3,4],diffuse_components=[5,6],
    emission_probe=[8,.5,.125],emission_abs=1e-4,
    regions={'room':{'floor':[.43,.67,.57,.72],'left_wall':[.375,.4,.405,.6],
                     'right_wall':[.595,.4,.625,.6],'back_wall':[.45,.45,.55,.6]},
             'sponza':{'hall':[.35,.3,.65,.75]}})


def load_lighting(prefix):
    metadata=json.loads(Path(str(prefix)+'.lighting.json').read_text())
    assert metadata['bytes_per_pixel']==112
    data=array.array('f');data.frombytes(Path(str(prefix)+'.lighting.bin').read_bytes())
    if sys.byteorder!='little':data.byteswap()
    assert len(data)==metadata['width']*metadata['height']*28
    return metadata,data


def analyze(prefix, scene):
    metadata,data=load_lighting(prefix)
    valid=above_one=0;max_sum_error=max_diffuse_error=0.
    emission_matches=0
    for offset in range(0,len(data),28):
        pixel=data[offset:offset+28]
        assert all(math.isfinite(v) and v>=0 for v in pixel),'Invalid HDR value'
        assert pixel[3] in (0,1) and all(pixel[i*4+3]==pixel[3] for i in range(7))
        if pixel[3]==0:
            assert not any(pixel),'Uncleared background'
            continue
        valid+=1;above_one+=int(max(pixel)>1)
        emission_matches+=int(all(abs(pixel[16+c]-CONTRACT['emission_probe'][c])<=CONTRACT['emission_abs'] for c in range(3)))
        for channel in range(3):
            for target,parts in ((channel,CONTRACT['sum_components']),(8+channel,CONTRACT['diffuse_components'])):
                expected=sum(pixel[p*4+channel] for p in parts)
                error=abs(pixel[target]-expected)
                assert error<=CONTRACT['component_abs']+CONTRACT['component_rel']*max(abs(expected),abs(pixel[target])),(offset,channel,target,error)
                if target<3:max_sum_error=max(max_sum_error,error)
                else:max_diffuse_error=max(max_diffuse_error,error)
    assert valid>0
    stats=dict(valid_pixels=valid,background_pixels=len(data)//28-valid,above_one_pixels=above_one,
               emission_probe_pixels=emission_matches,max_sum_error=max_sum_error,max_diffuse_error=max_diffuse_error,regions={})
    width,height=metadata['width'],metadata['height']
    for name,(x0,y0,x1,y1) in CONTRACT['regions'].get(scene,{}).items():
        sums=[[0.,0.,0.] for _ in range(7)];count=0
        for y in range(int(y0*height),int(y1*height)):
            for x in range(int(x0*width),int(x1*width)):
                offset=(y*width+x)*28
                if data[offset+3]:
                    count+=1
                    for c in range(7):
                        for rgb in range(3):sums[c][rgb]+=data[offset+c*4+rgb]
        assert count>0,(scene,name,'Empty measurement region')
        stats['regions'][name]=dict(valid_pixels=count,component_mean_rgb=[[v/count for v in values] for values in sums])
    Path(str(prefix)+'.stats.json').write_text(json.dumps(stats,indent=2))
    return stats


def compare_component(a,b,component,tolerance=2e-6):
    ma,da=load_lighting(a);mb,db=load_lighting(b)
    assert (ma['width'],ma['height'])==(mb['width'],mb['height'])
    error=max(abs(da[i+c]-db[i+c]) for i in range(component*4,len(da),28) for c in range(4))
    assert error<=tolerance,(a,b,component,error)
    return error


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=OUT)
    parser.add_argument('--quick',action='store_true',help='One room capture to check the transport/readback path')
    args=parser.parse_args();folder=args.output.resolve();folder.mkdir(parents=True,exist_ok=True)
    (folder/'locked-contract.json').write_text(json.dumps(CONTRACT,indent=2))
    fixtures=folder/'fixtures';fixtures.mkdir(exist_ok=True)
    legacy.OUT=fixtures;legacy.make_fixture()
    probe=json.loads((fixtures/'emissive.gltf').read_text());probe['materials'][3]['emissiveFactor']=[1,.0625,.015625]
    (fixtures/'emission-probe.gltf').write_text(json.dumps(probe))
    models={'room':fixtures/'room.gltf','sponza':ROOT.parent/'glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf',
            'emission':fixtures/'emission-probe.gltf'}
    config=ROOT/'Data/engine.cfg';original=config.read_bytes();last=original
    env=os.environ.copy();env.update(VK_LAYER_VALIDATE_SYNC='1',VK_LOADER_LAYERS_DISABLE='~implicit~',DISABLE_RTSS_LAYER='1')
    results={}

    def run(tag,scene,backend='geom',thread=0,queue=0,changes=None,capture=True,mode=3):
        nonlocal last
        settings=dict(default_model_path=models[scene].as_posix(),camera_position='.55,-.15,0' if scene=='sponza' else '0,0,2',
            voxelizer=backend,voxel_resolution=64,voxel_reflectance_policy='averaged',voxel_reflectance_budget_mb=512,
            voxel_gi_method='cone',environment_lighting='true',environment_intensity=1,environment_rotation_degrees=0,
            skybox_visible='false',light_count=1,voxel_gi_indirect_intensity=1)
        settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false',
            'light.0.type':'point','light.0.position':'0,0.1,0.25','light.0.color':'1,1,1',
            'light.0.intensity':2,'light.0.range':4,'light.0.enabled':'true','light.0.casts_shadows':'true'})
        settings.update(changes or {})
        assert config.read_bytes()==last,'Configuration changed externally'
        last=original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode();config.write_bytes(last)
        prefix=folder/tag;Path(str(prefix)+'.cfg').write_bytes(last)
        command=[str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'),'--frames=3',f'--mode={mode}',
            f'--rhi-thread={thread}',f'--async-compute={queue}',f'--capture={prefix}.ppm']
        if capture:command.append(f'--capture-lighting={prefix}')
        with Path(str(prefix)+'.log').open('w') as output:
            completed=subprocess.run(command,cwd=ROOT,env=env,stdout=output,stderr=subprocess.STDOUT,timeout=180)
        text=Path(str(prefix)+'.log').read_text(errors='replace')
        errors=[line for line in text.splitlines() if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
        assert completed.returncode==0 and not errors,(tag,errors[:3])
        legacy.load_capture(Path(str(prefix)+'.ppm'))
        results[tag]=analyze(prefix,scene) if capture else dict(presentation_only=True)
        print('PASS',tag,flush=True)
        return prefix

    try:
        if args.quick:
            run('quick-room','room')
        else:
            for scene in ('room','sponza'):
                reference=None
                for backend in ('geom','comp'):
                    for thread in (0,1):
                        for queue in (0,1):
                            prefix=run(f'{scene}-{backend}-thread{thread}-async{queue}',scene,backend,thread,queue)
                            digest=hashlib.sha256(Path(str(prefix)+'.lighting.bin').read_bytes()).hexdigest()
                            if reference is None:reference=digest
                            assert digest==reference,'Lighting differs across voxelizer or submission mode'
                    owner=run(f'{scene}-{backend}-owner',scene,backend,changes=dict(voxel_reflectance_policy='owner'))
                    ordinary=run(f'{scene}-{backend}-owner-no-diagnostics',scene,backend,changes=dict(voxel_reflectance_policy='owner'),capture=False)
                    assert Path(str(owner)+'.ppm').read_bytes()==Path(str(ordinary)+'.ppm').read_bytes(),'Capture changed presentation'
                    previous=ROOT/'build/dynamic-voxel-v1/images'/f'{scene}-{backend}-owner.ppm'
                    if previous.exists():assert previous.read_bytes()==Path(str(ordinary)+'.ppm').read_bytes(),'Legacy owner presentation changed'
            base=folder/'room-geom-thread0-async0'
            zero=run('room-zero-indirect','room',changes=dict(voxel_gi_indirect_intensity=0))
            _,data=load_lighting(zero)
            assert all(data[i+c]==0 for i in range(24,len(data),28) for c in range(3))
            for component in (1,3,4,5):compare_component(base,zero,component)
            direct=run('room-direct-only-shadows','room',changes=dict(voxel_gi_indirect_intensity=0,environment_lighting='false'))
            _,data=load_lighting(direct)
            assert all(data[i+component*4+c]==0 for i in range(0,len(data),28) for component in (2,3,4) for c in range(3))
            probe=run('emission-probe','emission',changes=dict(light_count=0,environment_lighting='false'))
            assert results[probe.name]['emission_probe_pixels']>0 and results[probe.name]['above_one_pixels']>0
            run('pbr-components','room',mode=2)
            markers=run('room-markers','room',changes={'light_markers.enabled':'true'})
            assert Path(str(markers)+'.lighting.bin').read_bytes()==Path(str(base)+'.lighting.bin').read_bytes()
            for backend in ('voxel_dda','hardware_rt'):
                prefix=run('unavailable-'+backend,'room',changes=dict(voxel_gi_method='dynamic_voxel',dynamic_voxel_gi_query_backend=backend))
                metadata,_=load_lighting(prefix);assert metadata['method']=='cone' and metadata['query_backend']=='none'
                assert Path(str(prefix)+'.lighting.bin').read_bytes()==Path(str(base)+'.lighting.bin').read_bytes()
    finally:
        assert config.read_bytes()==last,'Config changed externally; preserving it'
        config.write_bytes(original)
        (folder/'results.json').write_text(json.dumps(results,indent=2))
        (folder/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__=='__main__':
    main()
