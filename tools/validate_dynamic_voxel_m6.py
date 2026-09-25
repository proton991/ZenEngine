"""M6 HDR lighting/material regressions. Owns engine.cfg serially and restores exact bytes."""
import argparse
import array
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess

import validate_voxel_gi as legacy
import validate_dynamic_voxel_gi as hdr
import validate_static_voxel_gi as static

ROOT = Path(__file__).resolve().parents[1]


def fixtures(folder):
    legacy.OUT = folder
    legacy.make_fixture()
    legacy.make_surface_contract_fixtures()
    uv1 = json.loads((folder/'surface-texture-shared-slot.gltf').read_text())
    for mesh in uv1['meshes']:
        for primitive in mesh['primitives']:
            # Preserve UV0 for the existing receiver normal-map tangent basis;
            # only the base-color/metallic-roughness sampling moves to UV1.
            primitive['attributes']['TEXCOORD_1'] = primitive['attributes']['TEXCOORD_0']
    pbr = uv1['materials'][0]['pbrMetallicRoughness']
    for key in ('baseColorTexture','metallicRoughnessTexture'): pbr[key]['texCoord'] = 1
    (folder/'surface-texture-uv1.gltf').write_text(json.dumps(uv1))
    source = json.loads((folder/'room.gltf').read_text())
    transformed = copy.deepcopy(source); transformed['nodes'][0]['scale'] = [2,.5,1.5]
    (folder/'nonuniform-transform.gltf').write_text(json.dumps(transformed))
    baked = copy.deepcopy(source); data = bytearray((folder/'room.bin').read_bytes())
    positions = {p['attributes']['POSITION'] for m in baked['meshes'] for p in m['primitives']}
    for index in positions:
        accessor = baked['accessors'][index]; view = baked['bufferViews'][accessor['bufferView']]
        start = view.get('byteOffset',0)+accessor.get('byteOffset',0)
        for vertex in range(accessor['count']):
            offset = start+vertex*view.get('byteStride',12)
            p = struct.unpack_from('<3f',data,offset)
            struct.pack_into('<3f',data,offset,*(v*s for v,s in zip(p,(2,.5,1.5))))
        for key in ('min','max'): accessor[key] = [v*s for v,s in zip(accessor[key],(2,.5,1.5))]
    baked['buffers'][0]['uri'] = 'nonuniform-baked.bin'
    (folder/'nonuniform-baked.bin').write_bytes(data)
    (folder/'nonuniform-baked.gltf').write_text(json.dumps(baked))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'build/dynamic-voxel-m6/scenes')
    parser.add_argument('--quick',action='store_true')
    parser.add_argument('--exe',type=Path,default=ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe')
    args = parser.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    folder=out/'fixtures'; folder.mkdir(exist_ok=True); fixtures(folder)
    config=ROOT/'Data/engine.cfg'; original=config.read_bytes(); last=original
    env=os.environ.copy(); env.update(VK_LAYER_VALIDATE_SYNC='1',VK_LOADER_LAYERS_DISABLE='~implicit~',DISABLE_RTSS_LAYER='1')
    results={}; outputs={}

    def run(name,model='room',changes=None,frames=12,smoke=False):
        nonlocal last
        tag=f'{voxelizer}-{name}'; prefix=out/tag
        settings=dict(default_model_path=(folder/(model+'.gltf')).as_posix(),camera_position='0,0,2',
            voxelizer=voxelizer,voxel_resolution=64,voxel_reflectance_policy='averaged',voxel_reflectance_budget_mb=512,
            voxel_gi_method='dynamic_voxel',dynamic_voxel_gi_query_backend='voxel_dda',dynamic_voxel_gi_memory_budget_mb=3072,
            dynamic_voxel_gi_temporal_filter='off',dynamic_voxel_gi_spatial_filter='false',
            dynamic_voxel_gi_analytic_lighting='true',dynamic_voxel_gi_environment_lighting='true',dynamic_voxel_gi_emissive_lighting='true',
            environment_lighting='false',environment_intensity=1,environment_rotation_degrees=0,skybox_visible='false',
            light_count=1,voxel_gi_indirect_intensity=1,voxel_gi_shadow_enabled='true')
        settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false'})
        for i,position in enumerate(('-.25,.25,-.1','.25,.25,-.1','-.25,.25,.1','.25,.25,.1','0,.15,.25')):
            settings.update({f'light.{i}.type':'point',f'light.{i}.position':position,f'light.{i}.color':'1,1,1',
                f'light.{i}.intensity':1,f'light.{i}.range':4,f'light.{i}.enabled':'true',f'light.{i}.casts_shadows':'true'})
        settings.update(changes or {})
        assert config.read_bytes()==last,'Configuration changed externally'
        last=original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode(); config.write_bytes(last)
        Path(str(prefix)+'.cfg').write_bytes(last)
        command=[str(args.exe.resolve()),f'--frames={frames}','--mode=3',f'--rhi-thread={thread}',
            f'--async-compute={queue}','--width=320','--height=180','--gbuffer-size=256',f'--capture-lighting={prefix}']
        if smoke: command.append('--smoke-test')
        with Path(str(prefix)+'.log').open('w') as log:
            process=subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=300)
        text=Path(str(prefix)+'.log').read_text(errors='replace')
        errors=[line for line in text.splitlines() if any(x in line for x in ('[error]','VUID-','SYNC-HAZARD'))]
        assert process.returncode==0 and not errors,(tag,process.returncode,errors[:3])
        assert '[OK] No memory leaks detected' in text
        stats=hdr.analyze(prefix,'room'); surface_stats,_=static.analyze(prefix); stats.update(surface_stats)
        meta,data=hdr.load_lighting(prefix); assert meta['method']=='dynamic_voxel'
        components=[sum(data[i+c] for i in range(4*component,len(data),28) for c in range(3)) for component in range(7)]
        stats.update(component_sums=components,enabled_lights=len(meta['lights']))
        voxel_meta=json.loads(Path(str(prefix)+'.static.json').read_text())
        raw=Path(str(prefix)+'.static.bin').read_bytes(); cells=64**3
        masks=struct.unpack_from('<'+str(cells)+'I',raw,cells*6*32)
        assert all(mask < (1<<len(meta['lights'])) for mask in masks)
        results[tag]=stats; outputs[tag]=prefix
        print('PASS',tag,'lights',len(meta['lights']),'diffuse',components[2],flush=True)
        return prefix,stats

    try:
        for voxelizer,thread,queue in ([('comp',0,0)] if args.quick else [('comp',0,0),('geom',1,1)]):
            _,dark=run('zero',changes=dict(light_count=0))
            assert dark['component_sums'][0]==0
            _,sky=run('sky',changes=dict(light_count=0,environment_lighting='true'))
            _,visible=run('sky-zero-gain',changes=dict(light_count=0,environment_lighting='true',voxel_gi_indirect_intensity=0))
            assert 0 < visible['component_sums'][2] <= sky['component_sums'][2]
            run('sky-rotated',changes=dict(light_count=0,environment_lighting='true',environment_rotation_degrees=90))
            _,emission=run('emission',model='emissive',changes=dict(light_count=0))
            _,no_emission=run('emission-disabled',model='emissive',changes=dict(light_count=0,dynamic_voxel_gi_emissive_lighting='false'))
            assert emission['component_sums'][2]>0 and no_emission['component_sums'][2]==0
            assert emission['component_sums'][4]==no_emission['component_sums'][4]>0
            mixed=dict(light_count=5,environment_lighting='true')
            _,all_terms=run('mixed',model='emissive',changes=mixed)
            for name,key in [('analytic','analytic'),('environment','environment'),('emissive','emissive')]:
                _,isolated=run('without-'+name,model='emissive',changes=mixed|{f'dynamic_voxel_gi_{key}_lighting':'false'})
                assert isolated['component_sums'][2] < all_terms['component_sums'][2]
            run('mixed-filtered',model='emissive',changes=mixed|dict(dynamic_voxel_gi_temporal_filter='elapsed',dynamic_voxel_gi_spatial_filter='true'))
            five,_=run('five',changes=dict(light_count=5))
            markers,_=run('five-markers',changes={'light_count':5,'light_markers.enabled':'true'})
            assert Path(str(five)+'.lighting.bin').read_bytes()==Path(str(markers)+'.lighting.bin').read_bytes()
            moving={'light_count':5,'dynamic_light.enabled':'true','dynamic_light.index':4,
                'dynamic_light.orbit_center':'0,.15,0','dynamic_light.orbit_radius':.25,
                'dynamic_light.angular_speed_degrees':90,'light_markers.enabled':'true'}
            animated=[]
            for name,frames in [('fifth-first',33),('fifth-moved',34),('fifth-removed',35)]:
                prefix,stats=run(name,changes=moving,frames=frames,smoke=True); animated.append((prefix,stats))
            assert [s['enabled_lights'] for _,s in animated]==[5,5,4]
            assert Path(str(animated[0][0])+'.lighting.bin').read_bytes()!=Path(str(animated[1][0])+'.lighting.bin').read_bytes()
            for name in ('factor','vertex','vertex-rgba','masked','opaque'):
                run('material-'+name,model='material-'+name)
            for names in [('factor','vertex','vertex-rgba'),('masked','opaque')]:
                images=[Path(str(outputs[f'{voxelizer}-material-{name}'])+'.lighting.bin').read_bytes() for name in names]
                assert all(image==images[0] for image in images)
            texture_names=('split','shared-image','shared-slot','uv1')
            for name in texture_names: run('texture-'+name,model='surface-texture-'+name)
            images=[Path(str(outputs[f'{voxelizer}-texture-{name}'])+'.lighting.bin').read_bytes() for name in texture_names]
            assert all(image==images[0] for image in images),'Texture-role or UV selection mismatch'
            surface=static.floats(Path(str(outputs[f'{voxelizer}-texture-uv1'])+'.surface.bin'))
            linear=((128/255+.055)/1.055)**2.4
            for i in range(0,len(surface),24):
                if surface[i+3]:
                    assert abs(surface[i+16]-linear)<1/255+.0001
                    assert abs(surface[i+19]-128/255)<1/255+.0001
            first,_=run('nonuniform-transform',model='nonuniform-transform')
            second,_=run('nonuniform-baked',model='nonuniform-baked')
            _,a=hdr.load_lighting(first); _,b=hdr.load_lighting(second)
            assert len(a)==len(b) and max(abs(x-y) for x,y in zip(a,b))<.0001
    finally:
        assert config.read_bytes()==last,'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (out/'results.json').write_text(json.dumps(results,indent=2))
        (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__=='__main__': main()
