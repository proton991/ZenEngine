"""Deterministic raw-volume calibration assets (generated under build only)."""

import json
import math
import struct
import base64
import zlib


class Fixture:
    def __init__(self):
        self.data = bytearray()
        self.document = dict(asset={'version': '2.0'}, bufferViews=[], accessors=[],
            meshes=[], nodes=[], scenes=[dict(nodes=[])], scene=0,
            materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.2, .6, .8, 1],
                metallicFactor=.25, roughnessFactor=1), emissiveFactor=[1, .5, .25],
                extensions={'KHR_materials_emissive_strength': {'emissiveStrength': 4}})],
            extensionsUsed=['KHR_materials_emissive_strength'])

    def attribute(self, values, kind, component=5126):
        start = len(self.data)
        flat = [x for row in values for x in (row if isinstance(row, (list, tuple)) else (row,))]
        self.data.extend(struct.pack('<'+('f' if component == 5126 else 'I')*len(flat), *flat))
        self.document['bufferViews'].append(dict(buffer=0, byteOffset=start, byteLength=len(self.data)-start))
        accessor = dict(bufferView=len(self.document['bufferViews'])-1, componentType=component,
                        count=len(values), type=kind)
        if kind == 'VEC3':
            accessor.update(min=[min(p[i] for p in values) for i in range(3)],
                            max=[max(p[i] for p in values) for i in range(3)])
        self.document['accessors'].append(accessor)
        return len(self.document['accessors'])-1

    def mesh(self, triangles, material=0, transform=None, uv0=None, uv1=None, colors=None):
        points = [p for triangle in triangles for p in triangle]
        normals = []
        for a,b,c in triangles:
            u, v = [b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)]
            n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
            length = math.sqrt(sum(x*x for x in n))
            n = tuple(x/length for x in n) if length else (0., 0., 1.)
            normals.extend([n]*3)
        attrs = dict(POSITION=self.attribute(points, 'VEC3'), NORMAL=self.attribute(normals, 'VEC3'))
        for semantic, values, kind in (('TEXCOORD_0', uv0, 'VEC2'), ('TEXCOORD_1', uv1, 'VEC2'),
                                        ('COLOR_0', colors, 'VEC4')):
            if values is not None:
                assert len(values) == len(points)
                attrs[semantic] = self.attribute(values, kind)
        indices = self.attribute(list(range(len(points))), 'SCALAR', 5125)
        self.document['meshes'].append(dict(primitives=[dict(attributes=attrs, indices=indices, material=material)]))
        node = dict(mesh=len(self.document['meshes'])-1)
        node.update(transform or {})
        self.document['nodes'].append(node)
        self.document['scenes'][0]['nodes'].append(len(self.document['nodes'])-1)

    def save(self, folder, name):
        self.document['buffers'] = [dict(uri=name+'.bin', byteLength=len(self.data))]
        (folder/(name+'.bin')).write_bytes(self.data)
        path = folder/(name+'.gltf')
        path.write_text(json.dumps(self.document), encoding='utf-8')
        return path


def geometry_fixture(folder, dimension):
    fixture = Fixture()
    # Exact power-of-two unit bounds: the 100% calibration grid has an identity
    # normalized scene transform and exactly representable cell boundaries.
    fixture.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])
    triangles = []

    def point(p):
        return tuple(x/dimension-.5 for x in p)

    def add(a,b,c):
        triangles.append([point(a),point(b),point(c)])

    # Closed box: twelve boundary triangles, not a filled solid.
    low, high = (10,11,12), (20,22,25)
    for axis in range(3):
        x,y = (axis+1)%3, (axis+2)%3
        for depth in (low[axis], high[axis]):
            corners=[]
            for u,v in ((low[x],low[y]), (high[x],low[y]), (high[x],high[y]), (low[x],high[y])):
                p=[0,0,0]; p[axis],p[x],p[y]=depth,u,v; corners.append(p)
            add(*corners[:3]); add(corners[0],corners[2],corners[3])
    # Tessellated plane, shared edges, axis ties, four-cell closed depth endpoints.
    for x in range(27,39,2):
        for y in range(9,21,2):
            add((x,y,30), (x+2,y,30), (x+2,y+2,30))
            add((x,y,30), (x+2,y+2,30), (x,y+2,30))
    add((25,25,40), (33,25,32), (25,33,32))
    # Near-degenerate sliver, exact collinearity, and outer face/edge/corner contact.
    add((5,40,20), (50,40,20), (50,40+2**-16,20))
    add((8,40,22), (20,40,22), (40,40,22))
    add((0,0,0), (0,dimension,0), (0,0,dimension))
    add((dimension,dimension,dimension), (dimension,dimension-4,dimension),
        (dimension,dimension,dimension-4))
    add((3,45,5), (dimension-3,45,dimension-5), (dimension-3,46,dimension-5))
    fixture.mesh(triangles)
    # Same local mesh with independent nonuniform and negative-scale instances.
    local = [[(-.06,-.04,-.02),(.06,-.04,.02),(-.06,.04,.02)]]
    fixture.mesh(local, transform=dict(translation=[.2,.2,.1],scale=[.7,1.7,1.1]))
    fixture.mesh(local, transform=dict(translation=[-.2,.2,-.1],scale=[-1.3,.7,1.1]))
    return fixture.save(folder, 'geometry')


