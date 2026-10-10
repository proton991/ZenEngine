"""Independent full-resolution sky ground truth for a hybrid GI capture, using Mitsuba 3.

Evaluates the lighting-contract sky term

    D_sky(x) = (1 / pi) * integral(L_env(w) * V(x, w) * max(dot(n_s, w), 0) dw)

at the pixel centers of a capture, with directions below the geometric normal blocked. It
shares no code with the engine. glTF parsing, primary visibility, the BVH (Mitsuba),
environment importance sampling and the estimator are also separate from
tools/environment_reference.py, which only supplies the cube lookup used to resample the
environment and, on purpose, the Embree tracer of the visibility cross-check. Inputs common
to the engine by definition are the scene file, the capture's camera matrix, and the
captured level-zero environment cube (the engine's L_env).

Primary rays follow rasterization: back faces of single-sided materials and alpha-masked
texels are skipped. Visibility rays follow the contract: both faces of every surface
block; masked texels are transparent. Alpha masks use the engine's rule per hit (factor *
bilinear texture alpha at level zero * interpolated vertex alpha >= cutoff). Shading normals
default to interpolated vertex normals, so compare against captures made with
--strip-normal-maps. --shading-normals engine instead uses captured shading normals
to check the normal-mapped path while keeping primary geometry and visibility independent.

Each sample combines one environment-importance sample (Mitsuba's envmap) and one cosine
sample with the balance heuristic. Radiance always comes from a bilinear lookup of the captured
cube, exactly the engine's L_env; the envmap, resampled from that cube, only chooses directions
and supplies their density. (Evaluating the resampled map instead widened the hotel room's
sub-degree lamps enough to shift hard penumbrae.) Outputs: <output>.npz (sky, nu, two independent half
estimates, primary hits, normals, primitive index), <output>.json, and per compared capture
a report in <output>-compare.json and an image <output>-compare-<i>.png. --backend auto uses
Mitsuba's CUDA backend when an NVIDIA GPU is available (driver only) and its CPU LLVM backend
otherwise (DRJIT_LIBLLVM_PATH may point at LLVM-C.dll; Visual Studio's copy is found
automatically). Both run the same estimator.
"""
import argparse
import base64
import glob
import io
import json
import os
from pathlib import Path
import sys
from urllib.parse import unquote

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_hybrid_gi import load as load_capture  # noqa: E402
from environment_reference import Environment, cosine_directions, load_gltf  # noqa: E402


def setup_mitsuba(backend='auto'):
    """Mitsuba with its CUDA backend when requested or available, else the CPU LLVM backend.

    The backends run the same estimator; CUDA (NVIDIA driver only, no toolkit) is used purely
    to speed up this offline reference and never by the engine.
    """
    import mitsuba as mi
    if backend in ('auto', 'cuda'):
        try:
            mi.set_variant('cuda_ad_rgb')
            import drjit as dr
            dr.eval(dr.zeros(mi.Float, 1))
            print('Mitsuba backend: cuda_ad_rgb', file=sys.stderr, flush=True)
            return mi
        except Exception as error:
            if backend == 'cuda':
                raise
            print(f'Mitsuba CUDA backend unavailable ({error}); using the CPU LLVM backend', file=sys.stderr, flush=True)
    if 'DRJIT_LIBLLVM_PATH' not in os.environ and os.name == 'nt':
        found = sorted(glob.glob('C:/Program Files/LLVM/bin/LLVM-C.dll')
                       + glob.glob('C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/LLVM-C.dll'))
        if found:
            os.environ['DRJIT_LIBLLVM_PATH'] = found[-1]
    mi.set_variant('llvm_ad_rgb')
    return mi


