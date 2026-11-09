"""Configuration-only CUDA diffuse reference for GroundTruthWorkflowPlan.md.

All path shading, texture filtering, light selection and alpha traversal stay on
the GPU. NumPy is used only to load assets and transfer finished images.
"""
import base64
import io
import json
import time
from pathlib import Path
from urllib.parse import unquote

import numpy as np
from PIL import Image

from environment_reference import Environment, normalize
from ground_truth_mitsuba import build_scene, load_gltf_primitives, primary_hits, PRIMITIVE_ATTRIBUTE
from ground_truth_bounce import closest, offset


def camera_rays(setup):
    camera, run = setup['camera'], setup['run']
    if camera['orthographic']:
        raise ValueError('The ground-truth workflow requires perspective cameras')
    width, height = run['renderWidth'], run['renderHeight']
    if not np.isclose(camera['aspect'], width / height):
        raise ValueError('Camera aspect and render extent disagree')
    eye = np.array(camera['eye'], float)
    forward = normalize(np.array(camera['target']) - eye)
    right = normalize(np.cross(forward, [0, 1, 0]))
    up = np.cross(right, forward)
    x, y = np.meshgrid((np.arange(width)+.5)*2/width-1, 1-(np.arange(height)+.5)*2/height)
    tangent = np.tan(np.deg2rad(camera['verticalFovDegrees']) / 2)
    directions = normalize(forward + tangent * (x[..., None]*right*width/height + y[..., None]*up))
    directions = directions.reshape(-1, 3)
    origins = eye + directions * (camera['nearPlane'] / (directions @ forward))[:, None]
    return eye, directions, origins


class PanoramaRadiance:
    def __init__(self, mi, dr, environment):
        self.mi, self.dr, self.environment = mi, dr, environment
        bitmap = np.asarray(environment.equirect, np.float32)
        self.height = bitmap.shape[0]
        self.width = bitmap.shape[1]
        self.texels = mi.Float(bitmap[..., :3].ravel())

    def eval(self, direction):
        mi, dr, env = self.mi, self.dr, self.environment
        c, s = float(np.cos(env.rotation)), float(np.sin(env.rotation))
        x, z = c*direction.x-s*direction.z, s*direction.x+c*direction.z
        px = (dr.atan2(z, x)/(2*dr.pi)+.5)*self.width-.5
        py = dr.acos(dr.clip(direction.y, -1, 1))/dr.pi*self.height-.5
        x0, y0 = dr.floor(px), dr.floor(py)
        fx, fy = px-x0, py-y0
        result = dr.zeros(mi.Color3f, dr.width(direction))
        for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
            ix = mi.UInt32(x0+dx+self.width) % self.width
            iy = mi.UInt32(dr.clip(y0+dy, 0, self.height-1))
            base = (iy*self.width+ix)*3
            weight = (fx if dx else 1-fx)*(fy if dy else 1-fy)
            result += mi.Color3f(*[dr.gather(mi.Float, self.texels, base+k) for k in range(3)])*weight
        return result * env.intensity


