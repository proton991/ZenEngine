"""Stage A acceptance: wider grids, method switches, Sponza and triangle-reference error.

Owns engine.cfg serially. Triangle comparisons quantify the declared voxel approximation;
the existing independent interpolation/energy bounds remain the rendering correctness gate.
"""
import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import struct
import subprocess

import validate_dynamic_voxel_gi as hdr
import validate_dynamic_voxel_m4 as lifecycle
import validate_static_voxel_gi as static
from voxelization_fixtures import Fixture

ROOT = Path(__file__).resolve().parents[1]


def triangle_fixture(folder, name):
    asset = Fixture()
    asset.document['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[.7,.7,.7,1], metallicFactor=0, roughnessFactor=1)),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0),
             emissiveFactor=[1,0,0],extensions={'KHR_materials_emissive_strength':{'emissiveStrength':4}}),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1],metallicFactor=0))]
    asset.mesh([[(-.5,)*3]*3,[(.5,)*3]*3])
    triangles = []
    def panel(x0,x1,y0,y1,z,slope,material):
        p=[(x,y,z+slope*x) for x,y in [(x0,y0),(x1,y0),(x1,y1),(x0,y1)]]
        mesh=[[p[i] for i in t] for t in [(0,1,2),(0,2,3)]]
        asset.mesh(mesh,material=material)
        triangles.extend((t,4 if material==1 else 0) for t in mesh)
    panel(-.3,.3,-.3,.3,-.23,0,0)
    panel(-.12,.12,-.12,.12,.18,.3,1)
    if name!='emitter': panel(-.035,.035,-.17,.17,-.07,.8 if name=='slanted' else 0,2)
    return asset.save(folder,'triangle-'+name),triangles


def triangle_distance(origin,direction,triangle):
    # Double-precision, two-sided Moller-Trumbore; independent of voxel traversal/coverage.
    def sub(a,b): return tuple(x-y for x,y in zip(a,b))
    def dot(a,b): return sum(x*y for x,y in zip(a,b))
    def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
    edge1,edge2=sub(triangle[1],triangle[0]),sub(triangle[2],triangle[0])
    p=cross(direction,edge2);det=dot(edge1,p)
    if abs(det)<1e-12: return math.inf
    delta=sub(origin,triangle[0]);u=dot(delta,p)/det;q=cross(delta,edge1);v=dot(direction,q)/det
    distance=dot(edge2,q)/det
    return distance if u>=0 and v>=0 and u+v<=1 and distance>1e-6 else math.inf


