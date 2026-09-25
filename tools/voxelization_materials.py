"""Independent material expectations for the generated V0 material atlas."""
import json
import math
from pathlib import Path
import struct

ALPHA_BAND = .01  # Conservative allowance for finite texture-filter fractional precision.
COLOR_TOLERANCE = 3/255


def dot(a,b):
    return sum(x*y for x,y in zip(a,b))


def cross(a,b):
    return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])


def weights(triangle, point, clamp=True):
    # Signed projected sub-areas, independent of the shader's Gram-system solve.
    a,b,c=triangle
    n=cross([b[i]-a[i] for i in range(3)],[c[i]-a[i] for i in range(3)])
    denominator=dot(n,n)
    result=[dot(cross([p[i]-point[i] for i in range(3)],
                      [q[i]-point[i] for i in range(3)]),n)/denominator
            for p,q in ((b,c),(c,a),(a,b))] if denominator else [1,0,0]
    if clamp:
        result=[max(0,v) for v in result]
        result=[v/sum(result) for v in result]
    return result


def linear(value):
    value=value/255
    return value/12.92 if value<=.04045 else ((value+.055)/1.055)**2.4


class MaterialOracle:
    def __init__(self,prefix,metadata,triangles,contract=None):
        if contract is None:
            contract=json.loads((prefix.parent/'material-contract.json').read_text())
        self.materials,self.textures=contract['materials'],contract['textures']
        self.triangles=triangles
        vertices=Path(str(prefix)+'.vertices.bin').read_bytes()
        indices=Path(str(prefix)+'.indices.bin').read_bytes()
        records=Path(str(prefix)+'.triangles.bin').read_bytes()
        self.records=[]
        for first,_,material,_ in struct.iter_unpack('<4I',records[:metadata['triangle_count']*16]):
            attributes=[]
            for index in struct.unpack_from('<3I',indices,first*4):
                offset=index*metadata['vertex_stride']
                attributes.append((struct.unpack_from('<2f',vertices,offset+48),
                                   struct.unpack_from('<2f',vertices,offset+56),
                                   struct.unpack_from('<4f',vertices,offset+96)))
            self.records.append((material,attributes))

    def sample(self,info,uvs,srgb):
        result=[1.,1.,1.,1.]
        if info is not None:
            texture=self.textures[info['index']]
            u,v=uvs[info.get('texCoord',0)]
            x,y=u*texture['width']-.5,v*texture['height']-.5
            ix,iy=math.floor(x),math.floor(y)
            fx,fy=x-ix,y-iy
            result=[0.]*4
            for dx,dy,weight in ((0,0,(1-fx)*(1-fy)),(1,0,fx*(1-fy)),(0,1,(1-fx)*fy),(1,1,fx*fy)):
                pixel=texture['pixels'][((iy+dy)%texture['height'])*texture['width']+(ix+dx)%texture['width']]
                for i in range(4):
                    result[i]+=weight*(linear(pixel[i]) if srgb and i<3 else pixel[i]/255)
        return result

    def surface(self,record,point):
        material_id,vertices=self.records[record]
        material=self.materials[material_id]
        w=weights(self.triangles[record],point)
        attrs=[[sum(vertices[v][a][i]*w[v] for v in range(3)) for i in range(2 if a<2 else 4)]
               for a in range(3)]
        pbr=material.get('pbrMetallicRoughness',{})
        color=self.sample(pbr.get('baseColorTexture'),attrs[:2],True)
        color=[a*b*c for a,b,c in zip(color,attrs[2],pbr.get('baseColorFactor',[1]*4))]
        metal=self.sample(pbr.get('metallicRoughnessTexture'),attrs[:2],False)[2]*pbr.get('metallicFactor',1)
        strength=material.get('extensions',{}).get('KHR_materials_emissive_strength',{}).get('emissiveStrength',1)
        emission_factor=[x*strength for x in material.get('emissiveFactor',[0,0,0])]
        emission=[a*b for a,b in zip(self.sample(material.get('emissiveTexture'),attrs[:2],True),emission_factor)]
        cutoff=material.get('alphaCutoff',.5)
        masked=material.get('alphaMode','OPAQUE')!='OPAQUE'
        return dict(color=color,metal=metal,emission=emission,emission_factor=emission_factor,
                    visible=not masked or color[3]>=cutoff,
                    alpha_ambiguous=masked and abs(color[3]-cutoff)<=ALPHA_BAND,masked=masked)

    def attribute_error(self,owner,cell,color,metal,emission):
        value=self.surface(owner,[x+.5 for x in cell])
        return (max(abs(a-b) for a,b in zip(color[:3],value['color'][:3]))>COLOR_TOLERANCE or
                color[3]!=1 or abs(metal-value['metal'])>COLOR_TOLERANCE or
                any(abs(a-b)>max(.004,abs(factor)/128+abs(b)*.001)
                    for a,b,factor in zip(emission,value['emission'],value['emission_factor'])))