def rgba_png(width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    rows=b''.join(b'\0'+bytes(channel for pixel in pixels[y*width:(y+1)*width] for channel in pixel)
                  for y in range(height))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',width,height,8,6,0,0,0))+ \
        chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')


def material_fixture(folder, dimension, reverse=False):
    fixture=Fixture()
    fixture.mesh([[(-.5,)*3]*3,[(.5,)*3]*3])
    textures=[dict(width=8,height=8,pixels=[[32+x*28,48+y*24,64+(x+y)%4*48,255]
                                          for y in range(8) for x in range(8)]),
              dict(width=8,height=8,pixels=[[180,80,140,255 if (x+y)%2 else 0]
                                          for y in range(8) for x in range(8)]),
              dict(width=8,height=8,pixels=[[128,96,32+x*28,255]
                                          for y in range(8) for x in range(8)])]
    fixture.document['images']=[dict(uri='data:image/png;base64,'+base64.b64encode(
        rgba_png(t['width'],t['height'],t['pixels'])).decode()) for t in textures]
    fixture.document['textures']=[dict(source=i) for i in range(len(textures))]
    mats=fixture.document['materials']
    mats.extend([
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,0,1], metallicFactor=0)),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[.4,.8,.6,1], metallicFactor=.7)),
        dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0,texCoord=0),
             baseColorFactor=[.8,.6,.9,1],metallicFactor=.3)),
        dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=0,texCoord=1),metallicFactor=.5)),
        dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=1,texCoord=0),
             baseColorFactor=[.8,.8,.8,.9],metallicFactor=.4),alphaMode='MASK',alphaCutoff=.43),
        dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=1,texCoord=1),
             metallicRoughnessTexture=dict(index=2,texCoord=0),metallicFactor=.8),
             alphaMode='MASK',alphaCutoff=.43,emissiveFactor=[1,.5,.25],
             emissiveTexture=dict(index=0,texCoord=1),
             extensions={'KHR_materials_emissive_strength':{'emissiveStrength':8}}),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[1,0,0,1],metallicFactor=.1)),
        dict(pbrMetallicRoughness=dict(baseColorTexture=dict(index=2,texCoord=0),
             metallicRoughnessTexture=dict(index=2,texCoord=1),metallicFactor=.8),
             emissiveFactor=[.25,.5,1],emissiveTexture=dict(index=0,texCoord=1),
             extensions={'KHR_materials_emissive_strength':{'emissiveStrength':8}}),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0,0,1,1],metallicFactor=.9))])
    order=(0,1,2,0,2,3)
    uv_corners=[(.0625,.0625),(1.5625,.0625),(1.5625,1.5625),(.0625,1.5625)]
    colors=[(.2,.7,.3,.4),(.8,.4,.6,1),(.9,.9,.4,.7),(.5,.2,.9,1)]
    for tile in range(9):
        x,y=(tile%3-1)*.31,(tile//3-1)*.31
        points=[(x-.13,y-.13,-.247),(x+.13,y-.13,-.247),
                (x+.13,y+.13,-.247),(x-.13,y+.13,-.247)]
        triangles=[[points[i] for i in order[:3]],[points[i] for i in order[3:]]]
        uv0=[uv_corners[i] for i in order]
        uv1=[(v*1.2+.17,u*.8+.13) for u,v in uv0]
        if tile==4:
            uv0=[(.0625,.0625)]*6
        vertex_colors=[colors[i] for i in order] if tile in (2,6) else [(1,1,1,1)]*6
        fixture.mesh(triangles,tile,uv0=uv0,uv1=uv1,colors=vertex_colors)
        if tile==7:
            # Repeated same-material records and a competing material at identical depth.
            fixture.mesh(triangles,tile,uv0=uv0,uv1=uv1,colors=vertex_colors)
            fixture.mesh(triangles,9,uv0=uv0,uv1=uv1,colors=vertex_colors)
    if reverse:
        fixture.document['nodes'].reverse()
    path=fixture.save(folder,'materials')
    (folder/'material-contract.json').write_text(json.dumps(dict(materials=mats,textures=textures)))
    return path