def triangle_error(prefix,triangles):
    meta=json.loads(Path(str(prefix)+'.static.json').read_text());n=meta['resolution'];cells=n**3
    raw=Path(str(prefix)+'.static.bin').read_bytes();minimum=meta['minimum_cell_size'];cell_size=minimum[3]
    ids=struct.unpack_from('<'+str(meta['selected_static'])+'I',raw,meta['offsets']['static_list'])
    probes=[]
    for cell in ids:
        x,y,z=cell//(n*n),(cell//n)%n,cell%n
        center=[minimum[i]+(p+.5)*cell_size for i,p in enumerate((x,y,z))]
        if x%4 or y%4 or abs(center[2]+.23)>cell_size: continue
        origin=(center[0],center[1],center[2]+.5*cell_size);total=0
        for ray in range(128):
            u=(ray+.5)/128;phi=2*math.pi*((ray*.6180339887498949+.5)%1)
            direction=(math.sqrt(u)*math.cos(phi),math.sqrt(u)*math.sin(phi),math.sqrt(1-u))
            hits=[(triangle_distance(origin,direction,t),emission) for t,emission in triangles]
            distance,emission=min(hits)
            if math.isfinite(distance): total+=emission
        expected=math.pi*total/128
        actual=struct.unpack_from('<f',raw,32*(4*cells+cell))[0]
        assert math.isfinite(actual) and 0<=actual<=4*math.pi+.01
        probes.append(dict(cell=cell,triangle_irradiance=expected,voxel_irradiance=actual))
    assert len(probes)>20 and max(p['triangle_irradiance'] for p in probes)>0
    squared=sum((p['triangle_irradiance']-p['voxel_irradiance'])**2 for p in probes)
    energy=sum(p['triangle_irradiance']**2 for p in probes)
    dark=[p['voxel_irradiance'] for p in probes if p['triangle_irradiance']==0]
    result=dict(probes=len(probes),rmse=math.sqrt(squared/len(probes)),relative_rmse=math.sqrt(squared/energy),
        mean_signed_error=sum(p['voxel_irradiance']-p['triangle_irradiance'] for p in probes)/len(probes),
        dark_reference_count=len(dark),dark_reference_max=max(dark,default=None),
        samples=probes)
    Path(str(prefix)+'.triangle-reference.json').write_text(json.dumps(result,indent=2))
    return {k:v for k,v in result.items() if k!='samples'}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'build/dynamic-voxel-m7/scenes')
    parser.add_argument('--phase',choices=('all','defaults','grids','switches','triangles','sponza'),default='all')
    parser.add_argument('--quick',action='store_true')
    args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    folder=out/'fixtures';folder.mkdir(exist_ok=True);model=lifecycle.fixture(folder)
    config=ROOT/'Data/engine.cfg';original=config.read_bytes();last=original
    env=os.environ.copy();env.update(VK_LAYER_VALIDATE_SYNC='1',VK_LOADER_LAYERS_DISABLE='~implicit~',DISABLE_RTSS_LAYER='1')
    results={};images={}
    def run(tag,n=64,voxelizer='comp',thread=0,queue=0,changes=None,flags=(),frames=12,asset=model):
        nonlocal last
        settings=dict(default_model_path=asset.as_posix(),camera_position='0,0,2',voxelizer=voxelizer,
            voxel_resolution=n,voxel_reflectance_policy='averaged',voxel_reflectance_budget_mb=512,
            voxel_gi_method='dynamic_voxel',dynamic_voxel_gi_query_backend='voxel_dda',
            dynamic_voxel_gi_memory_budget_mb=3072,dynamic_voxel_gi_temporal_filter='off',
            dynamic_voxel_gi_spatial_filter='false',dynamic_voxel_gi_analytic_lighting='true',
            dynamic_voxel_gi_environment_lighting='true',dynamic_voxel_gi_emissive_lighting='true',
            environment_lighting='true',environment_intensity=1,environment_rotation_degrees=0,
            skybox_visible='false',light_count=5,voxel_gi_indirect_intensity=1,voxel_gi_shadow_enabled='true')
        settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false'})
        for i,position in enumerate(('-.25,.25,-.1','.25,.25,-.1','-.25,.25,.1','.25,.25,.1','0,.15,.25')):
            settings.update({f'light.{i}.type':'point',f'light.{i}.position':position,f'light.{i}.color':'1,1,1',
                f'light.{i}.intensity':1,f'light.{i}.range':4,f'light.{i}.enabled':'true',f'light.{i}.casts_shadows':'true'})
        settings.update(changes or {});assert config.read_bytes()==last,'Configuration changed externally'
        omitted={k.encode() for k,v in settings.items() if v is None}
        base=b''.join(line for line in original.splitlines(keepends=True) if line.partition(b'=')[0].strip() not in omitted)
        last=base+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items() if v is not None).encode();config.write_bytes(last)
        prefix=out/tag;Path(str(prefix)+'.cfg').write_bytes(last)
        command=[str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'),'--disable-rt',f'--frames={frames}','--mode=3',f'--rhi-thread={thread}',
            f'--async-compute={queue}','--width=320','--height=180','--gbuffer-size=256',f'--capture-lighting={prefix}',*flags]
        with Path(str(prefix)+'.log').open('w') as log:
            process=subprocess.run(command,cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600)
        log=Path(str(prefix)+'.log').read_text(errors='replace')
        errors=[line for line in log.splitlines() if any(t in line for t in ('[error]','VUID-','SYNC-HAZARD'))]
        assert process.returncode==0 and not errors,(tag,process.returncode,errors[:3])
        assert all('Enabled Device Extension: '+extension not in log for extension in (
            'VK_KHR_acceleration_structure','VK_KHR_ray_tracing_pipeline','VK_KHR_ray_query'))
        assert '[OK] No memory leaks detected' in log
        return prefix,log

    def analyze(prefix,method='dynamic_voxel',moving=False):
        meta,_=hdr.load_lighting(prefix);assert meta['method']==method,(prefix,meta['method'])
        assert meta['voxel_geometry_generation']==meta['scene_geometry_generation']
        stats=hdr.analyze(prefix,'fixture')
        if method=='dynamic_voxel':
            stats.update(lifecycle.analyze(prefix) if moving else static.analyze(prefix)[0])
        results[prefix.name]=stats
        print('PASS',prefix.name,method,flush=True)
        return stats

    try:
        modes=[('comp',0,0)] if args.quick else list(itertools.product(('comp','geom'),(0,1),(0,1)))
        if args.phase in ('all','defaults'):
            p,_=run('default-grid',changes={'voxel_resolution':None});analyze(p)
            assert hdr.load_lighting(p)[0]['voxel_resolution']==64
        if args.phase in ('all','grids'):
            for n in (64,128):
                for voxelizer,thread,queue in modes:
                    p,_=run(f'grid{n}-{voxelizer}-t{thread}-q{queue}',n,voxelizer,thread,queue)
                    analyze(p);digest=hashlib.sha256(Path(str(p)+'.lighting.bin').read_bytes()).hexdigest()
                    assert n not in images or images[n]==digest,'Grid voxelizer/submission mismatch'
                    images[n]=digest
                for voxelizer,thread,queue in ([('comp',0,0)] if args.quick else [('comp',0,0),('geom',1,1)]):
                    p,_=run(f'filtered{n}-{voxelizer}',n,voxelizer,thread,queue,dict(
                        dynamic_voxel_gi_temporal_filter='elapsed',dynamic_voxel_gi_spatial_filter='true'))
                    analyze(p)
            p,log=run('budget256',256,frames=2);analyze(p,'cone')
            assert 'memory preflight rejected' in log and 'cached static receivers' not in log
            q,_=run('cone256',256,changes=dict(voxel_gi_method='cone'),frames=2);analyze(q,'cone')
            assert Path(str(p)+'.lighting.bin').read_bytes()==Path(str(q)+'.lighting.bin').read_bytes()
        if args.phase in ('all','switches'):
            for voxelizer,thread,queue in modes:
                p,_=run(f'switch-{voxelizer}-t{thread}-q{queue}',voxelizer=voxelizer,thread=thread,queue=queue,
                        flags=['--gi-method-switching'])
                batches=None
                for stage in lifecycle.STAGES:
                    capture=Path(str(p)+'.'+stage);directional=stage not in ('moved','deformed','removed','camera')
                    stats=analyze(capture,'dynamic_voxel' if directional else 'cone',directional)
                    digest=hashlib.sha256(Path(str(capture)+'.lighting.bin').read_bytes()).hexdigest()
                    assert stage not in images or images[stage]==digest,'Method switching output mismatch'
                    images[stage]=digest
                    if directional:
                        assert batches is None or batches==stats['cache_batches']
                        batches=stats['cache_batches']
                assert images['initial']==images['returned']==images['restored']
        if args.phase in ('all','triangles'):
            for name,n,voxelizer in itertools.product(('emitter','thin','slanted'),(64,128),('comp',) if args.quick else ('comp','geom')):
                asset,triangles=triangle_fixture(folder,name)
                p,_=run(f'triangle-{name}-{n}-{voxelizer}',n,voxelizer,changes=dict(light_count=0,environment_lighting='false'),asset=asset)
                analyze(p);results[p.name]['triangle_error']=triangle_error(p,triangles)
        if args.phase in ('all','sponza'):
            asset=(ROOT/'../glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf').resolve();assert asset.exists()
            changes={'camera_position':'.55,-.15,0','dynamic_voxel_gi_memory_budget_mb':6144}
            for i,position in enumerate(('-.55,-.25,-.13','.55,-.25,-.13','-.55,-.25,.13','.55,-.25,.13','0,-.25,0')):
                changes.update({f'light.{i}.position':position,f'light.{i}.intensity':5,f'light.{i}.range':1000})
            for voxelizer in (('comp',) if args.quick else ('comp','geom')):
                for filtered in (False,True):
                    p,_=run(f'sponza-{voxelizer}-'+('filtered' if filtered else 'raw'),voxelizer=voxelizer,
                        thread=1,queue=1,frames=24,asset=asset,changes=changes|dict(
                            dynamic_voxel_gi_temporal_filter='elapsed' if filtered else 'off',
                            dynamic_voxel_gi_spatial_filter='true' if filtered else 'false'))
                    analyze(p)
    finally:
        assert config.read_bytes()==last,'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (out/f'{args.phase}-results.json').write_text(json.dumps(results,indent=2))
        (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__=='__main__': main()