class Materials:
    def __init__(self, mi, dr, path, primitives):
        self.mi, self.dr = mi, dr
        path = Path(path)
        doc = json.loads(path.read_text(encoding='utf-8'))

        def uri_bytes(uri):
            return (base64.b64decode(uri.split(',', 1)[1]) if uri.startswith('data:') else
                    (Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:') else path.parent/unquote(uri)).read_bytes())

        cache = {}
        self.owned = []

        def texture(info, srgb):
            key = (info['index'], srgb) if info else (None, False)
            if key not in cache:
                if info:
                    source = doc['textures'][info['index']]
                    image = doc['images'][source['source']]
                    if 'uri' in image:
                        encoded = uri_bytes(image['uri'])
                    else:
                        view = doc['bufferViews'][image['bufferView']]
                        data = uri_bytes(doc['buffers'][view['buffer']]['uri'])
                        start = view.get('byteOffset', 0)
                        encoded = data[start:start+view['byteLength']]
                    pixels = np.asarray(Image.open(io.BytesIO(encoded)).convert('RGB'), np.float32)/255
                    if srgb:
                        pixels = np.where(pixels <= .04045, pixels/12.92, ((pixels+.055)/1.055)**2.4)
                    sampler = doc.get('samplers', [{}])[source['sampler']] if 'sampler' in source else {}
                    if sampler.get('wrapS', 10497) != sampler.get('wrapT', 10497):
                        raise ValueError('Mixed S/T wrap modes need a separate material fixture')
                    wrap = {33071: 'clamp', 33648: 'mirror'}.get(sampler.get('wrapS', 10497), 'repeat')
                    value = mi.load_dict(dict(type='bitmap', bitmap=mi.Bitmap(pixels), raw=True,
                                             filter_type='bilinear', wrap_mode=wrap))
                else:
                    value = mi.load_dict(dict(type='rgb', value=1.0))
                cache[key] = value
                self.owned.append(value)
            return cache[key]

        colors, metals, factors, metallic = [], [], [], []
        for primitive in primitives:
            material = doc['materials'][primitive['material']] if primitive['material'] >= 0 else {}
            if 'KHR_materials_pbrSpecularGlossiness' in material.get('extensions', {}):
                raise ValueError('Specular/glossiness materials need a separate diffuse contract fixture')
            pbr = material.get('pbrMetallicRoughness', {})
            colors.append(texture(pbr.get('baseColorTexture'), True))
            metals.append(texture(pbr.get('metallicRoughnessTexture'), False))
            factors.append(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3])
            metallic.append(pbr.get('metallicFactor', 1))
        self.colors, self.metals = mi.TexturePtr(colors), mi.TexturePtr(metals)
        self.factors = mi.Color3f(*np.asarray(factors, np.float32).T)
        self.metallic = mi.Float(metallic)

    def eval(self, hit, active, albedo_only=False):
        mi, dr = self.mi, self.dr
        index = mi.UInt32(hit.shape.eval_attribute_1(PRIMITIVE_ATTRIBUTE, hit, active))
        color = dr.gather(mi.TexturePtr, self.colors, index).eval_3(hit, active)
        color *= dr.gather(mi.Color3f, self.factors, index)
        color *= hit.shape.eval_attribute_3('vertex_color', hit, active)
        if albedo_only:
            return color
        metal_hit = mi.SurfaceInteraction3f(hit)
        uv = hit.shape.eval_attribute_3('vertex_metal_uv', hit, active)
        metal_hit.uv = mi.Point2f(uv.x, uv.y)
        metal = dr.gather(mi.TexturePtr, self.metals, index).eval_3(metal_hit, active).z
        metal *= dr.gather(mi.Float, self.metallic, index)
        # Same Lambertian secondary diffuse contract as the radiance cache.
        return color * (.96 * (1-dr.clip(metal, 0, 1)))


def light_sample(mi, dr, light, points):
    if light['type'] == 0:
        direction = mi.Vector3f(*normalize(-np.array(light['direction'])).tolist())
        distance, falloff = None, 1
    elif light['type'] == 1:
        delta = mi.Point3f(*light['position']) - points
        distance = dr.norm(delta)
        direction = delta / dr.maximum(distance, 1e-10)
        falloff = 1/dr.maximum(distance*distance, 1e-6)
        if light['range'] > 0:
            falloff *= dr.maximum(1-dr.power(distance/light['range'], 4), 0)
    else:
        raise ValueError('Phase 1 supports directional and point lights')
    radiance = mi.Color3f(*light['color']) * light['intensity'] * falloff
    return direction, distance, radiance


def direct(mi, dr, scene, points, ng, ns, lights, active, choice=None):
    result = dr.zeros(mi.Color3f, dr.width(points))
    powers = np.array([max(np.dot(light['color'], [.2126, .7152, .0722])*light['intensity'], 1e-12)
                       for light in lights])
    probabilities = powers/powers.sum() if len(powers) else powers
    cdf = np.r_[0, np.cumsum(probabilities)]
    for index, light in enumerate(lights):
        direction, distance, radiance = light_sample(mi, dr, light, points)
        cosine = dr.maximum(dr.dot(ns, direction), 0)
        enabled = active & (cosine > 0) & (dr.dot(ng, direction) > 0)
        if choice is not None:
            enabled &= (choice >= float(cdf[index])) & (choice < float(cdf[index+1]))
            radiance /= float(probabilities[index])
        if light['castsShadows']:
            hit = closest(mi, dr, scene, offset(mi, dr, points, ng, direction, distance), enabled)
            enabled &= ~hit.is_valid()
        result += dr.select(enabled, radiance*cosine/dr.pi, 0)
    return result


def environment_sample(mi, dr, scene, proposal, radiance, point, geometric, shading, sampler, active):
    """One-sample balance MIS: equal environment/cosine mixture, one shadow ray."""
    interaction = dr.zeros(mi.SurfaceInteraction3f, dr.width(point))
    interaction.p, interaction.n = point, geometric
    sample, _ = proposal.sample_emitter_direction(interaction, sampler.next_2d(), False)
    cosine_direction = mi.Frame3f(shading).to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
    direction = dr.select(sampler.next_1d() < .5, sample.d, cosine_direction)
    cosine = dr.maximum(dr.dot(shading, direction), 0)
    sample.d = direction
    density = .5*(proposal.environment().pdf_direction(interaction, sample)+cosine/dr.pi)
    eligible = active & (cosine > 0) & (dr.dot(geometric, direction) > 0) & (density > 0)
    blocker = closest(mi, dr, scene, offset(mi, dr, point, geometric, direction), eligible)
    return dr.select(eligible & ~blocker.is_valid(), radiance.eval(direction)*cosine/(dr.pi*dr.maximum(density, 1e-20)), 0)


def check_split_samples(samples, batch):
    """Both independent halves need the same number of passes."""
    if samples < 2*batch or samples % (2*batch):
        raise ValueError('Samples must be a positive multiple of twice the batch size')


def sky(mi, dr, scene, proposal, radiance, points, ng, ns, samples, batch, seed):
    check_split_samples(samples, batch)
    count, lanes = len(points), len(points)*batch
    def vector(kind, values):
        return kind(*[mi.Float(np.repeat(values[:, i], batch).astype(np.float32)) for i in range(3)])
    p, g, n = vector(mi.Point3f, points), vector(mi.Normal3f, ng), vector(mi.Normal3f, ns)
    sampler = mi.load_dict({'type': 'independent'})
    halves = [dr.zeros(mi.Color3f, lanes), dr.zeros(mi.Color3f, lanes)]
    passes = samples//batch
    for index in range(passes):
        sampler.seed(dr.opaque(mi.UInt32, (seed*1000003+index) & 0xffffffff), lanes)
        halves[index % 2] += environment_sample(mi, dr, scene, proposal, radiance, p, g, n, sampler, mi.Bool(True))
        dr.eval(halves[index % 2])
    split = np.stack([np.array(value).T.reshape(count, batch, 3).mean(1)/(passes//2) for value in halves])
    return split.mean(0), split


def bounce(mi, dr, scene, materials, environment, points, ng, ns, lights, samples, batch, seed, proposal=None):
    check_split_samples(samples, batch)
    count, lanes = len(points), len(points)*batch
    def vector(kind, values):
        return kind(*[mi.Float(np.repeat(values[:, i], batch).astype(np.float32)) for i in range(3)])
    point, geometric, shading = vector(mi.Point3f, points), vector(mi.Normal3f, ng), vector(mi.Normal3f, ns)
    frame = mi.Frame3f(shading)
    sampler = mi.load_dict({'type': 'independent'})
    halves = [dr.zeros(mi.Color3f, lanes), dr.zeros(mi.Color3f, lanes)]
    passes = samples//batch
    for index in range(passes):
        sampler.seed(dr.opaque(mi.UInt32, (seed*1000003+index) & 0xffffffff), lanes)
        direction = frame.to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
        hit = closest(mi, dr, scene, offset(mi, dr, point, geometric, direction), dr.dot(geometric, direction) > 0)
        valid = hit.is_valid()
        facing = dr.dot(hit.n, direction) < 0
        normal = dr.select(valid, dr.select(facing, hit.n, -hit.n), mi.Normal3f(0, 1, 0))
        smooth = dr.select(valid, dr.select(facing, hit.sh_frame.n, -hit.sh_frame.n), mi.Normal3f(0, 1, 0))
        position = dr.select(valid, hit.p, mi.Point3f(0))
        value = direct(mi, dr, scene, position, normal, smooth, lights, valid, sampler.next_1d())
        if proposal is not None:
            value += environment_sample(mi, dr, scene, proposal, environment, position, normal, smooth, sampler, valid)
        halves[index % 2] += dr.select(valid, value*materials.eval(hit, valid), 0)
        dr.eval(halves[index % 2])
    split = np.stack([np.array(value).T.reshape(count, batch, 3).mean(1)/(passes//2) for value in halves])
    return split.mean(0), split


class Reference:
    def __init__(self, mi, source):
        self.mi = mi
        import drjit as dr
        self.dr = dr
        self.primitives, self.alpha, self.normalization = load_gltf_primitives(source, material_inputs=True)
        self.source = source
        self.materials = None
        self.scene, _, _ = build_scene(mi, self.primitives, self.alpha, Environment(constant=(0, 0, 0)), 4)

    def configure(self, setup):
        mi, dr = self.mi, self.dr
        settings = setup['rendering']
        if not np.allclose(settings['normalizationCenter'], self.normalization['center'], atol=1e-5) or not np.isclose(
                settings['normalizationScale'], self.normalization['scale'], rtol=1e-5):
            raise ValueError('Setup normalization does not match the source scene')
        env = settings['environment']
        pixels = np.array(mi.Bitmap(env['texturePath']).convert(mi.Bitmap.PixelFormat.RGB,
                                                               mi.Struct.Type.Float32, False))
        self.environment = Environment(equirect=pixels, rotation=np.deg2rad(env['rotationDegrees']),
                                       intensity=env['intensity']*env['lighting'])
        self.radiance = PanoramaRadiance(mi, dr, self.environment)
        # Only the proposal is resampled. Radiance always uses the source HDR.
        proposal, _, self.black = build_scene(mi, [], self.alpha, self.environment, 1024)
        # Retain the geometry/BVH across lighting setups; the sampling scene owns just its emitter.
        self.proposal = proposal
        self.lights = [entry['light'] for entry in settings['lights'] if entry['light']['enabled']]

    def primary(self, setup, return_interaction=False):
        eye, directions, origins = camera_rays(setup)
        return primary_hits(self.mi, self.dr, self.scene, eye, directions, origins=origins, return_interaction=return_interaction)

    def render(self, setup, samples=256, batch=16, seed=1):
        check_split_samples(samples, batch)
        started = time.perf_counter()
        load_seconds = 0
        if self.materials is None:
            self.materials = Materials(self.mi, self.dr, self.source, self.primitives)
            load_seconds = time.perf_counter()-started
            started = time.perf_counter()
        self.configure(setup)
        valid, points, ng, ns, primitive, interaction = self.primary(setup, return_interaction=True)
        mi, dr = self.mi, self.dr
        p, g, s = points[valid], ng[valid], ns[valid]
        result = dict(valid=valid, position=np.where(valid[:, None], points, 0),
                      geometric=np.where(valid[:, None], ng, 0), normal=np.where(valid[:, None], ns, 0), primitive=primitive)
        result['albedo'] = np.array(dr.select(interaction.is_valid(),
                                              self.materials.eval(interaction, interaction.is_valid(), albedo_only=True), 0)).T
        times = dict(material_load=load_seconds, primary=time.perf_counter()-started)
        started = time.perf_counter()
        d = direct(mi, dr, self.scene, mi.Point3f(*p.T), mi.Normal3f(*g.T), mi.Normal3f(*s.T),
                   self.lights, mi.Bool(True))
        values = dict(D_direct=np.array(d).T)
        times['direct'] = time.perf_counter()-started
        started = time.perf_counter()
        if self.black:
            sky_value, sky_halves = np.zeros_like(p), np.zeros((2, *p.shape))
        else:
            sky_value, sky_halves = sky(mi, dr, self.scene, self.proposal, self.radiance, p, g, s, samples, batch, seed)
        values.update(D_sky=sky_value)
        times['sky'] = time.perf_counter()-started
        started = time.perf_counter()
        bounced, bounce_halves = bounce(mi, dr, self.scene, self.materials, self.radiance,
                                         p, g, s, self.lights, samples, batch, seed+1009,
                                         None if self.black else self.proposal)
        values['D_bounce'] = bounced
        times['bounce'] = time.perf_counter()-started
        for key, value in values.items():
            result[key] = np.zeros_like(points, dtype=np.float32)
            result[key][valid] = value
        for key, value in [('D_sky_halves', sky_halves), ('D_bounce_halves', bounce_halves)]:
            result[key] = np.zeros((2, *points.shape), np.float32)
            result[key][:, valid] = value
        h, w = setup['run']['renderHeight'], setup['run']['renderWidth']
        result = {key: value.reshape(((2, h, w, 3) if key.endswith('_halves') else
                                      (h, w, 3) if value.ndim == 2 else (h, w))) for key, value in result.items()}
        if any(not np.isfinite(value).all() for value in result.values()):
            raise ValueError('Non-finite reference output')
        return result, times


