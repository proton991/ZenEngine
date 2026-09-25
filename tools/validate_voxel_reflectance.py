"""V1 verification. Run config-mutating phases serially; restore exact config bytes."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import validate_voxel_gi as legacy
from voxelization_reflectance import mixture_fixture

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/dynamic-voxel-v1'


def raw():
    cases=[('geometry-64',['--fixture','geometry','--matrix','--lifecycle','--grid-percent','100']),
        ('materials-64',['--fixture','materials','--matrix','--lifecycle','--gbuffer','--grid-percent','100']),
        ('materials-128',['--fixture','materials','--resolution','128','--grid-percent','100']),
        ('materials-reversed',['--fixture','materials','--reverse-order','--grid-percent','100']),
        ('mixture-128',['--fixture','mixtures','--resolution','128','--grid-percent','100']),
        ('mixture-256',['--fixture','mixtures','--resolution','256','--grid-percent','100'])]
    cases += [('mixture-'+v,['--fixture','mixtures','--mixture-variant',v,'--grid-percent','100'])
              for v in ('base','reverse','duplicate','tessellated')]
    for name,args in cases:
        subprocess.run([sys.executable,str(ROOT/'tools/validate_voxelization.py'),
            '--reflectance-policy','averaged','--output',str(OUT/name),*args],check=True)
    check_order()


def check_order():
    results={}
    for variant,count,multiplier in (('base',4,1),('reverse',4,1),('duplicate',5,2),('tessellated',7,4)):
        path=OUT/('mixture-'+variant)/'geom-64-thread0-async0.reflectance.bin'
        records=[v[:5] for v in struct.iter_unpack('<5I3f',path.read_bytes()) if v[3]]
        assert len(records)==1
        record=records[0]
        assert record[:4]==(3931*multiplier,0,1966,count),record
        results[variant]=record
    assert results['base']==results['reverse']
    for suffix in ('.reflectance.bin',):
        a=OUT/'materials-64'/('geom-64-thread0-async0'+suffix)
        b=OUT/'materials-reversed'/('geom-64-thread0-async0'+suffix)
        assert a.read_bytes()==b.read_bytes(), 'Record reversal changed the contribution multiset'
    (OUT/'order-duplicate-tessellation.json').write_text(json.dumps(results,indent=2))


def images():
    folder=OUT/'images';folder.mkdir(parents=True,exist_ok=True)
    fixtures=folder/'fixtures';fixtures.mkdir(exist_ok=True)
    legacy.OUT=fixtures;legacy.make_fixture()
    mixture=mixture_fixture(fixtures,64,'base')
    mixed=json.loads((fixtures/'room.gltf').read_text())
    mixed['materials'].append(dict(pbrMetallicRoughness=dict(baseColorFactor=[.05,.05,.8,1],metallicFactor=.5,roughnessFactor=1)))
    mixed['meshes'][0]['primitives'].append({**mixed['meshes'][0]['primitives'][3], 'material':4})
    (fixtures/'mixed-room.gltf').write_text(json.dumps(mixed))
    sponza=ROOT.parent/'glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf'
    assert sponza.exists()
    config=ROOT/'Data/engine.cfg';original=config.read_bytes();last=original
    env=os.environ.copy();env.update(VK_LAYER_VALIDATE_SYNC='1',VK_LOADER_LAYERS_DISABLE='~implicit~',DISABLE_RTSS_LAYER='1')
    results={}
    try:
        for scene,model,camera in (('room',fixtures/'room.gltf','0,0,2'),('mixed-room',fixtures/'mixed-room.gltf','0,0,2'),('sponza',sponza,'.55,-.15,0'),
                                   ('mixture',mixture,'0,0,2')):
            for backend in ('geom','comp'):
                captures={}
                for policy in ('owner','averaged'):
                    tag=f'{scene}-{backend}-{policy}';prefix=folder/tag
                    settings=dict(default_model_path=model.as_posix(),camera_position=camera,
                        voxelizer=backend,voxel_resolution=64,voxel_reflectance_policy=policy,
                        voxel_reflectance_budget_mb=512,environment_lighting='true',
                        environment_intensity=1,environment_rotation_degrees=0,skybox_visible='false',
                        light_count=1,voxel_gi_indirect_intensity=1)
                    settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false',
                        'light.0.type':'point','light.0.position':'0,0.1,0.25','light.0.color':'1,1,1',
                        'light.0.intensity':2,'light.0.range':4,'light.0.enabled':'true','light.0.casts_shadows':'true'})
                    args=[]
                    if scene=='mixture':
                        settings.update(environment_lighting='false')
                        settings.update({'light.0.type':'directional','light.0.direction':'0,0,-1',
                            'light.0.intensity':math.pi,'light.0.casts_shadows':'false'})
                        args=['--voxel-grid-percent=100']
                    assert config.read_bytes()==last
                    last=original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode()
                    config.write_bytes(last);Path(str(prefix)+'.cfg').write_bytes(last)
                    with Path(str(prefix)+'.log').open('w') as log:
                        result=subprocess.run([str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'),'--frames=3','--mode=3',
                            '--rhi-thread=0','--async-compute=0',f'--capture={prefix}.ppm',
                            f'--capture-voxels={prefix}',*args],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=180)
                    text=Path(str(prefix)+'.log').read_text(errors='replace')
                    errors=[line for line in text.splitlines() if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
                    assert result.returncode==0 and not errors,(tag,errors[:3])
                    metadata=json.loads(Path(str(prefix)+'.json').read_text())
                    assert metadata['reflectance_policy']==policy
                    captures[policy]=legacy.load_capture(Path(str(prefix)+'.ppm'))
                    if scene=='mixture' and policy=='averaged':
                        records=[v for v in struct.iter_unpack('<5I3f',Path(str(prefix)+'.reflectance.bin').read_bytes()) if v[3]]
                        assert metadata['has_radiance']==1 and len(records)==1
                        r,g,b,count,packed,lr,lg,lb=records[0]
                        normal=1/math.sqrt(1+2*(1/255)**2)
                        expected=[((packed>>(i*8))&255)/255*normal for i in range(3)]
                        assert max(abs(a-b) for a,b in zip((lr,lg,lb),expected))<.00025,(records,expected)
                        results[tag+'-radiance']=dict(actual=[lr,lg,lb],expected=expected,tolerance=.00025)
                    print('PASS',tag,flush=True)
                owner=(folder/f'{scene}-{backend}-owner.voxels.bin').read_bytes()
                averaged=(folder/f'{scene}-{backend}-averaged.voxels.bin').read_bytes()
                assert owner==averaged,'Averaging changed owner surface records'
                differences=[abs(a-b) for a,b in zip(captures['owner'][2],captures['averaged'][2])]
                results[f'{scene}-{backend}']=dict(owner_bytes_equal=True,changed_channels=sum(v!=0 for v in differences),
                    mean_abs_byte_difference=sum(differences)/len(differences),max_byte_difference=max(differences))
                if scene=='mixed-room':
                    assert max(differences)>1,'Mixed sender did not affect indirect lighting'
        for scene in ('room','mixed-room','sponza','mixture'):
            for policy in ('owner','averaged'):
                for suffix in ('.ppm','.voxels.bin')+ (('.reflectance.bin',) if policy=='averaged' else ()):
                    assert (folder/f'{scene}-geom-{policy}{suffix}').read_bytes()==(folder/f'{scene}-comp-{policy}{suffix}').read_bytes(),(scene,policy,suffix)
    finally:
        assert config.read_bytes()==last,'Config changed externally; preserving it'
        config.write_bytes(original)
        (folder/'results.json').write_text(json.dumps(results,indent=2))
        (folder/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


def gi():
    for policy in ('owner','averaged'):
        legacy.OUT=OUT/('gi-'+policy)
        legacy.main(dict(voxel_reflectance_policy=policy,voxel_reflectance_budget_mb=512))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase',choices=('all','raw','images','gi','order'),default='all')
    args=parser.parse_args();OUT.mkdir(parents=True,exist_ok=True)
    for name,run in (('raw',raw),('images',images),('gi',gi),('order',check_order)):
        if args.phase==name or (args.phase=='all' and name!='order'):
            run()


if __name__=='__main__':
    main()
