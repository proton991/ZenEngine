"""Independent P0 triangle references. Run with requirements-gi-quality.txt.

The receiver file is the opt-in *.hybrid.bin capture, not an inferred raster.
Environment input is a linear level-zero cubemap .npy in Vulkan face order
(+X,-X,+Y,-Y,+Z,-Z), or a constant RGB for analytic fixtures. Never substitute
the raw skybox for the prefiltered level zero. All outputs are linear NPZ.
"""
import argparse
import base64
import io
import json
from pathlib import Path
import struct
from urllib.parse import unquote

import numpy as np
from PIL import Image
from embreex.rtcore_scene import EmbreeScene
from embreex.mesh_construction import TriangleMesh


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-30)


def basis(n):
    helper = np.zeros_like(n)
    helper[..., 2] = 1
    helper[np.abs(n[..., 2]) > .999] = [0, 1, 0]
    t = normalize(np.cross(helper, n))
    return t, np.cross(n, t)


def cosine_directions(n, count, seed=0):
    # Independent scrambled Hammersley sequence, not the GPU's R2 implementation.
    indices = np.arange(count, dtype=np.uint32)
    bits = indices.copy()
    bits = (bits << 16) | (bits >> 16)
    for shift, mask in ((1, 0x55555555), (2, 0x33333333), (4, 0x0f0f0f0f), (8, 0x00ff00ff)):
        bits = ((bits & mask) << shift) | ((bits >> shift) & mask)
    phi = 2*np.pi*np.mod(bits.astype(np.float64)/2**32 + seed*.618033988749895, 1)
    radius = np.sqrt((indices+.5)/count)
    t, b = basis(n)
    return radius[:, None]*(np.cos(phi)[:, None]*t+np.sin(phi)[:, None]*b) + np.sqrt(1-radius**2)[:, None]*n


def ggx_directions(n, view, roughness, count):
    # Independent numerical inverse-CDF sampling of the GGX VNDF projected disk.
    t, b = basis(n)
    v = np.array([np.dot(view, t), np.dot(view, b), np.dot(view, n)])
    alpha = max(roughness**2, .0016)
    v = normalize(v * [alpha, alpha, 1])
    t1 = normalize(np.array([-v[1], v[0], 0])) if np.dot(v[:2], v[:2]) else np.array([1., 0, 0])
    t2 = np.cross(v, t1)
    u = (np.arange(count)+.5)/count
    phi = 2*np.pi*np.mod(np.arange(count)*.618033988749895, 1)
    x, y = np.sqrt(u)*np.cos(phi), np.sqrt(u)*np.sin(phi)
    s = (1+v[2])/2
    y = (1-s)*np.sqrt(np.maximum(1-x*x, 0)) + s*y
    h = x[:, None]*t1 + y[:, None]*t2 + np.sqrt(np.maximum(1-x*x-y*y, 0))[:, None]*v
    h = normalize(h * [alpha, alpha, 1])
    h = h[:, :1]*t+h[:, 1:2]*b+h[:, 2:]*n
    return 2*(h@view)[:, None]*h-view


class Environment:
    def __init__(self, constant=(1, 1, 1), cube=None, rotation=0, orientation=(0, 0, 0, 1), intensity=1):
        self.constant = np.asarray(constant, dtype=float)
        self.cube = cube
        self.rotation, self.orientation, self.intensity = rotation, np.asarray(orientation), intensity

    def sample(self, directions):
        d = np.asarray(directions, dtype=float).copy()
        c, s = np.cos(self.rotation), np.sin(self.rotation)
        d[:, 0], d[:, 2] = c*d[:, 0]-s*d[:, 2], s*d[:, 0]+c*d[:, 2]
        q = self.orientation
        d += 2*np.cross(q[:3], np.cross(q[:3], d)+q[3]*d)
        if self.cube is None:
            return np.broadcast_to(self.constant*self.intensity, d.shape).copy()
        axis = np.argmax(np.abs(d), axis=1)
        faces = axis*2+(d[np.arange(len(d)), axis] < 0)
        # Vulkan cube selection, including the Y-face orientation.
        uv = np.empty((len(d), 2))
        for face in range(6):
            mask = faces == face
            x, y, z = d[mask].T
            major = np.abs(d[mask, axis[mask]])
            sc, tc = ((-z, -y), (z, -y), (x, z), (x, -z), (x, -y), (-x, -y))[face]
            uv[mask] = np.stack((sc/major, tc/major), axis=-1)*.5+.5
        size = self.cube.shape[1]
        pixel = uv*size-.5
        low = np.floor(pixel).astype(int)
        f = pixel-low
        result = np.zeros((len(d), 3))
        for dx, dy in ((0,0), (1,0), (0,1), (1,1)):
            xy = np.clip(low+[dx,dy], 0, size-1)
            weight = (f[:,0] if dx else 1-f[:,0])*(f[:,1] if dy else 1-f[:,1])
            result += self.cube[faces, xy[:,1], xy[:,0], :3]*weight[:,None]
        return result*self.intensity


