"""Generate the P0 geometry/camera fixtures; binary assets stay in the requested directory."""
import argparse
import copy
import json
import math
from pathlib import Path
from voxelization_fixtures import Fixture


def quad(a,b,c,d):
    return [[a,b,c],[a,c,d]]


def box(lo,hi):
    triangles=[]
    for axis in range(3):
        u,v=(axis+1)%3,(axis+2)%3
        for plane in (lo[axis],hi[axis]):
            corners=[]
            for x,y in ((lo[u],lo[v]),(hi[u],lo[v]),(hi[u],hi[v]),(lo[u],hi[v])):
                p=[0,0,0];p[axis]=plane;p[u]=x;p[v]=y;corners.append(p)
            triangles.extend(quad(*corners))
    return triangles


def generate(folder):
    folder.mkdir(parents=True,exist_ok=True)
    names=['open_plane','closed_box','half_wall','narrow_slot','thin_pole','alpha_mask','mirrored_two_sided',
           'outside_volume','point_light_room','thin_wall_light','glossy_floor','moving_occluder_light',
           'forward_open_plane','forward_closed_box']
    manifest={}
    for name in names:
        f=Fixture()
        f.document['materials']=[dict(doubleSided=True,pbrMetallicRoughness=dict(baseColorFactor=[.5,.5,.5,1],metallicFactor=0,roughnessFactor=.5))]
        f.document.pop('extensionsUsed',None)
        floor=quad([-2,0,-2],[-2,0,2],[2,0,2],[2,0,-2])
        f.mesh(floor)
        if name in ('closed_box','point_light_room','forward_closed_box'):
            f.mesh(box([-2,0,-2],[2,3,2]))
        elif name in ('half_wall','thin_wall_light','glossy_floor'):
            f.mesh(quad([0,0,-2],[0,3,-2],[0,3,2],[0,0,2]))
        elif name=='narrow_slot':
            f.mesh(box([-2,1,-2],[-.1,1.0625,2])+box([.1,1,-2],[2,1.0625,2]))
        elif name=='thin_pole':
            f.mesh(box([-.02,1,-1],[.02,1.04,1]))
        elif name=='alpha_mask':
            material=copy.deepcopy(f.document['materials'][0]);material.update(alphaMode='MASK',alphaCutoff=.5)
            f.document['materials'].append(material)
            f.mesh(quad([-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]),material=1,
                   colors=[[1,1,1,0],[1,1,1,1],[1,1,1,1],[1,1,1,0],[1,1,1,1],[1,1,1,0]])
        elif name=='mirrored_two_sided':
            f.mesh(box([-.2,.5,-.2],[.2,1,.2]),transform=dict(scale=[-1,1,1]))
        elif name=='outside_volume':
            f.mesh(box([-2,5,-2],[2,5.1,2]))
        elif name=='moving_occluder_light':
            f.mesh(box([-.2,.8,-.2],[.2,1,.2]))
            times=f.attribute([0,1,2],'SCALAR')
            values=f.attribute([[-1,0,0],[1,0,0],[-1,0,0]],'VEC3')
            f.document['animations']=[dict(samplers=[dict(input=times,output=values,interpolation='LINEAR')],
                                          channels=[dict(sampler=0,target=dict(node=1,path='translation'))])]
        if name in ('point_light_room','thin_wall_light','glossy_floor','moving_occluder_light'):
            f.document['extensionsUsed']=['KHR_lights_punctual']
            f.document['extensions']={'KHR_lights_punctual':dict(lights=[dict(type='point',color=[1,.25,.1],intensity=4)])}
            light=len(f.document['nodes'])
            f.document['nodes'].append(dict(translation=[-1,2,0],extensions={'KHR_lights_punctual':dict(light=0)}))
            f.document['scenes'][0]['nodes'].append(light)
            if name=='moving_occluder_light':
                values=f.attribute([[-1,2,0],[1,2,0],[-1,2,0]],'VEC3')
                animation=f.document['animations'][0]
                animation['samplers'].append(dict(input=times,output=values,interpolation='LINEAR'))
                animation['channels'].append(dict(sampler=1,target=dict(node=light,path='translation')))
        if name=='glossy_floor':
            f.document['materials'][0]['pbrMetallicRoughness']['roughnessFactor']=.2
        if name.startswith('forward_'):
            f.document['extensionsUsed']=['KHR_materials_clearcoat']
            f.document['materials'][0]['extensions']={'KHR_materials_clearcoat':dict(clearcoatFactor=.5,clearcoatRoughnessFactor=.2)}
        # Fixed camera inside the box, looking obliquely down onto its floor.
        angle=math.radians(-35)
        camera=dict(camera=0,translation=[0,1.4,1.8],rotation=[math.sin(angle/2),0,0,math.cos(angle/2)])
        f.document['cameras']=[dict(type='perspective',perspective=dict(yfov=math.radians(60),znear=.001,zfar=20))]
        f.document['scenes'][0]['nodes'].append(len(f.document['nodes']))
        f.document['nodes'].append(camera)
        path=f.save(folder,name)
        manifest[name]=dict(scene=path.name,camera=camera,voxel_resolutions=[64,128,256])
    # A portable constant linear radiance panorama, avoiding an environment preprocessing dependency.
    (folder/'constant.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n'+bytes([128,128,128,129])*8)
    (folder/'manifest.json').write_text(json.dumps(manifest,indent=2))
    return manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    print(json.dumps(generate(parser.parse_args().output),indent=2))
