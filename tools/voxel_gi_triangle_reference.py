"""Independent CPU triangle integration for captured Stage A diffuse images.

Uses source glTF materials and exported original triangles, never voxel records,
GI hit caches, light masks or filtered volumes. Primary raster surfaces are shared
deliberately to isolate the one-bounce transport approximation. No environment,
normal-map sender scattering, or extra bounces are added to the engine contract.
"""
import base64
import io
import json
from pathlib import Path

import numpy as np
from PIL import Image
from embreex import mesh_construction, rtcore_scene


def normalize(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-20)


def srgb_to_linear(v):
    return np.where(v <= .04045, v / 12.92, ((v + .055) / 1.055) ** 2.4)


def sample_image(pixels, uv, srgb=False):
    """LOD-zero repeat/bilinear sampler; decode sRGB before interpolation."""
    h, w = pixels.shape[:2]
    p = uv * [w, h] - .5
    low = np.floor(p).astype(np.int64)
    f = p - low
    result = np.zeros((len(uv), 4))
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        value = pixels[(low[:, 1] + dy) % h, (low[:, 0] + dx) % w].astype(np.float64) / 255
        if srgb:
            value[:, :3] = srgb_to_linear(value[:, :3])
        weight = (f[:, 0] if dx else 1-f[:, 0]) * (f[:, 1] if dy else 1-f[:, 1])
        result += weight[:, None] * value
    return result


class TriangleScene:
    def __init__(self, vertices, normals, uv, colors, material_ids, document, folder):
        self.vertices = np.asarray(vertices, dtype=np.float32)
        self.normals = normals
        self.uv = uv
        self.colors = colors
        self.material_ids = material_ids
        self.document = document
        self.materials = document.get('materials', []) + [{}]
        self.folder = Path(folder)
        self.images = {}
        self.scene = rtcore_scene.EmbreeScene()
        self.mesh = mesh_construction.TriangleMesh(self.scene, self.vertices)

    @classmethod
    def from_capture(cls, prefix, asset):
        prefix, asset = str(prefix), Path(asset)
        meta = json.loads(Path(prefix+'.json').read_text())
        assert meta['vertex_stride'] == 112 and meta['node_stride'] == 128
        vertices = np.fromfile(prefix+'.vertices.bin', '<f4').reshape(-1, 28)
        indices = np.fromfile(prefix+'.indices.bin', '<u4')
        nodes = np.fromfile(prefix+'.nodes.bin', '<f4').reshape(-1, 2, 4, 4).transpose(0, 1, 3, 2)
        records = np.fromfile(prefix+'.triangles.bin', '<u4').reshape(-1, 4)[:meta['triangle_count']]
        records = records[records[:, 3] != 0]
        values = vertices[indices[records[:, 0, None] + np.arange(3)]]
        transforms = nodes[records[:, 1]]
        points = np.einsum('tij,tkj->tki', transforms[:, 0, :3, :3], values[:, :, :3])
        points += transforms[:, 0, None, :3, 3]
        # Interpolate transformed normals before normalization; normalizing each
        # corner first changes smooth shading under nonuniform transforms.
        normals = np.einsum('tij,tkj->tki', transforms[:, 1, :3, :3], values[:, :, 4:7])
        assert np.isfinite(points).all()
        return cls(points, normals, values[:, :, 12:16].reshape(-1, 3, 2, 2),
                   values[:, :, 24:28], records[:, 2], json.loads(asset.read_text()), asset.parent)

    def texture(self, spec, uv, srgb=False):
        if spec is None:
            return np.ones((len(uv), 4))
        assert 'extensions' not in spec, 'Texture transforms need explicit reference support'
        source = self.document['textures'][spec['index']]['source']
        if source not in self.images:
            entry = self.document['images'][source]
            if 'uri' in entry:
                uri = entry['uri']
                data = base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else (self.folder/uri).read_bytes()
            else:
                view = self.document['bufferViews'][entry['bufferView']]
                uri = self.document['buffers'][view['buffer']]['uri']
                buffer = base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else (self.folder/uri).read_bytes()
                start = view.get('byteOffset', 0)
                data = buffer[start:start+view['byteLength']]
            self.images[source] = np.asarray(Image.open(io.BytesIO(data)).convert('RGBA'))
        return sample_image(self.images[source], uv[:, spec.get('texCoord', 0)], srgb)

    def surface(self, ids, u, v):
        bary = np.stack((1-u-v, u, v), axis=1)
        uv = np.einsum('nk,nkij->nij', bary, self.uv[ids])
        color = np.einsum('nk,nkj->nj', bary, self.colors[ids])
        normal = normalize(np.einsum('nk,nkj->nj', bary, self.normals[ids]))
        diffuse, emission = np.zeros((len(ids), 3)), np.zeros((len(ids), 3))
        visible = np.ones(len(ids), dtype=bool)
        material_ids = self.material_ids[ids]
        for index in np.unique(material_ids):
            mask = material_ids == index
            material = self.materials[index]
            pbr = material.get('pbrMetallicRoughness', {})
            albedo = color[mask] * pbr.get('baseColorFactor', [1]*4)
            albedo *= self.texture(pbr.get('baseColorTexture'), uv[mask], True)
            if material.get('alphaMode', 'OPAQUE') != 'OPAQUE':
                visible[mask] = albedo[:, 3] >= material.get('alphaCutoff', .5)
            metal = pbr.get('metallicFactor', 1) * self.texture(pbr.get('metallicRoughnessTexture'), uv[mask])[:, 2]
            diffuse[mask] = np.clip(albedo[:, :3] * (1-np.clip(metal, 0, 1))[:, None] * .96, 0, 1)
            strength = material.get('extensions', {}).get('KHR_materials_emissive_strength', {}).get('emissiveStrength', 1)
            emission[mask] = self.texture(material.get('emissiveTexture'), uv[mask], True)[:, :3]
            emission[mask] *= np.asarray(material.get('emissiveFactor', [0]*3)) * strength
        return visible, normal, diffuse, emission

    def visible(self, ids, u, v):
        # Visibility needs only alpha. Avoid evaluating RGB/emission/MR textures
        # for every candidate of every shadow ray in a full-scene reference.
        visible = np.ones(len(ids), dtype=bool)
        material_ids = self.material_ids[ids]
        for index in np.unique(material_ids):
            material = self.materials[index]
            if material.get('alphaMode', 'OPAQUE') != 'OPAQUE':
                mask = material_ids == index
                bary = np.stack((1-u[mask]-v[mask], u[mask], v[mask]), axis=1)
                uv = np.einsum('nk,nkij->nij', bary, self.uv[ids[mask]])
                color_alpha = np.einsum('nk,nk->n', bary, self.colors[ids[mask], :, 3])
                pbr = material.get('pbrMetallicRoughness', {})
                alpha = color_alpha * pbr.get('baseColorFactor', [1]*4)[3]
                alpha *= self.texture(pbr.get('baseColorTexture'), uv)[:, 3]
                visible[mask] = alpha >= material.get('alphaCutoff', .5)
        return visible

    def trace(self, origins, directions, limit=None, epsilon=1e-5):
        """Two-sided closest accepted hit, including transparent candidate continuation."""
        count = len(origins)
        ids = np.full(count, -1, dtype=np.int32)
        distance = np.full(count, np.inf)
        u, v = np.zeros(count), np.zeros(count)
        limit = np.full(count, np.inf) if limit is None else np.broadcast_to(limit, (count,))
        pending = np.arange(count)
        travelled = np.zeros(count)
        for _ in range(128):
            if not len(pending):
                break
            start = origins[pending] + directions[pending] * travelled[pending, None]
            result = self.scene.run(np.ascontiguousarray(start, dtype=np.float32),
                                    np.ascontiguousarray(directions[pending], dtype=np.float32), output=1)
            candidate = result['primID']
            t = result['tfar'] + travelled[pending]
            hit = (candidate >= 0) & (t < limit[pending])
            active = pending[hit]
            if not len(active):
                pending = active
                break
            accepted = self.visible(candidate[hit], result['u'][hit], result['v'][hit])
            good = active[accepted]
            ids[good], distance[good] = candidate[hit][accepted], t[hit][accepted]
            u[good], v[good] = result['u'][hit][accepted], result['v'][hit][accepted]
            pending = active[~accepted]
            travelled[pending] = t[hit][~accepted] + epsilon
        if len(pending):
            raise RuntimeError('Alpha traversal exceeded 128 candidates; reference is inconclusive')
        return ids, distance, u, v


