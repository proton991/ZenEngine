"""Extract the measured New Sponza lantern for test-only native GPU field qualification.

Requires NumPy and Pillow. No Mitsuba renderer or GPU execution is used.
Outputs original material coverage, canonical triangles, selection evidence and hashes;
all distance-field samples are constructed by the engine GPU tests.
"""

import argparse
from pathlib import Path
import hashlib
import json
import sys
import struct
from collections import Counter
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ground_truth_mitsuba import load_gltf_primitives

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, help='Original NewSponza_Main_glTF_003.gltf')
parser.add_argument('--output', type=Path, default=ROOT / 'build/compute-gi/brixelizer-execution/newsponza-lantern')
args = parser.parse_args()
SOURCE = args.source.resolve()
OUT = args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
expected_source = 'e04c4c540c74bdddcbd3f590a85c14119bfd5839702246a23b3a737c0cce2400'
if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != expected_source:
    parser.error('Source hash differs from the inspected New Sponza fixture; inspect/requalify before changing this pin')
doc = json.loads(SOURCE.read_text())
buffer_data = [(SOURCE.parent / b['uri']).read_bytes() for b in doc['buffers']]
expected_buffer = '903d443b46b51c8397c25b65286d9f9e111121f12a3e0e303f19019982568d6b'
if len(buffer_data) != 1 or hashlib.sha256(buffer_data[0]).hexdigest() != expected_buffer:
    parser.error('Binary buffer hash differs from the inspected New Sponza fixture')

def accessor(index):
    a = doc['accessors'][index]
    view = doc['bufferViews'][a['bufferView']]
    dtype = np.dtype({5120:'i1',5121:'u1',5122:'<i2',5123:'<u2',5125:'<u4',5126:'<f4'}[a['componentType']])
    width = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4}[a['type']]
    values = np.ndarray((a['count'],width), dtype, buffer=buffer_data[view['buffer']],
        offset=view.get('byteOffset',0)+a.get('byteOffset',0),
        strides=(view.get('byteStride',width*dtype.itemsize),dtype.itemsize)).astype(float)
    if a.get('normalized'):
        values = np.maximum(-1,values/np.iinfo(dtype).max)
    return values