def load_gltf_primitives(path):
    """Primitives in the renderer's normalized world space: positions, normals, alpha UVs, faces, material."""
    path = Path(path)
    doc = json.loads(path.read_text(encoding='utf-8'))

    def uri_bytes(uri):
        if uri.startswith('data:'):
            return base64.b64decode(uri.split(',', 1)[1])
        return (Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:') else path.parent / unquote(uri)).read_bytes()

    buffers = [uri_bytes(b['uri']) for b in doc.get('buffers', [])]

    def accessor(index):
        a = doc['accessors'][index]
        assert 'sparse' not in a, 'Sparse accessors are not supported'
        view = doc['bufferViews'][a['bufferView']]
        dtype = np.dtype({5120: 'i1', 5121: 'u1', 5122: '<i2', 5123: '<u2', 5125: '<u4', 5126: '<f4'}[a['componentType']])
        width = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        out = np.ndarray((a['count'], width), dtype, buffer=buffers[view['buffer']],
                         offset=view.get('byteOffset', 0) + a.get('byteOffset', 0),
                         strides=(view.get('byteStride', width * dtype.itemsize), dtype.itemsize)).astype(float)
        if a.get('normalized'):
            out = np.maximum(-1, out / np.iinfo(dtype).max)
        return out

    def node_matrix(node):
        if 'matrix' in node:
            return np.array(node['matrix'], float).reshape(4, 4).T
        x, y, z, w = node.get('rotation', [0, 0, 0, 1])
        m = np.eye(4)
        m[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
        m[:3, :3] = m[:3, :3] * np.array(node.get('scale', [1, 1, 1]))[None, :]
        m[:3, 3] = node.get('translation', [0, 0, 0])
        return m

    materials = doc.get('materials', [])
    primitives = []

    def visit(index, parent):
        node = doc['nodes'][index]
        assert 'skin' not in node and 'weights' not in node, 'Deformed geometry is not supported'
        world = parent @ node_matrix(node)
        if 'mesh' in node:
            linear = world[:3, :3]
            normal_matrix = np.linalg.inv(linear).T
            flip = np.linalg.det(linear) < 0
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                assert primitive.get('mode', 4) == 4 and 'targets' not in primitive
                assert 'KHR_draco_mesh_compression' not in primitive.get('extensions', {})
                attributes = primitive['attributes']
                positions = accessor(attributes['POSITION']) @ linear.T + world[:3, 3]
                faces = (accessor(primitive['indices']).astype(np.uint32).reshape(-1, 3) if 'indices' in primitive
                         else np.arange(len(positions), dtype=np.uint32).reshape(-1, 3))
                if flip:
                    # A mirroring transform reverses the front-face winding (glTF 3.7.4).
                    faces = faces[:, ::-1].copy()
                normals = None
                if 'NORMAL' in attributes:
                    normals = accessor(attributes['NORMAL']) @ normal_matrix.T
                    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-20)
                material_index = primitive.get('material')
                material = materials[material_index] if material_index is not None else {}
                pbr = material.get('pbrMetallicRoughness', {})
                specular_glossiness = material.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness')
                if specular_glossiness:
                    pbr = dict(baseColorFactor=specular_glossiness.get('diffuseFactor', [1, 1, 1, 1]),
                               baseColorTexture=specular_glossiness.get('diffuseTexture'))
                masked = material.get('alphaMode') == 'MASK'
                texture = pbr.get('baseColorTexture')
                uv = np.zeros((len(positions), 2))
                vertex_alpha = np.ones(len(positions))
                if masked:
                    if 'COLOR_0' in attributes:
                        colors = accessor(attributes['COLOR_0'])
                        vertex_alpha = colors[:, 3] if colors.shape[1] == 4 else vertex_alpha
                    if texture:
                        transform = texture.get('extensions', {}).get('KHR_texture_transform', {})
                        channel = transform.get('texCoord', texture.get('texCoord', 0))
                        uv = accessor(attributes[f'TEXCOORD_{channel}'])
                        c, s = np.cos(transform.get('rotation', 0)), np.sin(transform.get('rotation', 0))
                        uv = (uv * transform.get('scale', [1, 1])) @ np.array([[c, s], [-s, c]]) + transform.get('offset', [0, 0])
                primitives.append(dict(positions=positions, normals=normals, uv=uv, faces=faces,
                                       material=-1 if material_index is None else material_index,
                                       double_sided=bool(material.get('doubleSided', False)), masked=masked,
                                       cutoff=material.get('alphaCutoff', 0.5), vertex_alpha=vertex_alpha,
                                       alpha=pbr.get('baseColorFactor', [1, 1, 1, 1])[3], texture=texture))
        for child in node.get('children', []):
            visit(child, world)

    for node in doc['scenes'][doc.get('scene', 0)]['nodes']:
        visit(node, np.eye(4))
    referenced = np.concatenate([p['positions'][p['faces'].ravel()] for p in primitives])
    lo, hi = referenced.min(axis=0), referenced.max(axis=0)
    center, extent = (lo + hi) / 2, max(hi - lo)
    for p in primitives:
        p['positions'] = (p['positions'] - center) / extent

    def alpha_image(texture):
        source = doc['textures'][texture['index']]
        image = doc['images'][source['source']]
        if 'uri' in image:
            encoded = uri_bytes(image['uri'])
        else:
            view = doc['bufferViews'][image['bufferView']]
            start = view.get('byteOffset', 0)
            encoded = buffers[view['buffer']][start:start + view['byteLength']]
        texel_alpha = np.asarray(Image.open(io.BytesIO(encoded)).convert('RGBA'), dtype=np.float32)[..., 3] / 255
        sampler = doc.get('samplers', [{}])[source['sampler']] if 'sampler' in source else {}
        wrap = {33071: 'clamp', 33648: 'mirror'}.get(sampler.get('wrapS', 10497), 'repeat')
        return texel_alpha[..., None], wrap

    return primitives, alpha_image, dict(center=center.tolist(), scale=1 / extent)


# Alpha masks follow the engine: factor * bilinear texture alpha * interpolated vertex alpha
# >= cutoff, evaluated per hit. The texture alpha is the opacity of a Mitsuba mask BSDF; the
# rest is the per-vertex attribute below (huge for surfaces that are never transparent).
MASK_ATTRIBUTE = 'vertex_mask_scale'
PRIMITIVE_ATTRIBUTE = 'vertex_primitive'


def build_scene(mi, primitives, alpha_image, environment, resolution=2048):
    """Mitsuba scene and whether the environment is black (D_sky is then zero by definition)."""
    import drjit as dr
    # Mitsuba's envmap maps a world direction d to u = atan2(d.x, -d.z) / 2pi, v = acos(d.y) / pi.
    # Each texel averages 2x2 sub-directions of the engine's L_env (cube lookup with the
    # capture's rotation, orientation and intensity).
    width, height = resolution, resolution // 2
    accumulated = np.zeros((height, width, 3))
    for sx in (0.25, 0.75):
        for sy in (0.25, 0.75):
            u = (np.arange(width) + sx) / width
            v = (np.arange(height) + sy) / height
            theta, phi = np.meshgrid(v * np.pi, u * 2 * np.pi, indexing='ij')
            d = np.stack([np.sin(theta) * np.sin(phi), np.cos(theta), -np.sin(theta) * np.cos(phi)], -1).reshape(-1, 3)
            accumulated += environment.sample(d).reshape(height, width, 3)
    radiance = (accumulated / 4).astype(np.float32)
    black = not radiance.max() > 0
    shapes = {} if black else {'environment': mi.load_dict({'type': 'envmap', 'bitmap': mi.Bitmap(radiance)})}
    for index, p in enumerate(primitives):
        bsdf = {'type': 'diffuse', 'reflectance': {'type': 'rgb', 'value': 1.0}}
        if p['double_sided']:
            bsdf = {'type': 'twosided', 'bsdf': bsdf}
        scale = np.full(len(p['positions']), 1e30)
        if p['masked'] and p['cutoff'] > 0:
            scale = p['alpha'] * p['vertex_alpha'] / p['cutoff']
            if p['texture']:
                alpha, wrap = alpha_image(p['texture'])
                opacity = {'type': 'bitmap', 'bitmap': mi.Bitmap(alpha), 'raw': True, 'filter_type': 'bilinear', 'wrap_mode': wrap}
                bsdf = {'type': 'mask', 'opacity': opacity, 'bsdf': bsdf}
        props = mi.Properties()
        props['bsdf'] = mi.load_dict(bsdf)
        has_normals = p['normals'] is not None
        mesh = mi.Mesh(f'primitive_{index}', len(p['positions']), len(p['faces']), props, has_normals, True)
        params = mi.traverse(mesh)
        params['vertex_positions'] = mi.Float(p['positions'].astype(np.float32).ravel())
        if has_normals:
            params['vertex_normals'] = mi.Float(p['normals'].astype(np.float32).ravel())
        params['vertex_texcoords'] = mi.Float(p['uv'].astype(np.float32).ravel())
        params['faces'] = mi.UInt32(p['faces'].ravel())
        params.update()
        mesh.add_attribute(MASK_ATTRIBUTE, 1, mi.Float(scale.astype(np.float32)))
        mesh.add_attribute(PRIMITIVE_ATTRIBUTE, 1, mi.Float(np.full(len(p['positions']), index, np.float32)))
        shapes[f'primitive_{index}'] = mesh
    shapes['type'] = 'scene'
    return mi.load_dict(shapes), dr, black


def transparent(mi, si, bsdf, active):
    """Alpha-mask rejection at a hit; a mask BSDF's null transmission is 1 - texture alpha."""
    texture_alpha = 1 - bsdf.eval_null_transmission(si, active).x
    return active & (texture_alpha * si.shape.eval_attribute_1(MASK_ATTRIBUTE, si, active) < 1)


def camera_rays(metadata, capture):
    """Pixel-center world-space ray directions from the capture's projection-view matrix.

    The NDC y convention is chosen by reprojecting the engine's receiver positions, and
    the choice is asserted to land on pixel centers.
    """
    width, height = metadata['width'], metadata['height']
    # Metadata prints enough decimal digits to round-trip float32. Restore those
    # exact matrix values before inverting in float64. The separately stored eye
    # differs slightly from the eye implied by the rounded projection-view matrix;
    # subtracting it from a near-plane point amplifies that difference with distance.
    pv = np.array(metadata['projection_view_column_major'], np.float32).astype(np.float64).reshape(4, 4).T
    inverse = np.linalg.inv(pv)
    if abs(inverse[3, 2]) < 1e-12:
        raise ValueError('Ground-truth primary rays currently require a perspective projection')
    eye = inverse[:3, 2]/inverse[3, 2]
    valid = capture[:, :, 8, 3] > 0
    ys, xs = np.nonzero(valid)
    pick = np.linspace(0, len(xs) - 1, min(len(xs), 2000)).astype(int)
    points = capture[ys[pick], xs[pick], 8, :3]
    clip = np.c_[points, np.ones(len(points))] @ pv.T
    ndc = clip[:, :2] / clip[:, 3:4]
    px = (ndc[:, 0] * .5 + .5) * width - .5
    best = None
    for sign in (1, -1):
        py = (sign * ndc[:, 1] * .5 + .5) * height - .5
        error = np.median(np.hypot(px - xs[pick], py - ys[pick]))
        if best is None or error < best[0]:
            best = (error, sign)
    assert best[0] < 0.05, f'Capture camera does not reproject to pixel centers ({best[0]:.3f} px)'
    sign = best[1]
    gx, gy = np.meshgrid((np.arange(width) + .5) / width * 2 - 1, (np.arange(height) + .5) / height * 2 - 1)
    ndc = np.stack([gx, sign * gy, np.full_like(gx, .5), np.ones_like(gx)], -1).reshape(-1, 4)
    world = ndc @ inverse.T
    world = world[:, :3] / world[:, 3:4]
    directions = world - eye
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    return eye, directions.reshape(height, width, 3)


def primary_hits(mi, dr, scene, eye, directions, max_layers=64, origins=None):
    """Rasterization-equivalent primary hits: skip culled back faces and masked texels."""
    count = len(directions)
    origin = (mi.Point3f(*[mi.Float(np.full(count, c, np.float32)) for c in eye]) if origins is None else
              mi.Point3f(*[mi.Float(origins[:, i].astype(np.float32)) for i in range(3)]))
    ray = mi.Ray3f(origin,
                   mi.Vector3f(*[mi.Float(directions[:, i].astype(np.float32)) for i in range(3)]))
    active = mi.Bool(np.ones(count, bool))
    for _ in range(max_layers):
        si = scene.ray_intersect(ray, active)
        bsdf = si.bsdf(ray)
        back = dr.dot(si.n, ray.d) > 0
        culled = back & ~mi.has_flag(bsdf.flags(), mi.BSDFFlags.BackSide)
        hit = active & si.is_valid()
        skip = hit & (culled | transparent(mi, si, bsdf, hit))
        ray = dr.select(skip, continue_ray(mi, si, ray.d), ray)
        active = skip
        dr.eval(ray, active)
        if not dr.any(active):
            break
    if dr.any(active):
        raise RuntimeError('Primary alpha/back-face traversal exhausted max_layers')
    si = scene.ray_intersect(ray)
    toward = dr.dot(si.n, ray.d) < 0
    n_g = dr.select(toward, si.n, -si.n)
    n_s = dr.select(toward, si.sh_frame.n, -si.sh_frame.n)
    valid = np.array(si.is_valid())
    primitive = np.array(si.shape.eval_attribute_1(PRIMITIVE_ATTRIBUTE, si, si.is_valid())).round().astype(np.int32)
    return valid, np.array(si.p).T, np.array(n_g).T, np.array(n_s).T, primitive


def continue_ray(mi, interaction, direction):
    """A rejected alpha candidate is not a new reflecting surface. Keep the line of
    sight; spawn_ray's normal offset can jump across foliage and thin nearby walls.
    Match the independent Embree oracle's one-micro-unit forward progress.
    """
    return mi.Ray3f(interaction.p + direction * 1e-6, direction)


def visible(mi, dr, scene, origin_interaction, directions, valid, max_layers=64, ray_origin=None):
    """Visibility toward the environment for valid lanes (others are blocked); masked texels
    are transparent and both faces of every surface block."""
    # Start a few float ULPs above the surface. Mitsuba's spawn_ray offsets by ~1e-4 of the
    # position magnitude (millimeters in Sponza), which closes centimeter-wide gaps under
    # ledges; the engine and the Embree reference both use offsets of order 1e-6.
    p = origin_interaction.p
    offset = 4e-6 * (1 + dr.maximum(dr.maximum(dr.abs(p.x), dr.abs(p.y)), dr.abs(p.z)))
    ray = mi.Ray3f(p + origin_interaction.n * offset if ray_origin is None else ray_origin, directions)
    open_ = mi.Bool(valid)
    active = mi.Bool(valid)
    def step(current, open_mask, pending, iteration):
        si = scene.ray_intersect(current, pending)
        hit = pending & si.is_valid()
        passes = transparent(mi, si, si.bsdf(current), hit)
        open_mask = open_mask & ~(hit & ~passes)
        current = dr.select(passes, continue_ray(mi, si, current.d), current)
        return current, open_mask, passes, iteration + 1

    # Run alpha continuation on the device. Host-side reductions at every layer
    # otherwise dominate the high-sample sweep without adding independent samples.
    ray, open_, active, _ = dr.while_loop(
        state=(ray, open_, active, mi.UInt32(0)),
        cond=lambda current, open_mask, pending, iteration: pending & (iteration < max_layers),
        body=step, mode='symbolic', label='sky visibility')
    if dr.any(active):
        raise RuntimeError('Visibility alpha traversal exhausted max_layers')
    return open_


class CubeRadiance:
    """The engine's L_env: bilinear lookup of the captured level-zero cube (Vulkan face layout,
    filtering clamped within a face) after the capture's rotation and orientation, times its
    intensity. Mitsuba's envmap only chooses directions and supplies their density; radiance
    always comes from here, so resampling the cube for sampling cannot change the integrand."""

    def __init__(self, mi, dr, environment):
        self.mi, self.dr = mi, dr
        self.size = environment.cube.shape[1]
        self.texels = mi.Float(np.ascontiguousarray(environment.cube[..., :3], np.float32).ravel())
        self.rotation, self.orientation = float(environment.rotation), [float(v) for v in environment.orientation]
        self.intensity = float(environment.intensity)

    def eval(self, d):
        mi, dr, size = self.mi, self.dr, self.size
        c, s = np.cos(self.rotation), np.sin(self.rotation)
        v = mi.Vector3f(c * d.x - s * d.z, d.y, s * d.x + c * d.z)
        q = mi.Vector3f(*self.orientation[:3])
        v = v + 2 * dr.cross(q, dr.cross(q, v) + self.orientation[3] * v)
        ax, ay, az = dr.abs(v.x), dr.abs(v.y), dr.abs(v.z)
        x_major = (ax >= ay) & (ax >= az)
        y_major = ~x_major & (ay >= az)
        major = dr.select(x_major, ax, dr.select(y_major, ay, az))
        face = dr.select(x_major, dr.select(v.x >= 0, 0, 1), dr.select(y_major, dr.select(v.y >= 0, 2, 3), dr.select(v.z >= 0, 4, 5)))
        sc = dr.select(x_major, dr.select(v.x >= 0, -v.z, v.z), dr.select(y_major, v.x, dr.select(v.z >= 0, v.x, -v.x)))
        tc = dr.select(x_major, -v.y, dr.select(y_major, dr.select(v.y >= 0, v.z, -v.z), -v.y))
        px = (sc / major * .5 + .5) * size - .5
        py = (tc / major * .5 + .5) * size - .5
        x0, y0 = dr.floor(px), dr.floor(py)
        fx, fy = px - x0, py - y0
        face = mi.UInt32(face)
        result = dr.zeros(mi.Color3f, dr.width(d))
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            xi = mi.UInt32(dr.clip(x0 + dx, 0, size - 1))
            yi = mi.UInt32(dr.clip(y0 + dy, 0, size - 1))
            base = ((face * size + yi) * size + xi) * 3
            weight = (fx if dx else 1 - fx) * (fy if dy else 1 - fy)
            result += mi.Color3f(*[dr.gather(mi.Float, self.texels, base + k) for k in range(3)]) * weight
        return result * self.intensity


def render_sky(mi, dr, scene, points, n_g, n_s, samples, batch, seed, cube, origins=None):
    """Per-pixel D_sky and nu (cosine-weighted unblocked fraction) with environment+cosine MIS."""
    count = len(points)
    lanes = count * batch
    environment = scene.environment()
    sampler = mi.load_dict({'type': 'independent'})
    passes = max(2, samples // batch)
    repeat = lambda a: mi.Float(np.repeat(a, batch).astype(np.float32))  # noqa: E731
    it = dr.zeros(mi.SurfaceInteraction3f, lanes)
    it.p = mi.Point3f(*[repeat(points[:, i]) for i in range(3)])
    it.n = mi.Normal3f(*[repeat(n_g[:, i]) for i in range(3)])
    shading = mi.Normal3f(*[repeat(n_s[:, i]) for i in range(3)])
    ray_origin = None if origins is None else mi.Point3f(*[repeat(origins[:, i]) for i in range(3)])
    it.sh_frame = mi.Frame3f(shading)
    halves = [dr.zeros(mi.Color3f, lanes), dr.zeros(mi.Color3f, lanes)]
    unblocked = dr.zeros(mi.Float, lanes)
    for index in range(passes):
        sampler.seed(seed * 1000003 + index, lanes)
        # Environment importance sample.
        ds, _ = scene.sample_emitter_direction(it, sampler.next_2d(), False)
        cos_e = dr.dot(shading, ds.d)
        ok_e = (cos_e > 0) & (dr.dot(it.n, ds.d) > 0) & (ds.pdf > 0)
        pdf_bsdf_e = dr.maximum(cos_e, 0) / dr.pi
        mis_e = ds.pdf / (ds.pdf + pdf_bsdf_e)
        vis_e = visible(mi, dr, scene, it, ds.d, ok_e, ray_origin=ray_origin)
        contribution = dr.select(vis_e, cube.eval(ds.d) / ds.pdf * cos_e / dr.pi * mis_e, 0)
        # Cosine sample.
        local = mi.warp.square_to_cosine_hemisphere(sampler.next_2d())
        direction = it.sh_frame.to_world(local)
        cos_b = local.z
        ok_b = (cos_b > 0) & (dr.dot(it.n, direction) > 0)
        radiance = cube.eval(direction)
        sample = dr.zeros(mi.DirectionSample3f, lanes)
        sample.d = direction
        pdf_env = environment.pdf_direction(it, sample)
        mis_b = (cos_b / dr.pi) / (cos_b / dr.pi + pdf_env)
        vis_b = visible(mi, dr, scene, it, direction, ok_b, ray_origin=ray_origin)
        contribution += dr.select(vis_b, radiance * mis_b, 0)
        halves[index % 2] += contribution
        unblocked += dr.select(vis_b, 1.0, 0.0)
        dr.eval(halves[index % 2], unblocked)
    # Each half averages every other pass, so the two are independent estimates.
    split = [np.array(h).T.reshape(count, batch, 3).mean(1) / ((passes + 1 - k) // 2) for k, h in enumerate(halves)]
    total = (split[0] * ((passes + 1) // 2) + split[1] * (passes // 2)) / passes
    nu = np.array(unblocked).reshape(count, batch).mean(1) / passes
    return total, nu, np.stack(split)


def render_capture(mi, capture, output, scene_path=None, samples=1024, batch=8, seed=1, environment_resolution=2048,
                   engine_receivers=False, engine_normals=False):
    metadata, data = load_capture(capture)
    e = metadata['environment_intensity_rotation_enabled_visible']
    size = metadata['environment_cube_size']
    cube = np.fromfile(str(capture) + '.environment.bin', '<f4').reshape(6, size, size, 4)
    environment = Environment(cube=cube, rotation=e[1], orientation=metadata.get('environment_orientation', [0, 0, 0, 1]),
                              intensity=e[0] * e[2])
    scene_path = scene_path or Path(str(capture) + '.gltf')
    primitives, alpha_image, normalization = load_gltf_primitives(scene_path)
    scene, dr, black = build_scene(mi, primitives, alpha_image, environment, environment_resolution)
    eye, directions = camera_rays(metadata, data)
    height, width = directions.shape[:2]
    covered = data[:, :, 8, 3].reshape(-1) > 0
    valid, points, n_g, n_s, primitive = primary_hits(mi, dr, scene, eye, directions.reshape(-1, 3)[covered])
    origins = None
    if engine_normals:
        # Test the actual normal-mapped shading path without substituting Mitsuba's
        # tangent convention. Geometry, alpha, origins and visibility remain independent.
        n_s = data[:, :, 9, :3].reshape(-1, 3)[covered].copy()
    if engine_receivers:
        # Diagnostic: shade the engine's receivers instead of the exact hits,
        # separating receiver-reconstruction error from sky-integral error.
        flat = data.reshape(-1, data.shape[2], 4)[covered]
        points, n_s, n_g = flat[:, 8, :3].astype(float), flat[:, 9, :3].astype(float), flat[:, 10, :3].astype(float)
        origins = engine_origins(metadata, data).reshape(-1, 3)[covered][valid]
    if engine_normals or engine_receivers:
        # The capture stores normals in half precision. Restore unit length before
        # building the cosine frame and evaluating its solid-angle density.
        n_s /= np.maximum(np.linalg.norm(n_s, axis=1, keepdims=True), 1e-20)
    sky, nu, halves = np.zeros((height * width, 3)), np.zeros(height * width), np.zeros((2, height * width, 3))
    lit = np.nonzero(covered)[0][valid]
    if len(lit) and not black:
        s, n, h = render_sky(mi, dr, scene, points[valid], n_g[valid], n_s[valid], samples, batch, seed,
                             CubeRadiance(mi, dr, environment), origins=origins)
        sky[lit], nu[lit], halves[:, lit] = s, n, h
    position = np.zeros((height * width, 4), np.float32)
    position[lit, :3], position[lit, 3] = points[valid], 1
    normal = np.zeros((height * width, 3), np.float32)
    normal[lit] = n_s[valid]
    geometric = np.zeros((height * width, 3), np.float32)
    geometric[lit] = n_g[valid]
    primitive_index = np.full(height * width, -1, np.int32)
    primitive_index[lit] = primitive[valid]
    output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(str(output) + '.npz', sky=sky.reshape(height, width, 3).astype(np.float32),
                        nu=nu.reshape(height, width).astype(np.float32),
                        halves=halves.reshape(2, height, width, 3).astype(np.float32),
                        position=position.reshape(height, width, 4), normal=normal.reshape(height, width, 3), geometric=geometric.reshape(height, width, 3),
                        primitive=primitive_index.reshape(height, width))
    manifest = dict(capture=str(capture), scene=str(scene_path), samples=samples, seed=seed,
                    shading_normals='engine' if engine_normals or engine_receivers else 'vertex',
                    captured_normal_policy='unit_renormalized' if engine_normals or engine_receivers else 'not_applicable',
                    receivers='engine' if engine_receivers else 'exact',
                    alpha_continuation='direction_1e-6',
                    primary_rays='inverse_float32_projection_view',
                    environment_resolution=environment_resolution, normalization=normalization, primitives=len(primitives),
                    covered=int(covered.sum()), primary_hits=int(len(lit)), mitsuba=mi.__version__, variant=mi.variant())
    Path(str(output) + '.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return Path(str(output) + '.npz')


def engine_origins(metadata, data, project_error=False, policy='auto'):
    if policy == 'auto':
        policy = 'plane' if metadata.get('receiver_position') == 'raster_fp32' else 'depth'
    position, normal = data[:, :, 8, :3], data[:, :, 10, :3]
    if policy == 'truth-offset':
        return position + normal * np.float32(4e-6) * (1 + np.abs(position).max(-1))[..., None]
    # Reconstruct the two-adjacent-depth displacement with float32 shader arithmetic.
    pv = np.array(metadata['projection_view_column_major'], np.float32).astype(np.float64).reshape(4, 4).T
    inverse = np.linalg.inv(pv)
    world_origin = (inverse[:3, 3]/inverse[3, 3]).astype(np.float32)
    inverse[:3] -= world_origin[:, None]*inverse[3]
    inverse = inverse.astype(np.float32)
    height, width = position.shape[:2]
    x, y = np.meshgrid((np.arange(width, dtype=np.float32)+.5)*2/width-1,
                       (np.arange(height, dtype=np.float32)+.5)*2/height-1)
    depth = data[:, :, 10, 3].copy()
    adjacent = (depth.view(np.uint32)+2).view(np.float32)
    shifted = np.stack((x, y, adjacent, np.ones_like(x)), -1) @ inverse.T
    shifted = shifted[..., :3]/shifted[..., 3:4] + world_origin
    delta = shifted-position
    error = abs(np.sum(delta*normal, axis=-1)) if project_error else np.linalg.norm(delta, axis=-1)
    if policy == 'plane':
        error = np.zeros_like(error)
    origin = (position + normal*(error+1e-6)[..., None]).astype(np.float32)
    bits = origin.view(np.int32)
    offset = (normal*32).astype(np.int32)
    return (bits + np.where(origin < 0, -offset, offset)).view(np.float32)


def depth_quantization_error(metadata, positions):
    """Distance the reconstructed receiver moves for one float32 step of its stored depth."""
    pv = np.array(metadata['projection_view_column_major'], float).reshape(4, 4).T
    flat = positions.reshape(-1, 3)
    clip = np.c_[flat, np.ones(len(flat))] @ pv.T
    ndc = clip[:, :3] / clip[:, 3:4]
    step = np.spacing(np.abs(ndc[:, 2]).astype(np.float32)).astype(float)
    moved = np.c_[ndc[:, :2], ndc[:, 2] + step, np.ones(len(flat))] @ np.linalg.inv(pv).T
    moved = moved[:, :3] / moved[:, 3:4]
    return np.linalg.norm(moved - flat, axis=1).reshape(positions.shape[:-1])


# Receivers on the same surface have geometric normals within 0.05 degrees (G-buffer encoding and
# derivative error, measured on Sponza at 960x540); verdicts are unchanged from 0.1 to 2 degrees.
# Where two faces meet, positions agree but the rasterizer, with its fixed-point vertex snapping,
# and the ray may take different faces at the pixel center.
SAME_SURFACE_NORMAL_DEGREES = 0.5


def receiver_agreement(truth, metadata, data):
    """Pixels where the engine's receiver and the ground truth's primary hit are the same surface:
    within four depth-quantization steps of each other and, where the truth has geometric normals,
    with geometric normals within SAME_SURFACE_NORMAL_DEGREES (either orientation)."""
    engine = data[:, :, 8, :3]
    covered = data[:, :, 8, 3] > 0
    hit = truth['position'][..., 3] > 0
    tolerance = 4 * depth_quantization_error(metadata, engine) + 1e-6 * (1 + np.abs(engine).max(-1))
    agree = covered & hit & (np.linalg.norm(truth['position'][..., :3] - engine, axis=-1) <= tolerance)
    if 'geometric' in truth.files:
        a, b = data[:, :, 10, :3], truth['geometric']
        cosine = np.abs(np.sum(a * b, axis=-1)) / np.maximum(np.linalg.norm(a, axis=-1) * np.linalg.norm(b, axis=-1), 1e-20)
        agree &= cosine >= np.cos(np.radians(SAME_SURFACE_NORMAL_DEGREES))
    return covered, hit, agree


def compare(truth_path, capture, component=1, far=0.3, additional_regions=None):
    """Error of a capture's sky component against the ground truth, per region, normalized by the region mean.

    Pixels whose primary hit differs from the engine's receiver by more than four times the
    receiver's depth-quantization error (one float32 step of its stored depth, unprojected), or
    lies on a face with a different geometric normal, are excluded and counted. They measure
    where rasterization and rays see different surfaces, chiefly alpha masks tested at a mip
    level by the rasterizer and at level zero by rays, and faces meeting at the pixel center,
    not sky error. All receivers are also split at 'far' renderer units from the camera.
    """
    truth = np.load(truth_path)
    metadata, data = load_capture(capture)
    eye = np.array(metadata['camera_position'])
    engine = data[:, :, 8, :3]
    distance = np.linalg.norm(engine - eye, axis=-1)
    covered, hit, agree = receiver_agreement(truth, metadata, data)
    normal_agree = np.sum(truth['normal'] * data[:, :, 9, :3], axis=-1) >= .99
    regions = {'all_receivers': agree, 'sponza_floor': agree & (data[:, :, 10, 1] > .99) & (data[:, :, 8, 1] < -.12),
               'primary_mismatch_receivers': covered & hit & ~agree}
    if additional_regions:
        regions.update({name: agree & mask for name, mask in additional_regions.items()})
    expected, actual = truth['sky'], data[:, :, component, :3]
    report = dict(covered=int(covered.sum()), primary_mismatch=int((covered & ~agree).sum()),
                  primary_missing=int((covered & ~hit).sum()),
                  shading_normal_mismatch=int((agree & ~normal_agree).sum()))
    # Half the difference of the two independent half estimates has the full estimate's variance.
    noise = (truth['halves'][0] - truth['halves'][1]) / 2
    for name, mask in regions.items():
        if not mask.any():
            continue
        mean = float(expected[mask].mean())
        scale = max(mean, 1e-20)
        error = actual[mask] - expected[mask]
        mse, noise_mse = float((error ** 2).mean()), float((noise[mask] ** 2).mean())
        entry = dict(pixels=int(mask.sum()), truth_mean=mean, bias=float(error.mean() / scale),
                     rms=float(np.sqrt(mse) / scale), p99=float(np.quantile(abs(error), .99) / scale),
                     truth_noise_rms=float(np.sqrt(noise_mse) / scale),
                     # Error RMS with the ground truth's own noise removed (systematic error plus candidate noise).
                     excess_rms=float(np.sqrt(max(mse - noise_mse, 0)) / scale))
        # 8x8 blocks with at least half their pixels in the region: misplaced shadows at low noise.
        h, w = mask.shape[0] // 8 * 8, mask.shape[1] // 8 * 8
        blocks = lambda a: a[:h, :w].reshape(h // 8, 8, w // 8, 8, *a.shape[2:]).sum((1, 3))  # noqa: E731
        weight = blocks(mask.astype(float))
        keep = weight >= 32
        if keep.any():
            region = mask[..., None]
            block_error = (blocks((actual - expected) * region)[keep] / weight[keep][:, None])
            block_noise = (blocks(noise * region)[keep] / weight[keep][:, None])
            block_mse, block_noise_mse = float((block_error ** 2).mean()), float((block_noise ** 2).mean())
            entry['blocks'] = dict(count=int(keep.sum()), rms=float(np.sqrt(block_mse) / scale),
                                   p99=float(np.quantile(abs(block_error), .99) / scale),
                                   truth_noise_rms=float(np.sqrt(block_noise_mse) / scale),
                                   excess_rms=float(np.sqrt(max(block_mse - block_noise_mse, 0)) / scale))
        if name == 'all_receivers':
            # Normalized by the whole region's mean, so the bands are comparable with its excess.
            entry['distance'] = dict(split=far)
            for band, selected in (('near', mask & (distance < far)), ('far', mask & (distance >= far))):
                if selected.any():
                    band_mse = float(((actual[selected] - expected[selected]) ** 2).mean())
                    entry['distance'][band] = dict(pixels=int(selected.sum()), excess_rms=float(
                        np.sqrt(max(band_mse - float((noise[selected] ** 2).mean()), 0)) / scale))
        report[name] = entry
    return report


def embree_cross_check(truth_path, scene_path, count=256, directions=8192, seed=7):
    """Rung-2 cross-check of the ground truth's visibility: the unblocked fraction nu at
    sampled ground-truth receivers, recomputed with tools/environment_reference.py (Embree,
    separate glTF loader and tracer). Mitsuba's nu is itself a 'samples'-ray estimate, so
    its binomial noise is reported alongside the difference."""
    truth = np.load(truth_path)
    if 'geometric' not in truth.files:
        return None
    hit = np.argwhere(truth['position'][..., 3] > 0)
    picks = hit[np.random.default_rng(seed).choice(len(hit), min(count, len(hit)), replace=False)]
    oracle, _ = load_gltf(scene_path)
    mitsuba, embree = [], []
    for y, x in picks:
        p, ns, ng = truth['position'][y, x, :3], truth['normal'][y, x], truth['geometric'][y, x]
        d = cosine_directions(ns.astype(float), directions, seed=1)
        keep = d @ ng > 0
        ids, _, _ = oracle.intersect(p + ng * 4e-6 * (1 + np.abs(p).max()), d[keep].astype(np.float32))
        embree.append(np.sum(ids < 0) / directions)
        mitsuba.append(float(truth['nu'][y, x]))
    mitsuba, embree = np.array(mitsuba), np.array(embree)
    manifest = json.loads(Path(str(truth_path)[:-4] + '.json').read_text())
    noise = np.sqrt(np.maximum(mitsuba * (1 - mitsuba), 1e-12) / manifest['samples'])
    return dict(receivers=len(picks), mean_nu_mitsuba=float(mitsuba.mean()), mean_nu_embree=float(embree.mean()),
                mean_difference=float((mitsuba - embree).mean()), rms_difference=float(np.sqrt(((mitsuba - embree) ** 2).mean())),
                expected_rms_from_noise=float(np.sqrt((noise ** 2).mean())),
                beyond_4_sigma=int(np.sum(np.abs(mitsuba - embree) > 4 * noise + 2 / directions)))


ANALYTIC = ('open_plane', 'forward_open_plane', 'closed_box', 'forward_closed_box', 'single_sided_closed_box',
            'black_environment_plane', 'bright_environment_plane', 'rotated_environment_plane')
CONSISTENCY = ('narrow_slot', 'thin_pole', 'alpha_mask', 'mirrored_two_sided')


def validate_alpha_continuation(mi):
    """A transparent sheet must not teleport a ray past an opaque sheet 20 um behind it.
    Test both primary and secondary traversal, independently of image-noise tolerances.
    """
    primitives = []
    for index, z in enumerate((.1, .10002)):
        positions = np.array([[-1, -1, z], [1, -1, z], [1, 1, z], [-1, 1, z]])
        primitives.append(dict(positions=positions, faces=np.array([[0, 1, 2], [0, 2, 3]]),
                               normals=None, uv=np.zeros((4, 2)), double_sided=True, masked=index == 0,
                               cutoff=.5, alpha=0, vertex_alpha=np.ones(4), texture=None))
    scene, dr, _ = build_scene(mi, primitives, None, Environment(constant=(0, 0, 0)), 16)
    directions = np.array([[0., 0., 1.]])
    valid, points, _, _, _ = primary_hits(mi, dr, scene, [0., 0., 0.], directions)
    assert valid.all() and np.allclose(points[:, 2], .10002, atol=1e-7)
    origin = dr.zeros(mi.SurfaceInteraction3f, 1)
    origin.p, origin.n = mi.Point3f(0), mi.Normal3f(0, 0, 1)
    assert not dr.any(visible(mi, dr, scene, origin, mi.Vector3f(0, 0, 1), mi.Bool(True)))
    return dict(passed=True, opaque_distance=float(points[0, 2]))


def validate_shadow_edge(mi, path):
    """Closed-form box shadow for the bright-edge fixture, independent of its triangles."""
    primitives, alpha, _ = load_gltf_primitives(path)
    scene, dr, _ = build_scene(mi, primitives, alpha, Environment(constant=(0, 0, 0)), 16)
    floor, wall = primitives[0]['positions'], primitives[1]['positions']
    lo, hi = wall.min(0), wall.max(0)
    count = 513
    x = np.linspace(floor[:, 0].min(), floor[:, 0].max(), count, dtype=np.float32)
    y = float(floor[:, 1].min())
    interaction = dr.zeros(mi.SurfaceInteraction3f, count)
    interaction.p, interaction.n = mi.Point3f(mi.Float(x), y, 0), mi.Normal3f(0, 1, 0)
    direction = np.array([.6, .8, 0], np.float32)
    actual = np.array(visible(mi, dr, scene, interaction, mi.Vector3f(*direction.tolist()), mi.Bool(True)))
    offset_y = y + 4e-6 * (1 + np.maximum(abs(x), abs(y)))
    entry = np.maximum((lo[0]-x)/direction[0], 0)
    leave = np.minimum((hi[0]-x)/direction[0], (hi[1]-offset_y)/direction[1])
    expected = leave <= entry
    return dict(rays=count, different=int(np.sum(actual != expected)),
                shadow_edge_x=float(lo[0]-(hi[1]-y)*direction[0]/direction[1]))


def validate_fixtures(mi, folder, output, samples, fixture_set='base'):
    """Gate: the ground truth must reproduce the analytic fixtures before it is used (plan P0)."""
    from validate_hybrid_gi import environment_integral
    from hybrid_gi_fixtures import ALPHA_MATRIX, BRIGHT_EDGES
    names = {'base': ANALYTIC + CONSISTENCY, 'materials': ALPHA_MATRIX,
             'normals': ('normal_map_plane',), 'edges': BRIGHT_EDGES}[fixture_set]
    missing = [name for name in names if not Path(str(folder / name) + '.lighting.json').exists()]
    if missing:
        raise RuntimeError(f'Incomplete ground-truth fixture gate: missing {", ".join(missing)}')
    results, failures = {'alpha_continuation': validate_alpha_continuation(mi)}, []
    for name in names:
        capture = folder / name
        truth = np.load(render_capture(mi, capture, output / name, folder / 'fixtures' / (name + '.gltf'), samples,
                                      engine_normals=fixture_set == 'normals'))
        metadata, data = load_capture(capture)
        mask = (data[:, :, 8, 3] > 0) & (truth['position'][..., 3] > 0)
        sky = truth['sky'][mask]
        entry = dict(pixels=int(mask.sum()), mean=sky.mean(0).tolist(), max=float(sky.max()))
        if fixture_set == 'edges':
            entry['analytic_shadow'] = validate_shadow_edge(mi, folder / 'fixtures' / (name + '.gltf'))
            entry['engine'] = compare(Path(str(output / name) + '.npz'), capture)
            a = entry['engine']['all_receivers']
            entry['engine_passed'] = abs(a['bias']) <= .01 and a['excess_rms'] <= .03 and a['blocks']['excess_rms'] <= .03
            ok = entry['analytic_shadow']['different'] == 0
        elif name == 'normal_map_plane':
            expected = (1 + np.sum(data[:, :, 9, :3][mask]*data[:, :, 10, :3][mask], axis=-1))/2
            entry['expected'] = float(expected.mean())
            entry['error'] = float(abs(sky.mean() - expected.mean()))
            entry['engine'] = compare(Path(str(output / name) + '.npz'), capture)
            ok = entry['error'] <= .005 and abs(entry['engine']['all_receivers']['bias']) <= .02
        elif name in ('open_plane', 'forward_open_plane'):
            entry['error'] = float(abs(sky.mean() - 1))
            ok = entry['error'] <= .005
        elif name in ('closed_box', 'forward_closed_box', 'single_sided_closed_box', 'black_environment_plane'):
            ok = entry['max'] == 0
        elif name in ('bright_environment_plane', 'rotated_environment_plane'):
            expected = environment_integral(capture, metadata, data[:, :, 9, :3][mask][0])
            entry['expected'] = expected.tolist()
            entry['error'] = float(abs(sky.mean(0) - expected).max() / expected.mean())
            ok = entry['error'] <= .005
        else:
            # No closed form: the engine's shipping capture must agree within its bias limit.
            entry['engine'] = compare(Path(str(output / name) + '.npz'), capture)
            ok = abs(entry['engine']['all_receivers']['bias']) <= .02
            if fixture_set == 'materials':
                ok = ok and entry['engine']['primary_mismatch']/max(entry['engine']['covered'],1) <= .005
        entry['passed'] = bool(ok)
        results[name] = entry
        failures += [] if ok else [name]
        print(name, json.dumps(entry), flush=True)
    (output / 'fixtures.json').write_text(json.dumps(results, indent=2) + '\n')
    assert not failures, f'Ground truth failed fixtures: {failures}'
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('capture', type=Path, nargs='?',
                        help='Capture prefix with .gltf (or --scene), .lighting.json, .hybrid.bin, .environment.bin')
    parser.add_argument('--scene', type=Path, help='glTF scene, default <capture>.gltf')
    parser.add_argument('--output', type=Path, required=True, help='Output prefix (a folder with --fixtures)')
    parser.add_argument('--samples', type=int, default=1024, help='Samples per pixel, each an environment and a cosine ray')
    parser.add_argument('--batch', type=int, default=8, help='Samples per pixel per pass')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--environment-resolution', type=int, default=2048)
    parser.add_argument('--compare', type=Path, nargs='*', default=[],
                        help='Captures of the same view to compare (sky component 1); defaults to the input capture')
    parser.add_argument('--fixtures', type=Path, help='validate_hybrid_gi.py output folder: validate against analytic fixtures')
    parser.add_argument('--fixture-set', choices=('base', 'materials', 'normals', 'edges'), default='base')
    parser.add_argument('--compare-only', action='store_true', help='Reuse <output>.npz instead of rendering')
    parser.add_argument('--cross-check', type=int, default=256, help='Receivers for the Embree visibility cross-check (0 disables)')
    parser.add_argument('--receivers', choices=('exact', 'engine'), default='exact',
                        help='Shade exact primary hits (ground truth) or the engine receivers (diagnostic)')
    parser.add_argument('--shading-normals', choices=('vertex', 'engine'), default='vertex',
                        help='Use captured shading normals to check normal mapping with independent geometry and visibility')
    parser.add_argument('--backend', choices=('auto', 'cuda', 'llvm'), default='auto',
                        help='Mitsuba backend: CUDA when an NVIDIA GPU is available (auto), else the CPU LLVM backend')
    args = parser.parse_args()
    if args.compare_only:
        truth = Path(str(args.output) + '.npz')
    else:
        mi = setup_mitsuba(args.backend)
        if args.fixtures:
            args.output.mkdir(parents=True, exist_ok=True)
            validate_fixtures(mi, args.fixtures, args.output, args.samples, args.fixture_set)
            return
        assert args.capture, 'A capture prefix is required unless --fixtures is given'
        truth = render_capture(mi, args.capture, args.output, args.scene, args.samples, args.batch, args.seed,
                               args.environment_resolution, args.receivers == 'engine', args.shading_normals == 'engine')
    reports = {}
    for index, capture in enumerate(args.compare or [args.capture]):
        reports[str(capture)] = compare(truth, capture)
        comparison_image(truth, capture, Path(f'{args.output}-compare-{index}.png'))
    if args.cross_check and args.capture:
        reports['embree_cross_check'] = embree_cross_check(truth, args.scene or Path(str(args.capture) + '.gltf'), args.cross_check)
    Path(str(args.output) + '-compare.json').write_text(json.dumps(reports, indent=2) + '\n')
    print(json.dumps(reports, indent=2))


def comparison_image(truth_path, capture, path):
    """Ground truth | capture sky | signed error (red: capture brighter, blue: darker; full scale 50% of the mean)."""
    truth = np.load(truth_path)['sky']
    _, data = load_capture(capture)
    actual = data[:, :, 1, :3]
    covered = data[:, :, 8, 3] > 0
    mean = max(float(truth[covered].mean()), 1e-20)
    tone = lambda x: (np.clip(x / (4 * mean), 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)  # noqa: E731
    error = (actual - truth).mean(2) / mean
    signed = np.zeros(error.shape + (3,), np.uint8)
    signed[..., 0] = (np.clip(error / .5, 0, 1) * 255).astype(np.uint8)
    signed[..., 2] = (np.clip(-error / .5, 0, 1) * 255).astype(np.uint8)
    Image.fromarray(np.concatenate([tone(truth), tone(actual), signed], 1)).save(path)


if __name__ == '__main__':
    main()
    # Release scenes while Dr.Jit is still alive: after many scenes, interpreter teardown
    # otherwise crashed in Dr.Jit (observed on Windows with Mitsuba 3.9.1 / Dr.Jit 1.5.0).
    if 'drjit' in sys.modules:
        import gc
        import drjit
        gc.collect()
        drjit.sync_thread()
        drjit.flush_kernel_cache()
        drjit.flush_malloc_cache()