def light_incident(light, positions):
    pr, dt, ci, cs = (np.asarray(light[k]) for k in ('position_range', 'direction_type', 'color_intensity', 'cone_shadow'))
    if dt[3] == 0:
        direction = np.broadcast_to(-dt[:3], positions.shape)
        distance, attenuation = np.full(len(positions), 1e20), np.ones(len(positions))
    else:
        delta = pr[:3] - positions
        distance = np.linalg.norm(delta, axis=1)
        direction = delta / np.maximum(distance[:, None], 1e-6)
        attenuation = np.clip(1-(distance/pr[3])**4, 0, 1)**2 / (distance**2+.01)
        if dt[3] == 2:
            x = np.clip((np.sum(-direction*dt[:3], axis=1)-cs[1])/(cs[0]-cs[1]), 0, 1)
            attenuation *= x*x*(3-2*x)
    return direction, distance, ci[:3]*ci[3]*attenuation[:, None]


def cosine_directions(normals, samples, seed):
    # Independent stratified radial / golden-angle azimuth samples, scrambled per pixel.
    rng = np.random.default_rng(seed)
    shifts = rng.random((len(normals), 2))
    radial = (np.arange(samples)[None, :] + shifts[:, :1]) / samples
    phi = 2*np.pi*((np.arange(samples)[None, :]*.6180339887498949 + shifts[:, 1:]) % 1)
    helper = np.zeros_like(normals)
    helper[:, 2] = 1
    helper[np.abs(normals[:, 2]) > .9] = [0, 1, 0]
    tangent = normalize(np.cross(helper, normals))
    bitangent = np.cross(normals, tangent)
    return (np.sqrt(radial)[..., None] * (np.cos(phi)[..., None]*tangent[:, None] +
            np.sin(phi)[..., None]*bitangent[:, None]) + np.sqrt(1-radial)[..., None]*normals[:, None])