class TriangleOracle:
    def __init__(self, triangles, alpha=None):
        self.triangles = np.asarray(triangles, dtype=np.float32).reshape(-1, 3, 3)
        self.scene = EmbreeScene()
        self.mesh = TriangleMesh(self.scene, self.triangles) if len(self.triangles) else None
        self.alpha = alpha

    def intersect(self, origins, directions, maximum=np.inf):
        origins = np.broadcast_to(origins, directions.shape).astype(np.float32).copy()
        directions = np.asarray(directions, dtype=np.float32)
        ids = np.full(len(directions), -1, dtype=int)
        distances = np.full(len(directions), np.inf)
        normals = np.zeros_like(directions)
        pending = np.arange(len(directions))
        current = origins.copy()
        travelled = np.zeros(len(directions))
        # Rejected alpha candidates continue through the complete scene.
        for _ in range(len(self.triangles)+1):
            if not len(pending) or self.mesh is None:
                break
            hit = self.scene.run(current[pending], directions[pending], output=1)
            found = hit['primID'] >= 0
            candidates = pending[found]
            tri = hit['primID'][found]
            t = hit['tfar'][found]
            bary = np.stack((1-hit['u'][found]-hit['v'][found], hit['u'][found], hit['v'][found]), axis=1)
            accepted = np.ones(len(tri), dtype=bool) if self.alpha is None else self.alpha(tri, bary)
            distance = travelled[candidates]+t
            within = distance < np.broadcast_to(maximum, len(directions))[candidates]
            keep = accepted & within
            chosen = candidates[keep]
            ids[chosen], distances[chosen] = tri[keep], distance[keep]
            normals[chosen] = normalize(hit['Ng'][found][keep])
            pending = candidates[~accepted & within]
            if len(pending):
                step = t[~accepted & within]+1e-6
                travelled[pending] += step
                current[pending] += directions[pending]*step[:, None]
        return ids, distances, normals


