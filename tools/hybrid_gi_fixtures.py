"""Generate the P0 geometry/camera fixtures; binary assets stay in the requested directory."""
import argparse
import base64
import copy
import json
import math
from pathlib import Path
from voxelization_fixtures import Fixture, rgba_png

ALPHA_MATRIX = ('alpha_uv1', 'alpha_transform', 'alpha_vertex', 'alpha_specgloss',
                'alpha_repeat', 'alpha_clamp', 'alpha_mirror')
BRIGHT_EDGES = ('bright_edge_near', 'bright_edge_middle', 'bright_edge_far')
GLOSSY = ('glossy_floor','glossy_smooth','glossy_open','glossy_closed','forward_glossy_closed','glossy_emissive')


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
           'forward_open_plane','forward_closed_box','single_sided_closed_box','bright_environment_plane',
           'rotated_environment_plane','black_environment_plane','normal_map_plane'] + list(ALPHA_MATRIX) + list(BRIGHT_EDGES) + list(GLOSSY[1:])
    manifest={}
    for name in names:
        f=Fixture()
        f.document['materials']=[dict(doubleSided=True,pbrMetallicRoughness=dict(baseColorFactor=[.5,.5,.5,1],metallicFactor=0,roughnessFactor=.5))]
        f.document.pop('extensionsUsed',None)
        floor=quad([-2,0,-2],[-2,0,2],[2,0,2],[2,0,-2])
        f.mesh(floor)
        if name == 'rotated_environment_plane':
            angle = math.radians(25)
            f.document['nodes'][0]['rotation'] = [0, 0, math.sin(angle/2), math.cos(angle/2)]
        if name == 'normal_map_plane':
            f.document['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(
                rgba_png(2,2,[[218,128,218,255]]*4)).decode())]
            f.document['textures'] = [dict(source=0)]
            f.document['materials'][0]['normalTexture'] = dict(index=0)
            attributes = f.document['meshes'][0]['primitives'][0]['attributes']
            attributes['TEXCOORD_0'] = f.attribute([[0,0],[0,1],[1,1],[0,0],[1,1],[1,0]], 'VEC2')
            attributes['TANGENT'] = f.attribute([[1,0,0,-1]]*6, 'VEC4')
        if name=='single_sided_closed_box':
            # box() winds opposite faces alike, so half the walls and the ceiling face
            # outward: receivers inside see their back faces, which must still block.
            f.document['materials'][0]['doubleSided']=False
        if name in ('closed_box','point_light_room','forward_closed_box','single_sided_closed_box','glossy_closed','forward_glossy_closed'):
            walls=box([-2,0,-2],[2,3,2])
            if name in ('point_light_room','glossy_closed','forward_glossy_closed'):
                # The one-value radiance cache stores the authored owner side. This
                # room's receiver and emitter surfaces face inward; avoid duplicating
                # the floor and do not rely on raster double-sided normal flipping.
                walls=[t for t in walls if not all(p[1]==0 for p in t)]
                for t in walls:
                    a,b,c=t
                    u=[b[k]-a[k] for k in range(3)];v=[c[k]-a[k] for k in range(3)]
                    n=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
                    inward=[-sum(p[k] for p in t)/3+(1.5 if k==1 else 0) for k in range(3)]
                    if sum(n[k]*inward[k] for k in range(3))<0:t[1],t[2]=t[2],t[1]
            f.mesh(walls)
        elif name in ('glossy_floor','glossy_smooth','glossy_emissive'):
            # A finite coloured wall faces the point light and camera; its reflection has both
            # a lit interior and disocclusion edges. The previous placeholder lay through the camera.
            wall_material=copy.deepcopy(f.document['materials'][0])
            wall_material['pbrMetallicRoughness']['baseColorFactor']=[.7,.12,.03,1]
            if name=='glossy_emissive':
                wall_material['emissiveFactor']=[1,.1,.025]
                wall_material['extensions']={'KHR_materials_emissive_strength':dict(emissiveStrength=2)}
                f.document['extensionsUsed']=['KHR_materials_emissive_strength']
            f.document['materials'].append(wall_material)
            f.mesh(quad([-1,0,-1.5],[1,0,-1.5],[1,2.5,-1.5],[-1,2.5,-1.5]),material=1)
        elif name in ('half_wall','thin_wall_light'):
            wall=quad([0,0,-2],[0,3,-2],[0,3,2],[0,0,2])
            if name=='thin_wall_light':
                for t in wall:t[1],t[2]=t[2],t[1] # Face the light at negative X.
            f.mesh(wall)
        elif name=='narrow_slot':
            f.mesh(box([-2,1,-2],[-.1,1.0625,2])+box([.1,1,-2],[2,1.0625,2]))
        elif name=='thin_pole':
            f.mesh(box([-.02,1,-1],[.02,1.04,1]))
        elif name in BRIGHT_EDGES:
            f.mesh(box([-.05,0,-2],[.05,1,2]))
        elif name=='alpha_mask':
            material=copy.deepcopy(f.document['materials'][0]);material.update(alphaMode='MASK',alphaCutoff=.5)
            f.document['materials'].append(material)
            f.mesh(quad([-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]),material=1,
                   colors=[[1,1,1,0],[1,1,1,1],[1,1,1,1],[1,1,1,0],[1,1,1,1],[1,1,1,0]])
        elif name in ALPHA_MATRIX:
            texture = dict(index=0, texCoord=1 if name in ('alpha_uv1', 'alpha_specgloss') else 0)
            if name == 'alpha_transform':
                texture['extensions'] = {'KHR_texture_transform': dict(texCoord=1, offset=[.17,.31], scale=[1.4,.7], rotation=.63)}
                f.document['extensionsUsed'] = ['KHR_texture_transform']
            material = copy.deepcopy(f.document['materials'][0])
            material.update(alphaMode='MASK', alphaCutoff=.43)
            material['pbrMetallicRoughness'].update(baseColorTexture=texture, baseColorFactor=[.5,.5,.5,.8])
            if name == 'alpha_specgloss':
                material['extensions'] = {'KHR_materials_pbrSpecularGlossiness': dict(
                    diffuseTexture=texture, diffuseFactor=[.5,.5,.5,.8], specularFactor=[0,0,0], glossinessFactor=.5)}
                material['pbrMetallicRoughness'].pop('baseColorTexture')
                material['pbrMetallicRoughness']['baseColorFactor'][3] = .1
                f.document['extensionsUsed'] = ['KHR_materials_pbrSpecularGlossiness']
            f.document['materials'].append(material)
            pixels = [[255,255,255,255 if (x//2+y//2)%2 else 0] for y in range(8) for x in range(8)]
            f.document['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(rgba_png(8,8,pixels)).decode())]
            wrap = {'alpha_clamp':33071, 'alpha_mirror':33648}.get(name,10497)
            f.document['samplers'] = [dict(wrapS=wrap, wrapT=wrap, minFilter=9729, magFilter=9729)]
            f.document['textures'] = [dict(source=0,sampler=0)]
            order = (0,1,2,0,2,3)
            corners = [(-.4,-.3),(1.7,-.3),(1.7,1.6),(-.4,1.6)]
            uv0 = [corners[i] for i in order]
            uv1 = [(v*.8+.11,u*1.2-.19) for u,v in uv0]
            colors = [[1,1,1,([.2,1,.8,.4][i] if name=='alpha_vertex' else 1)] for i in order]
            f.mesh(quad([-1,1,-1],[1,1,-1],[1,1,1],[-1,1,1]),material=1,uv0=uv0,uv1=uv1,colors=colors)
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
        if name in ('point_light_room','thin_wall_light','moving_occluder_light') or name in GLOSSY and name not in ('glossy_open','glossy_emissive'):
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
        if name in GLOSSY:
            f.document['materials'][0]['pbrMetallicRoughness']['roughnessFactor']=.1 if name=='glossy_smooth' else .2
        if name.startswith('forward_'):
            f.document.setdefault('extensionsUsed',[]).append('KHR_materials_clearcoat')
            f.document['materials'][0]['extensions']={'KHR_materials_clearcoat':dict(clearcoatFactor=.5,clearcoatRoughnessFactor=.2)}
        # Fixed camera inside the box, looking obliquely down onto its floor.
        angle=math.radians(-35)
        camera=dict(camera=0,translation=[0,1.4,1.8],rotation=[math.sin(angle/2),0,0,math.cos(angle/2)])
        f.document['cameras']=[dict(type='perspective',perspective=dict(yfov=math.radians(60),znear=.001,zfar=20))]
        if name in BRIGHT_EDGES:
            distance = dict(zip(BRIGHT_EDGES, (3, 10, 30)))[name]
            angle = -math.atan2(1.4, distance)
            camera.update(translation=[0,1.4,distance], rotation=[math.sin(angle/2),0,0,math.cos(angle/2)])
            f.document['cameras'][0]['perspective'].update(yfov=2*math.atan2(2,distance), zfar=100)
        f.document['scenes'][0]['nodes'].append(len(f.document['nodes']))
        f.document['nodes'].append(camera)
        path=f.save(folder,name)
        manifest[name]=dict(scene=path.name,camera=camera,voxel_resolutions=[64,128,256])
    # A portable constant linear radiance panorama, avoiding an environment preprocessing dependency.
    (folder/'constant.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n'+bytes([128,128,128,129])*8)
    (folder/'black.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 4\n'+bytes(32))
    # A tiny, intense source exposes rare-sample noise that a constant sky cannot.
    bright = bytearray(bytes([128,128,128,124]) * (512*256))
    offset = (52*512+77)*4
    bright[offset:offset+4] = bytes([250,200,125,144])
    (folder/'bright.hdr').write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 256 +X 512\n'+bright)
    (folder/'manifest.json').write_text(json.dumps(manifest,indent=2))
    return manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    print(json.dumps(generate(parser.parse_args().output),indent=2))