def receiver_weight(surface, camera):
    normal = normalize(surface[:, 4:7])
    view = normalize(np.asarray(camera) - surface[:, :3])
    ndv = np.maximum(np.sum(normal*view, axis=1), .001)
    albedo, metal = surface[:, 16:19], surface[:, 19:20]
    f0 = .04*(1-metal) + albedo*metal
    fresnel = f0 + (1-f0)*(1-ndv[:, None])**5
    return albedo * (1-fresnel) * (1-metal) * surface[:, 20:21]


def integrate(scene, surface, meta, samples, seed, primary_offset, epsilon):
    output = np.zeros((len(surface), 3))
    batch_pixels = max(1, 65536 // samples)
    for begin in range(0, len(surface), batch_pixels):
        end = min(begin+batch_pixels, len(surface))
        pixel = surface[begin:end]
        normals = normalize(pixel[:, 4:7])
        geom = normalize(pixel[:, 8:11])
        # FP16 primary positions can lie just behind their triangle; fixed offset is
        # independent of voxel resolution and bounded in the validation contract.
        origins = pixel[:, :3] + geom*primary_offset
        dirs = cosine_directions(normals, samples, seed+begin).reshape(-1, 3)
        origins = np.repeat(origins, samples, axis=0)
        ids, distance, u, v = scene.trace(origins, dirs, epsilon=epsilon)
        # A perturbed shading normal must not turn a ray into the receiver's
        # opaque back hemisphere into another lit bounce on the same surface.
        front = np.sum(dirs*np.repeat(geom, samples, axis=0), axis=1) > 0
        hit = (ids >= 0) & front
        radiance = np.zeros((len(ids), 3))
        if hit.any():
            _, normal, diffuse, emission = scene.surface(ids[hit], u[hit], v[hit])
            points = origins[hit] + dirs[hit]*distance[hit, None]
            incident = np.zeros_like(emission)
            for light in meta['lights']:
                direction, reach, energy = light_incident(light, points)
                cosine = np.maximum(np.sum(normal*direction, axis=1), 0)
                lit = (cosine > 0) & (np.max(energy, axis=1) > 0)
                if light['cone_shadow'][2] and lit.any():
                    blocker = scene.trace(points[lit]+normal[lit]*epsilon, direction[lit],
                                          np.maximum(reach[lit]-epsilon, 0), epsilon)[0]
                    lit[np.flatnonzero(lit)[blocker >= 0]] = False
                incident[lit] += energy[lit]*cosine[lit, None]
            radiance[hit] = diffuse*incident/np.pi + emission
        output[begin:end] = radiance.reshape(-1, samples, 3).mean(axis=1)
        if len(surface) > 5000 and (begin // batch_pixels) % 128 == 0:
            print(f'  triangle integration seed={seed} pixels={end}/{len(surface)}', flush=True)
    return output * receiver_weight(surface, meta['camera_position']) * meta['indirect_intensity']


def error_metrics(actual, reference):
    difference = actual-reference
    norm = np.linalg.norm(reference)
    return dict(rmse=float(np.sqrt(np.mean(difference**2))),
                relative_rmse=float(np.linalg.norm(difference)/max(norm, 1e-15)),
                relative_mean_bias=float(np.sum(difference)/max(np.sum(reference), 1e-15)),
                actual_mean_rgb=actual.mean(axis=0).tolist(), reference_mean_rgb=reference.mean(axis=0).tolist())


def load_capture(prefix):
    prefix = str(prefix)
    meta = json.loads(Path(prefix+'.lighting.json').read_text())
    assert meta['bytes_per_pixel'] == 112 and meta['method'] == 'dynamic_voxel'
    assert meta['query_backend'] == 'voxel_dda'
    assert meta['environment_intensity_rotation_enabled_visible'][2] == 0
    assert meta['voxel_geometry_generation'] == meta['scene_geometry_generation']
    lighting = np.fromfile(prefix+'.lighting.bin', '<f4').reshape(-1, 7, 4)
    surface = np.fromfile(prefix+'.surface.bin', '<f4').reshape(-1, 24)
    assert len(surface) == len(lighting) == meta['width']*meta['height']
    assert np.isfinite(lighting).all() and np.isfinite(surface[:, :12]).all()
    assert np.allclose(lighting[:, 0, :3], lighting[:, 1:5, :3].sum(axis=1), rtol=2e-5, atol=2e-5)
    assert np.allclose(lighting[:, 2, :3], lighting[:, 5:7, :3].sum(axis=1), rtol=2e-5, atol=2e-5)
    selected = (lighting[:, 0, 3] > .5) & (surface[:, 3] > .5)
    assert np.all(surface[selected, 22] == 1), 'Fallback/unready surfaces invalidate quality capture'
    selected &= np.max(lighting[:, 4, :3], axis=1) == 0
    return meta, lighting[:, 2, :3], surface, selected