def load_gltf(path):
    """glTF/GLB triangles, alpha masks, hierarchy and the renderer's unit-cube normalization.

    Unsupported compressed or deformed assets fail explicitly rather than producing
    an apparently independent but incorrect reference.
    """
    path = Path(path)
    raw = path.read_bytes()
    binary = None
    if raw[:4] == b'glTF':
        length, kind = struct.unpack_from('<II', raw, 12)
        assert kind == 0x4e4f534a
        doc = json.loads(raw[20:20+length])
        if len(raw) > 20+length:
            count, kind = struct.unpack_from('<II', raw, 20+length)
            assert kind == 0x004e4942
            binary = raw[28+length:28+length+count]
    else:
        doc = json.loads(raw)

    def uri_bytes(uri):
        return base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else (Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:') else path.parent/unquote(uri)).read_bytes()

    buffers = [uri_bytes(b['uri']) if 'uri' in b else binary for b in doc.get('buffers', [])]
    def accessor(index):
        a = doc['accessors'][index]
        assert 'sparse' not in a, 'Sparse reference accessor is not implemented'
        view = doc['bufferViews'][a['bufferView']]
        dtype = np.dtype({5120:'i1',5121:'u1',5122:'<i2',5123:'<u2',5125:'<u4',5126:'<f4'}[a['componentType']])
        width = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        out = np.ndarray((a['count'],width),dtype,buffer=buffers[view['buffer']],
                         offset=view.get('byteOffset',0)+a.get('byteOffset',0),
                         strides=(view.get('byteStride',width*dtype.itemsize),dtype.itemsize)).copy()
        if a.get('normalized'):
            out = np.maximum(-1, out.astype(float)/np.iinfo(dtype).max)
        return out

    images = []
    for image in doc.get('images', []):
        if 'uri' in image:
            encoded = uri_bytes(image['uri'])
        else:
            v = doc['bufferViews'][image['bufferView']]
            encoded = buffers[v['buffer']][v.get('byteOffset',0):v.get('byteOffset',0)+v['byteLength']]
        images.append(np.asarray(Image.open(io.BytesIO(encoded)).convert('RGBA'),dtype=float)/255)

    triangles, masks = [], []
    materials = doc.get('materials', [{}])
    def visit(index, parent):
        node = doc['nodes'][index]
        assert 'skin' not in node and 'weights' not in node, 'Capture deformed geometry for animated references'
        if 'matrix' in node:
            local = np.array(node['matrix']).reshape(4,4).T
        else:
            x,y,z,w = node.get('rotation',[0,0,0,1])
            local = np.eye(4)
            local[:3,:3] = [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                            [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                            [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
            local[:3,:3] *= node.get('scale',[1,1,1])
            local[:3,3] = node.get('translation',[0,0,0])
        world = parent@local
        if 'mesh' in node:
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                assert primitive.get('mode',4) == 4 and 'targets' not in primitive
                assert 'KHR_draco_mesh_compression' not in primitive.get('extensions',{})
                attrs = primitive['attributes']
                pos = accessor(attrs['POSITION'])
                ids = accessor(primitive['indices']).ravel() if 'indices' in primitive else np.arange(len(pos))
                ids = ids.reshape(-1,3)
                points = pos@world[:3,:3].T+world[:3,3]
                material = materials[primitive.get('material',0)]
                pbr = material.get('pbrMetallicRoughness',{})
                specular_glossiness = material.get('extensions',{}).get('KHR_materials_pbrSpecularGlossiness')
                if specular_glossiness:
                    pbr = dict(baseColorFactor=specular_glossiness.get('diffuseFactor',[1,1,1,1]),
                               baseColorTexture=specular_glossiness.get('diffuseTexture'))
                texture = pbr.get('baseColorTexture')
                colors = accessor(attrs['COLOR_0']) if 'COLOR_0' in attrs else np.ones((len(pos),4))
                alpha = colors[:,3] if colors.shape[1] == 4 else np.ones(len(pos))
                uv = np.zeros((len(pos),2))
                if texture:
                    transform = texture.get('extensions',{}).get('KHR_texture_transform',{})
                    channel = transform.get('texCoord',texture.get('texCoord',0))
                    uv = accessor(attrs[f'TEXCOORD_{channel}'])
                    c,s = np.cos(transform.get('rotation',0)),np.sin(transform.get('rotation',0))
                    uv = (uv*transform.get('scale',[1,1]))@np.array([[c,s],[-s,c]])+transform.get('offset',[0,0])
                for tri in ids:
                    triangles.append(points[tri])
                    masks.append((material.get('alphaMode') == 'MASK', material.get('alphaCutoff',.5),
                                  pbr.get('baseColorFactor',[1,1,1,1])[3], texture, uv[tri], alpha[tri]))
        for child in node.get('children',[]):
            visit(child,world)
    for node in doc['scenes'][doc.get('scene',0)]['nodes']:
        visit(node,np.eye(4))
    triangles = np.array(triangles)
    lo,hi = triangles.min(axis=(0,1)),triangles.max(axis=(0,1))
    extent = max(hi-lo)
    triangles = (triangles-(lo+hi)/2)/extent

    def accept(ids,bary):
        result = np.ones(len(ids),dtype=bool)
        for i,(index,weights) in enumerate(zip(ids,bary)):
            masked, cutoff, factor, texture, uv, color = masks[index]
            if masked:
                value = factor*np.dot(weights,color)
                if texture:
                    tex = doc['textures'][texture['index']]
                    image = images[tex['source']]
                    sampler = doc.get('samplers',[{}])[tex['sampler']] if 'sampler' in tex else {}
                    coordinate = weights@uv
                    for axis,key in enumerate(('wrapS','wrapT')):
                        wrap = sampler.get(key,10497)
                        coordinate[axis] = np.clip(coordinate[axis],0,1) if wrap == 33071 else (
                            1-abs(coordinate[axis]%2-1) if wrap == 33648 else coordinate[axis]%1)
                    pixel = coordinate*np.array([image.shape[1],image.shape[0]])-.5
                    low = np.floor(pixel).astype(int); f = pixel-low
                    sample = 0
                    for dx,dy in ((0,0),(1,0),(0,1),(1,1)):
                        indices = low + [dx,dy]
                        for axis,key in enumerate(('wrapS','wrapT')):
                            size = image.shape[1-axis]
                            wrap = sampler.get(key,10497)
                            if wrap == 33071:
                                indices[axis] = np.clip(indices[axis],0,size-1)
                            elif wrap == 33648:
                                index = indices[axis] % (2*size)
                                indices[axis] = min(index,2*size-1-index)
                            else:
                                indices[axis] %= size
                        sample += image[indices[1],indices[0],3]*(f[0] if dx else 1-f[0])*(f[1] if dy else 1-f[1])
                    value *= sample
                result[i] = value >= cutoff
        return result
    return TriangleOracle(triangles,accept), dict(center=((lo+hi)/2).tolist(),scale=1/extent)


def sky_reference(oracle, position, shading, geometric, environment, samples=4096):
    directions = cosine_directions(shading,samples)
    ids,_,_ = oracle.intersect(position+geometric*1e-5,directions)
    visible = (ids < 0) & (directions@geometric > 0)
    return np.mean(environment.sample(directions)*visible[:,None],axis=0), np.mean(visible)


def smith_g1(cosine, roughness):
    alpha2 = max(roughness**2, .0016)**2
    c = np.maximum(cosine, 0)
    return np.where(cosine > 0, 2*c/(c+np.sqrt(alpha2+(1-alpha2)*c*c)), 0.)


def specular_reference(oracle, position, shading, geometric, view, roughness, samples=4096):
    # S is the escaped part of the lobe f*cos over the upper hemisphere of the shading normal,
    # the domain of the prefiltered map and split-sum terms. Visible-normal samples carry the
    # weight f*cos/pdf = G1(l) (separable Smith, Fresnel excluded); below the surface it is zero.
    directions = ggx_directions(shading,view,roughness,samples)
    weight = smith_g1(directions@shading,roughness)
    ids,_,_ = oracle.intersect(position+geometric*1e-5,directions)
    escaped = (ids < 0) & (directions@geometric > 0)
    return float(np.sum(weight*escaped)/max(np.sum(weight),1e-30))


def bounce_reference(oracle, position, normal, environment, light_position, light_intensity, albedo=.5, samples=4096):
    directions = cosine_directions(normal,samples)
    ids,distances,normals = oracle.intersect(position+normal*1e-5,directions)
    result = np.zeros((samples,3))
    for i in np.flatnonzero(ids >= 0):
        point = position+normal*1e-5+directions[i]*distances[i]
        n = normals[i] if np.dot(normals[i],-directions[i]) > 0 else -normals[i]
        delta = np.asarray(light_position)-point
        distance = np.linalg.norm(delta)
        light = delta/distance
        blocker,_,_ = oracle.intersect(point+n*1e-5,light[None,:],distance-2e-5)
        direct = np.asarray(light_intensity)*max(np.dot(n,light),0)/distance**2 if blocker[0]<0 else np.zeros(3)
        sky,_ = sky_reference(oracle,point,n,n,environment,256)
        result[i] = albedo*(direct/np.pi+sky)
    return result.mean(axis=0)


def self_test():
    n = np.array([0.,1,0]); env = Environment()
    empty = TriangleOracle([])
    value,nu = sky_reference(empty,np.zeros(3),n,n,env)
    np.testing.assert_allclose(value,1,atol=1e-10); assert nu==1
    # Huge wall through the receiver partitions the hemisphere into equal halves.
    wall = TriangleOracle([[[0,-100,-100],[0,100,-100],[0,100,100]],[[0,-100,-100],[0,100,100],[0,-100,100]]])
    value,_ = sky_reference(wall,np.array([-1e-5,0,0]),n,n,env)
    np.testing.assert_allclose(value,.5,atol=.0025)
    ceiling = TriangleOracle([[[-100,1,-100],[100,1,-100],[100,1,100]],[[-100,1,-100],[100,1,100],[-100,1,100]]])
    value,nu = sky_reference(ceiling,np.zeros(3),n,n,env)
    assert nu==0 and np.max(value)==0
    directions = ggx_directions(n,n,.04,4096)
    assert np.mean(directions@n>0) >= .995
    bounce = bounce_reference(empty,np.zeros(3),n,env,[0,1,0],[1,1,1])
    assert np.max(bounce)==0
    bounce = bounce_reference(ceiling,np.zeros(3),n,env,[0,0,0],[0,0,0],albedo=.5)
    np.testing.assert_allclose(bounce,.5,atol=.0025)
    corners = np.array([[x,y,z] for x in (-1,1) for y in (-1,1) for z in (-1,1)],float)
    faces = [(0,1,3,2),(4,6,7,5),(0,4,5,1),(2,3,7,6),(0,2,6,4),(1,5,7,3)]
    box = TriangleOracle([corners[[a,b,c]] for a,b,c,d in faces]+[corners[[a,c,d]] for a,b,c,d in faces])
    for roughness in (.2,.5,1.0):
        directions = ggx_directions(n,n,roughness,65536)
        np.testing.assert_allclose(np.mean(directions@n>0),1/(1+roughness**4),atol=2/65536)
        # The below-horizon part of the lobe above is outside the integral: open surfaces keep S=1.
        for angle in (0,60,85):
            view = np.array([np.sin(np.radians(angle)),np.cos(np.radians(angle)),0])
            assert specular_reference(empty,np.zeros(3),n,n,view,roughness) == 1
            assert specular_reference(box,np.zeros(3),n,n,view,roughness) == 0
    # Reject the near mask and hit the later opaque triangle.
    masked = TriangleOracle(np.concatenate((ceiling.triangles,ceiling.triangles+[0,1,0])),lambda ids,b:ids>=2)
    ids,d,_ = masked.intersect(np.zeros(3),np.array([[0,1,0]],float))
    assert ids[0]>=2 and abs(d[0]-2)<1e-5
    return dict(open_plane='pass',half_wall='pass',closed_hemisphere='pass',ggx_normal_incidence='pass',
                empty_bounce='pass',uniform_bounce='pass',ggx_lobe_fraction='pass',open_closed_specular='pass',
                mask_continue='pass')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test',action='store_true')
    parser.add_argument('--scene',type=Path)
    parser.add_argument('--capture',type=Path)
    parser.add_argument('--environment-cube',type=Path)
    parser.add_argument('--constant-environment',type=float,nargs=3)
    parser.add_argument('--samples',type=int,default=4096)
    parser.add_argument('--stride',type=int,default=16)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--bounce',action='store_true',help='Also evaluate the uniform-albedo one-bounce oracle')
    parser.add_argument('--bounce-albedo',type=float,default=.5)
    parser.add_argument('--light-position',type=float,nargs=3,default=[0,1,0],help='Normalized renderer world coordinates')
    parser.add_argument('--light-intensity',type=float,nargs=3,default=[0,0,0],help='Point light RGB intensity before inverse-square falloff')
    args = parser.parse_args()
    if args.self_test:
        print(json.dumps(self_test(),indent=2))
    else:
        assert args.samples>=4096 and args.stride>0
        metadata = json.loads(Path(str(args.capture)+'.lighting.json').read_text())
        capture = np.fromfile(str(args.capture)+'.hybrid.bin',dtype='<f4').reshape(metadata['height'],metadata['width'],13,4)
        oracle, normalization = load_gltf(args.scene)
        e = metadata['environment_intensity_rotation_enabled_visible']
        cube = None
        if args.constant_environment is None:
            cube = np.load(args.environment_cube) if args.environment_cube else np.fromfile(
                str(args.capture)+'.environment.bin',dtype='<f4').reshape(6,metadata['environment_cube_size'],metadata['environment_cube_size'],4)
        environment = Environment(args.constant_environment or [1,1,1],cube,
                                  e[1],metadata.get('environment_orientation',[0,0,0,1]),e[0]*e[2])
        pixels,sky,nu,specular,bounce = [],[],[],[],[]
        for y in range(0,metadata['height'],args.stride):
            for x in range(0,metadata['width'],args.stride):
                r = capture[y,x]
                if r[8,3] == 0: continue
                d,v = sky_reference(oracle,r[8,:3],r[9,:3],r[10,:3],environment,args.samples)
                view = normalize(np.array(metadata['camera_position'])-r[8,:3])
                pixels.append([x,y]); sky.append(d); nu.append(v)
                specular.append(specular_reference(oracle,r[8,:3],r[9,:3],r[10,:3],view,r[9,3],args.samples))
                if args.bounce:
                    bounce.append(bounce_reference(oracle,r[8,:3],r[10,:3],environment,args.light_position,
                                                   args.light_intensity,args.bounce_albedo,args.samples))
        np.savez_compressed(args.output,pixels=pixels,sky=sky,nu=nu,S=specular,hit_fraction=1-np.array(specular),
                            bounce=bounce,samples=args.samples,normalization=json.dumps(normalization))


if __name__ == '__main__':
    main()