def node_matrix(node):
    if 'matrix' in node:
        return np.array(node['matrix']).reshape(4,4).T
    x,y,z,w=node.get('rotation',[0,0,0,1])
    m=np.eye(4)
    m[:3,:3]=[[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
               [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
               [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
    m[:3,:3]*=np.array(node.get('scale',[1,1,1]))[None,:]
    m[:3,3]=node.get('translation',[0,0,0])
    return m

metadata=[]
lights=[]
world_matrices={}
def visit(index,parent):
    node=doc['nodes'][index]
    world=parent@node_matrix(node)
    world_matrices[index]=world
    if 'mesh' in node:
        mesh=doc['meshes'][node['mesh']]
        for primitive_index,p in enumerate(mesh['primitives']):
            metadata.append({'node':index,'node_name':node.get('name'),'mesh':node['mesh'],
                             'mesh_name':mesh.get('name'),'primitive':primitive_index,'material':p.get('material',-1)})
    light=node.get('extensions',{}).get('KHR_lights_punctual',{}).get('light')
    if light is not None:
        lights.append({'index':light,'node':index,'name':node.get('name'),'position':world[:3,3].tolist()})
    for child in node.get('children',[]):visit(child,world)
for node in doc['scenes'][doc.get('scene',0)]['nodes']:visit(node,np.eye(4))

primitives,alpha_image,normalization=load_gltf_primitives(SOURCE)
alpha_records=[]
inert_materials=set()
for material_index,material in enumerate(doc['materials']):
    if material.get('alphaMode','OPAQUE')=='OPAQUE':continue
    pbr=material.get('pbrMetallicRoughness',{})
    tex=pbr.get('baseColorTexture')
    tex_alpha=alpha_image(tex)[0] if tex else np.ones((1,1,1))
    vertex_min=1.;vertex_max=0.;count=0;triangles=0
    for meta in metadata:
        if meta['material']!=material_index:continue
        p=doc['meshes'][meta['mesh']]['primitives'][meta['primitive']]
        colors=accessor(p['attributes']['COLOR_0']) if 'COLOR_0' in p['attributes'] else None
        values=colors[:,3] if colors is not None and colors.shape[1]==4 else np.ones(1)
        vertex_min=min(vertex_min,float(values.min()));vertex_max=max(vertex_max,float(values.max()))
        count+=1;triangles+=doc['accessors'][p['indices']]['count']//3
    factor=pbr.get('baseColorFactor',[1,1,1,1])[3]
    cutoff=material.get('alphaCutoff',.5) if material.get('alphaMode')=='MASK' else .5
    maximum=factor*vertex_max*float(tex_alpha.max())
    if maximum<cutoff:inert_materials.add(material_index)
    alpha_records.append({'material':material_index,'name':material.get('name'),'mode':material.get('alphaMode'),
        'factor':factor,'cutoff':cutoff,'texture_alpha_min':float(tex_alpha.min()),'texture_alpha_max':float(tex_alpha.max()),
        'texture_shape':list(tex_alpha.shape),'vertex_alpha_min':vertex_min,'vertex_alpha_max':vertex_max,
        'maximum_coverage':maximum,'primitive_instances':count,'triangles':triangles,'partial_coverage':maximum>=cutoff})
metadata=[m for m in metadata if m['material'] not in inert_materials]
assert len(metadata)==len(primitives)
center=np.array(normalization['center']);scale=float(normalization['scale'])
positions=[p['positions']/scale+center for p in primitives]
bounds_min=np.array([v[p['faces'].ravel()].min(axis=0) for p,v in zip(primitives,positions)])
bounds_max=np.array([v[p['faces'].ravel()].max(axis=0) for p,v in zip(primitives,positions)])

def closest_to_triangles(point,triangles):
    a,b,c=triangles[:,0],triangles[:,1],triangles[:,2]
    ab=b-a;ac=c-a;ap=point-a
    d00=np.einsum('ij,ij->i',ab,ab);d01=np.einsum('ij,ij->i',ab,ac);d11=np.einsum('ij,ij->i',ac,ac)
    d20=np.einsum('ij,ij->i',ap,ab);d21=np.einsum('ij,ij->i',ap,ac)
    det=d00*d11-d01*d01
    safe=np.where(det>1e-25,det,1.)
    u=(d11*d20-d01*d21)/safe;v=(d00*d21-d01*d20)/safe
    projection=a+u[:,None]*ab+v[:,None]*ac
    squared=np.einsum('ij,ij->i',projection-point,projection-point)
    valid=(det>1e-25)&(u>=0)&(v>=0)&(u+v<=1)
    squared=np.where(valid,squared,np.inf)
    nearest=projection.copy()
    for start,end in [(a,b),(b,c),(c,a)]:
        edge=end-start
        length=np.einsum('ij,ij->i',edge,edge)
        t=np.clip(np.einsum('ij,ij->i',point-start,edge)/np.maximum(length,1e-30),0,1)
        q=start+t[:,None]*edge
        d=np.einsum('ij,ij->i',q-point,q-point)
        replace=d<squared;squared=np.minimum(squared,d);nearest[replace]=q[replace]
    index=int(np.argmin(squared))
    return float(np.sqrt(squared[index])),nearest[index],index

chosen_light=next(l for l in lights if l['index']==8)
point=np.array(chosen_light['position'])
aabb_distance=np.linalg.norm(np.maximum(np.maximum(bounds_min-point,point-bounds_max),0),axis=1)
near=[]
for i in np.flatnonzero(aabb_distance<1.0):
    distance,nearest,triangle=closest_to_triangles(point,positions[i][primitives[i]['faces']])
    near.append({'primitive_global':int(i),**metadata[i],'distance':distance,'nearest':nearest.tolist(),
        'triangle':triangle,'bounds_min':bounds_min[i].tolist(),'bounds_max':bounds_max[i].tolist(),
        'triangles':len(primitives[i]['faces'])})
near.sort(key=lambda p:p['distance'])
selected_mesh=near[0]['mesh'];selected_node=near[0]['node']
selected=[i for i,m in enumerate(metadata) if m['mesh']==selected_mesh and m['node']==selected_node]
arrays={}
for i in selected:
    arrays[f'positions_{i}']=positions[i]
    arrays[f'faces_{i}']=primitives[i]['faces']
    arrays[f'normals_{i}']=primitives[i]['normals']
np.savez_compressed(OUT/'lantern-triangles.npz',**arrays)

fixture=json.loads(SOURCE.read_text())
fixture['scenes']=[{'nodes':[0]}];fixture['scene']=0
fixture['nodes']=[{'name':'MeasuredLanternFixture','mesh':0,'matrix':world_matrices[selected_node].T.ravel().tolist()}]
fixture['meshes']=[doc['meshes'][selected_mesh]]
fixture.pop('cameras',None);fixture.pop('extensions',None)
for b in fixture['buffers']:b['uri']=(SOURCE.parent/b['uri']).resolve().as_uri()
for im in fixture.get('images',[]):
    if 'uri' in im:im['uri']=(SOURCE.parent/im['uri']).resolve().as_uri()
# Uniform materials remove irrelevant texture loading; all three lantern materials are opaque.
for m in fixture['materials']:
    old=m.get('pbrMetallicRoughness',{});alpha=old.get('baseColorFactor',[1,1,1,1])[3]
    m['pbrMetallicRoughness']={'baseColorFactor':[.5,.5,.5,alpha],'metallicFactor':0,'roughnessFactor':1}
    for key in ['normalTexture','occlusionTexture','emissiveTexture']:m.pop(key,None)
fixture.pop('images',None);fixture.pop('textures',None);fixture.pop('samplers',None)
(OUT/'lantern-uniform.gltf').write_text(json.dumps(fixture,indent=2)+'\n')

selected_bounds=[bounds_min[selected].min(axis=0),bounds_max[selected].max(axis=0)]
fixture_center=(selected_bounds[0]+selected_bounds[1])*.5
fixture_scale=1/float(np.max(selected_bounds[1]-selected_bounds[0]))
fixture_light=(point-fixture_center)*fixture_scale
native=bytearray(struct.pack('<4I4f',0x5a4c414e,1,sum(len(primitives[i]['faces']) for i in selected),len(selected),
    *fixture_light,1/fixture_scale))
for i in selected:
    material=doc['materials'][metadata[i]['material']]
    mode={'OPAQUE':0,'MASK':1,'BLEND':2}[material.get('alphaMode','OPAQUE')]
    native.extend(struct.pack('<4I2f',len(primitives[i]['faces']),metadata[i]['material'],mode,int(material.get('doubleSided',False)),
        material.get('pbrMetallicRoughness',{}).get('baseColorFactor',[1,1,1,1])[3],material.get('alphaCutoff',.5)))
for i in selected:
    vertices=(positions[i][primitives[i]['faces']].reshape(-1,3)-fixture_center)*fixture_scale
    native.extend(vertices.astype('<f4').tobytes())
expected_native = '2c3132aa5ce7a9f6de9cbd73f7a89444051e72deafb297b85a2bc4af7fb6342d'
if hashlib.sha256(native).hexdigest() != expected_native:
    parser.error('Generated canonical fixture differs from the native fixture pin')
(OUT/'lantern-native.bin').write_bytes(native)
report={'source':str(SOURCE),'source_sha256':hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
    'buffer_sha256':hashlib.sha256(buffer_data[0]).hexdigest(),'normalization':normalization,
    'authored_extent':1/scale,'material_alpha_counts':dict(Counter(m.get('alphaMode','OPAQUE') for m in doc['materials'])),
    'alpha_records':alpha_records,'active_primitive_count':len(primitives),'active_triangles':sum(len(p['faces']) for p in primitives),
    'selected_light':chosen_light,'selected_light_normalized':((point-center)*scale).tolist(),
    'selected_mesh':selected_mesh,'selected_node':selected_node,'selected_primitive_indices':selected,
    'selected_bounds':[b.tolist() for b in selected_bounds],'selected_normalized_bounds':[((b-center)*scale).tolist() for b in selected_bounds],
    'fixture_normalization':{'center':fixture_center.tolist(),'scale':fixture_scale},
    'fixture_light_normalized':((point-fixture_center)*fixture_scale).tolist(),
    'native_fixture_sha256':hashlib.sha256(native).hexdigest(),'native_fixture_bytes':len(native),
    'near_light_half_metre_bounds':[(point-.5).tolist(),(point+.5).tolist()],
    'nearest_primitive_records':near,'selected_materials':[{'index':metadata[i]['material'],**doc['materials'][metadata[i]['material']]} for i in selected]}
(OUT/'inspection.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