def compare_gbuffer(prefix,metadata,oracle,voxel_data,alpha_ambiguous):
    data=Path(str(prefix)+'.gbuffer.bin').read_bytes()
    n=metadata['resolution']
    assert len(data)==n*n*48
    matched=masked=black=mixed=ambiguous=footprint_visible=footprint_point_misses=0
    errors=[]
    mixed_difference=[0.,0.,0.]
    for index,values in enumerate(struct.iter_unpack('<4f4I4f',data)):
        px,py,pz,valid,albedo,normal,metal,footprint,er,eg,eb,_=values
        if footprint:
            footprint_visible+=1
            cell_z=math.floor((pz-metadata['grid_min'][2])/metadata['voxel_size'])
            footprint_owner=struct.unpack_from('<I',voxel_data,32*(index+n*n*cell_z))[0]
            footprint_point_misses+=int(footprint_owner==0xffffffff)
        if valid==0:
            continue
        point=[(p-metadata['grid_min'][i])/metadata['voxel_size'] for i,p in enumerate((px,py,pz))]
        cell=tuple(math.floor(p) for p in point)
        assert cell[:2]==(index%n,index//n), 'Diagnostic camera/sample alignment changed'
        if cell in alpha_ambiguous:
            ambiguous+=1
            continue
        offset=32*(cell[0]+n*(cell[1]+n*cell[2]))
        owner,va,vn,occupied,vr,vg,vb,_=struct.unpack_from('<4I4f',voxel_data,offset)
        if not occupied:
            errors.append(dict(cell=cell,reason='G-buffer surface has no voxel'))
            continue
        material_id,_=oracle.records[owner]
        if material_id in (7,9):
            # Deliberate coplanar red/blue overlap: raster draw order and lowest
            # voxel record are different policies, quantified separately.
            mixed+=1
            for i in range(3):
                mixed_difference[i]+=abs(((va>>(8*i))&255)-((albedo>>(8*i))&255))/255
            continue
        if min(weights(oracle.triangles[owner],point,False)) < -1e-3:
            continue  # Conservative edge cell's representative differs from this raster surface.
        color_error=max(abs(((va>>(8*i))&255)-((albedo>>(8*i))&255)) for i in range(3))
        normal_error=max(abs(((vn>>(8*i))&255)-((normal>>(8*i))&255)) for i in range(3))
        metallic_error=abs(((vn>>24)&255)-(metal&255))
        emission_error=max(abs(a-b) for a,b in zip((vr,vg,vb),(er,eg,eb)))
        if color_error>3 or normal_error>2 or metallic_error>3 or emission_error>.016:
            errors.append(dict(cell=cell,color_bytes=color_error,normal_bytes=normal_error,
                               metallic_bytes=metallic_error,emission=emission_error))
        matched+=1
        masked+=int(oracle.materials[material_id].get('alphaMode','OPAQUE')!='OPAQUE')
        black+=int((va&0xffffff)==0)
    assert matched>0 and masked>0 and black>0 and mixed>0, 'Material atlas comparison did not exercise every required group'
    return dict(matched=matched,masked_matched=masked,black_matched=black,
                high_resolution_footprint_columns=footprint_visible,
                footprint_columns_missed_by_point_rule=footprint_point_misses,
                alpha_ambiguous_skipped=ambiguous,mixed_surface_samples=mixed,
                mixed_mean_rgb_difference=[x/max(1,mixed) for x in mixed_difference],
                failures=len(errors),errors=errors)
