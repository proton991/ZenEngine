"""Full multi-bounce CUDA path-traced reference for one native GI setup.

Mitsuba 3 provides only ray intersection and environment importance sampling. Shading uses
the engine's analytic material model at every path vertex: unclamped GGX with alpha = roughness^2,
Smith-Schlick G with the engine's image-based-lighting k = alpha / 2 (direct radiance at the
primary vertex, a consistency check, uses its direct-light k = (roughness + 1)^2 / 8),
Schlick Fresnel on H.V, a Lambert lobe weighted
by (1 - F)(1 - metallic), and roughness clamped to [0.04, 1]. Shading normals are interpolated
vertex normals, so compare against engine captures with normal maps off. Point and directional
lights use shadowed next-event estimation (one light per vertex, chosen by unshadowed
contribution, plus every light at the primary vertex). The environment uses balance-heuristic
MIS between environment and BSDF sampling. Emissive surfaces are found by BSDF sampling.
Paths continue to --max-depth vertices with Russian roulette from the third bounce.

The reference is evaluated at the centers of the render-resolution pixels (one path per pixel
per pass) and reported both per pixel and box-averaged by --footprint, which matches a
box-downsampled engine capture exactly. Terms, in the engine lighting capture's units:

  D_direct            irradiance/pi from analytic lights           (capture component 7)
  D_indirect          irradiance/pi from everything else           (components 8 + 9)
  L_direct            outgoing radiance from analytic lights       (component 1)
  L_diffuse_indirect  outgoing Lambert-lobe radiance, indirect     (component 2)
  L_specular_indirect outgoing GGX-lobe radiance, indirect         (component 3)
  L_emission          surface emission                             (component 4)
  L_total             sum of the outgoing terms                    (component 0)
"""
import argparse
import json
import time
from pathlib import Path

import numpy as np

from environment_reference import Environment
from ground_truth_bounce import closest, offset
from ground_truth_mitsuba import PRIMITIVE_ATTRIBUTE, build_scene, load_gltf_primitives, primary_hits, setup_mitsuba
from ground_truth_reference import Materials, PanoramaRadiance, camera_rays, light_sample

TERMS = ('D_direct', 'D_indirect', 'L_direct', 'L_diffuse_indirect', 'L_specular_indirect', 'L_emission', 'L_total')
SAMPLED = ('D_indirect', 'L_diffuse_indirect', 'L_specular_indirect')


def luminance(color):
    return color[0] * .2126 + color[1] * .7152 + color[2] * .0722


class FullMaterials(Materials):
    """Albedo, metallic, roughness and emission of glTF metallic-roughness materials."""

    def __init__(self, mi, dr, path, primitives):
        super().__init__(mi, dr, path, primitives)
        doc = json.loads(Path(path).read_text(encoding='utf-8'))
        roughness, emission = [], []
        for primitive in primitives:
            material = doc['materials'][primitive['material']] if primitive['material'] >= 0 else {}
            if 'emissiveTexture' in material:
                raise ValueError('Emissive textures need a separate reference fixture')
            strength = material.get('extensions', {}).get('KHR_materials_emissive_strength', {}).get('emissiveStrength', 1)
            roughness.append(material.get('pbrMetallicRoughness', {}).get('roughnessFactor', 1))
            emission.append(np.asarray(material.get('emissiveFactor', [0, 0, 0]), np.float64) * strength)
        self.roughness = mi.Float(np.asarray(roughness, np.float32))
        self.emission = mi.Color3f(*np.asarray(emission, np.float32).T)

    def evaluate(self, hit, active):
        mi, dr = self.mi, self.dr
        # The interpolated per-vertex index can read 311.99998: round it, or truncation picks
        # the previous primitive's material.
        index = mi.UInt32(dr.round(hit.shape.eval_attribute_1(PRIMITIVE_ATTRIBUTE, hit, active)))
        albedo = dr.gather(mi.TexturePtr, self.colors, index).eval_3(hit, active)
        albedo *= dr.gather(mi.Color3f, self.factors, index)
        albedo *= hit.shape.eval_attribute_3('vertex_color', hit, active)
        metal_hit = mi.SurfaceInteraction3f(hit)
        uv = hit.shape.eval_attribute_3('vertex_metal_uv', hit, active)
        metal_hit.uv = mi.Point2f(uv.x, uv.y)
        packed = dr.gather(mi.TexturePtr, self.metals, index).eval_3(metal_hit, active)
        metallic = dr.clip(packed.z * dr.gather(mi.Float, self.metallic, index), 0, 1)
        roughness = dr.clip(packed.y * dr.gather(mi.Float, self.roughness, index), .04, 1)
        emission = dr.gather(mi.Color3f, self.emission, index)
        return albedo, metallic, roughness, emission


class PathReference:
    def __init__(self, mi, setup, max_depth, contract=False, lambert=False):
        import drjit as dr
        self.mi, self.dr, self.setup, self.max_depth = mi, dr, setup, max_depth
        # Validation only: tools/ground_truth_reference.py's one-bounce contract (Lambert
        # 0.96 * (1 - metallic) secondaries, no emission, one secondary vertex).
        self.contract = contract
        # Diagnostic: the world cache's secondary material (Lambert 0.96 * (1 - metallic), no
        # specular) with every bounce and emission retained.
        self.lambert = lambert or contract
        if contract:
            self.max_depth = 1
        rendering = setup['rendering']
        source = rendering['scenePath']
        primitives, alpha, normalization = load_gltf_primitives(source, material_inputs=True)
        if not np.allclose(rendering['normalizationCenter'], normalization['center'], atol=1e-5) or not np.isclose(
                rendering['normalizationScale'], normalization['scale'], rtol=1e-5):
            raise ValueError('Setup normalization does not match the source scene')
        if rendering.get('normalMaps', True):
            raise ValueError('The reference uses vertex normals: render the engine with normal maps off')
        self.scene, _, _ = build_scene(mi, primitives, alpha, Environment(constant=(0, 0, 0)), 4)
        self.materials = FullMaterials(mi, dr, source, primitives)
        env = rendering['environment']
        pixels = np.array(mi.Bitmap(env['texturePath']).convert(mi.Bitmap.PixelFormat.RGB, mi.Struct.Type.Float32, False))
        self.environment = Environment(equirect=pixels, rotation=np.deg2rad(env['rotationDegrees']),
                                       intensity=env['intensity'] * env['lighting'])
        self.radiance = PanoramaRadiance(mi, dr, self.environment)
        self.proposal, _, self.black = build_scene(mi, [], alpha, self.environment, 1024)
        self.lights = [entry['light'] for entry in rendering['lights'] if entry['light']['enabled']]
        for light in self.lights:
            if light['type'] not in (0, 1):
                raise ValueError('The path reference supports directional and point lights')

    # ---------- engine material model ----------
    def brdf(self, n, v, l, albedo, metallic, roughness, direct=False):
        mi, dr = self.mi, self.dr
        h = dr.normalize(v + l)
        ndl = dr.maximum(dr.dot(n, l), 0)
        ndv_g = dr.maximum(dr.dot(n, v), 0)
        ndv = dr.maximum(dr.dot(n, v), .001)
        ndh = dr.maximum(dr.dot(n, h), 0)
        hdv = dr.maximum(dr.dot(h, v), 0)
        a = roughness * roughness
        a2 = a * a
        d = ndh * ndh * (a2 - 1) + 1
        # Unclamped GGX. The engine's direct-light DistributionGGX divides by max(pi d^2, 1e-6),
        # which caps the lobe peak below roughness ~0.155; that is not the reference material.
        distribution = a2 / dr.maximum(dr.pi * d * d, 1e-30)
        # Engine conventions: direct lights use k = (r + 1)^2 / 8; image-based lighting (the
        # split-sum LUT) uses the standard k = alpha / 2. Indirect transport uses the latter.
        k = (roughness + 1) * (roughness + 1) * .125 if direct else roughness * roughness * .5
        geometry = ndv_g / (ndv_g * (1 - k) + k) * (ndl / (ndl * (1 - k) + k))
        f0 = mi.Color3f(.04) * (1 - metallic) + albedo * metallic
        fresnel = f0 + (1 - f0) * dr.power(1 - hdv, 5)
        specular = distribution * geometry * fresnel / dr.maximum(4 * ndv * ndl, 1e-6)
        diffuse = (1 - fresnel) * (1 - metallic) * albedo / dr.pi
        return diffuse, specular

    def secondary_brdf(self, n, v, l, albedo, metallic, roughness):
        if not self.lambert:
            return self.brdf(n, v, l, albedo, metallic, roughness)
        return albedo / self.dr.pi, self.mi.Color3f(0)

    def ggx_pdf(self, n, v, l, roughness):
        dr = self.dr
        h = dr.normalize(v + l)
        ndh = dr.maximum(dr.dot(n, h), 0)
        hdv = dr.maximum(dr.dot(h, v), 1e-6)
        a2 = roughness * roughness * roughness * roughness
        d = ndh * ndh * (a2 - 1) + 1
        return dr.select(dr.dot(n, l) > 0, a2 / (dr.pi * d * d) * ndh / (4 * hdv), 0)

    def ggx_sample(self, n, v, roughness, u):
        mi, dr = self.mi, self.dr
        a2 = roughness * roughness * roughness * roughness
        cosine = dr.sqrt((1 - u.x) / (1 + (a2 - 1) * u.x))
        sine = dr.sqrt(dr.maximum(1 - cosine * cosine, 0))
        phi = 2 * dr.pi * u.y
        h = mi.Frame3f(n).to_world(mi.Vector3f(sine * dr.cos(phi), sine * dr.sin(phi), cosine))
        return 2 * dr.dot(v, h) * h - v

    def specular_probability(self, n, v, albedo, metallic):
        dr = self.dr
        ndv = dr.maximum(dr.dot(n, v), 0)
        f0 = .04 * (1 - metallic) + albedo * metallic
        fresnel = luminance(f0 + (1 - f0) * dr.power(1 - ndv, 5))
        diffuse = luminance((1 - metallic) * albedo)
        return dr.clip(fresnel / dr.maximum(fresnel + diffuse, 1e-6), .1, .9)

    def mixture_pdf(self, n, v, l, roughness, probability):
        dr = self.dr
        return (1 - probability) * dr.maximum(dr.dot(n, l), 0) / dr.pi + probability * self.ggx_pdf(n, v, l, roughness)

    def mixture_sample(self, n, v, roughness, probability, sampler):
        mi, dr = self.mi, self.dr
        cosine = mi.Frame3f(n).to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
        glossy = self.ggx_sample(n, v, roughness, sampler.next_2d())
        return dr.select(sampler.next_1d() < probability, glossy, cosine)

    # ---------- environment ----------
    def environment_pdf(self, point, geometric, direction):
        mi, dr = self.mi, self.dr
        if self.black:
            return dr.zeros(mi.Float, dr.width(point))
        interaction = dr.zeros(mi.SurfaceInteraction3f, dr.width(point))
        interaction.p, interaction.n = point, geometric
        sample = dr.zeros(mi.DirectionSample3f, dr.width(point))
        sample.d = direction
        return self.proposal.environment().pdf_direction(interaction, sample)

    def environment_sample(self, point, geometric, shading, sampler, active):
        mi, dr = self.mi, self.dr
        interaction = dr.zeros(mi.SurfaceInteraction3f, dr.width(point))
        interaction.p, interaction.n = point, geometric
        sample, _ = self.proposal.sample_emitter_direction(interaction, sampler.next_2d(), False)
        direction = sample.d
        ok = active & (dr.dot(geometric, direction) > 0) & (dr.dot(shading, direction) > 0) & (sample.pdf > 0)
        blocker = closest(mi, dr, self.scene, offset(mi, dr, point, geometric, direction), ok)
        ok &= ~blocker.is_valid()
        return direction, sample.pdf, self.radiance.eval(direction), ok

    # ---------- analytic lights ----------
    def all_lights(self, point, geometric, shading, view, albedo, metallic, roughness, active):
        """Every light with an exact shadow ray: irradiance/pi and outgoing radiance."""
        mi, dr = self.mi, self.dr
        irradiance = dr.zeros(mi.Color3f, dr.width(point))
        outgoing = dr.zeros(mi.Color3f, dr.width(point))
        for light in self.lights:
            direction, distance, radiance = light_sample(mi, dr, light, point)
            cosine = dr.maximum(dr.dot(shading, direction), 0)
            enabled = active & (cosine > 0) & (dr.dot(geometric, direction) > 0)
            if light['castsShadows']:
                blocker = closest(mi, dr, self.scene, offset(mi, dr, point, geometric, direction, distance), enabled)
                enabled &= ~blocker.is_valid()
            diffuse, specular = self.brdf(shading, view, direction, albedo, metallic, roughness, direct=True)
            irradiance += dr.select(enabled, radiance * cosine / dr.pi, 0)
            outgoing += dr.select(enabled, (diffuse + specular) * radiance * cosine, 0)
            dr.eval(irradiance, outgoing)
        return irradiance, outgoing

    def one_light(self, point, geometric, shading, view, albedo, metallic, roughness, sampler, active):
        """One light chosen by unshadowed contribution, with its shadow ray."""
        mi, dr = self.mi, self.dr
        width = dr.width(point)
        samples, total = [], dr.zeros(mi.Float, width)
        for light in self.lights:
            direction, distance, radiance = light_sample(mi, dr, light, point)
            weight = dr.select(active & (dr.dot(geometric, direction) > 0),
                               luminance(radiance) * dr.maximum(dr.dot(shading, direction), 0), 0)
            samples.append((light, direction, distance, radiance, weight))
            total += weight
        pick = sampler.next_1d() * total
        chosen = dr.zeros(mi.Bool, width)
        direction = dr.zeros(mi.Vector3f, width)
        radiance = dr.zeros(mi.Color3f, width)
        distance = dr.full(mi.Float, 1e30, width)
        weight = dr.zeros(mi.Float, width)
        shadowed = dr.zeros(mi.Bool, width)
        cumulative = dr.zeros(mi.Float, width)
        for light, light_direction, light_distance, light_radiance, light_weight in samples:
            cumulative += light_weight
            take = ~chosen & (light_weight > 0) & (pick < cumulative)
            direction = dr.select(take, light_direction, direction)
            radiance = dr.select(take, light_radiance, radiance)
            if light_distance is not None:
                distance = dr.select(take, light_distance, distance)
            weight = dr.select(take, light_weight, weight)
            shadowed = dr.select(take, mi.Bool(bool(light['castsShadows'])), shadowed)
            chosen |= take
        enabled = chosen & (weight > 0)
        blocker = closest(mi, dr, self.scene, offset(mi, dr, point, geometric, direction, distance), enabled & shadowed)
        enabled &= ~(shadowed & blocker.is_valid())
        diffuse, specular = self.secondary_brdf(shading, view, direction, albedo, metallic, roughness)
        cosine = dr.maximum(dr.dot(shading, direction), 0)
        return dr.select(enabled, (diffuse + specular) * radiance * cosine * total / dr.maximum(weight, 1e-30), 0)

    # ---------- paths ----------
    def continue_path(self, hit, incoming, active, sampler):
        """Radiance leaving each secondary hit toward the previous vertex (all bounces)."""
        mi, dr = self.mi, self.dr
        width = dr.width(incoming)
        result = dr.zeros(mi.Color3f, width)
        throughput = dr.full(mi.Color3f, 1, width)
        valid = active & hit.is_valid()
        for depth in range(self.max_depth):
            facing = dr.dot(hit.n, incoming) < 0
            geometric = dr.select(facing, hit.n, -hit.n)
            shading = dr.select(facing, hit.sh_frame.n, -hit.sh_frame.n)
            point = hit.p
            view = -incoming
            albedo, metallic, roughness, emission = self.materials.evaluate(hit, valid)
            if self.lambert:
                albedo, metallic = albedo * (.96 * (1 - metallic)), mi.Float(0)
            if self.contract:
                emission = mi.Color3f(0)
            result += dr.select(valid, throughput * emission, 0)
            result += dr.select(valid, throughput * self.one_light(point, geometric, shading, view, albedo, metallic,
                                                                   roughness, sampler, valid), 0)
            probability = self.specular_probability(shading, view, albedo, metallic)
            if self.lambert:
                probability = mi.Float(0)
            if not self.black:
                direction, pdf, radiance, ok = self.environment_sample(point, geometric, shading, sampler, valid)
                diffuse, specular = self.secondary_brdf(shading, view, direction, albedo, metallic, roughness)
                bsdf_pdf = self.mixture_pdf(shading, view, direction, roughness, probability)
                cosine = dr.maximum(dr.dot(shading, direction), 0)
                result += dr.select(ok, throughput * (diffuse + specular) * radiance * cosine / (pdf + bsdf_pdf), 0)
            direction = self.mixture_sample(shading, view, roughness, probability, sampler)
            pdf = self.mixture_pdf(shading, view, direction, roughness, probability)
            cosine = dr.dot(shading, direction)
            ok = valid & (cosine > 0) & (dr.dot(geometric, direction) > 0) & (pdf > 0)
            diffuse, specular = self.secondary_brdf(shading, view, direction, albedo, metallic, roughness)
            throughput *= dr.select(ok, (diffuse + specular) * cosine / dr.maximum(pdf, 1e-30), 0)
            if depth >= 2:
                survive = dr.clip(dr.maximum(dr.maximum(throughput[0], throughput[1]), throughput[2]), .05, .95)
                ok &= sampler.next_1d() < survive
                throughput /= survive
            next_hit = closest(mi, dr, self.scene, offset(mi, dr, point, geometric, direction), ok)
            escaped = ok & ~next_hit.is_valid()
            if not self.black:
                environment = self.environment_pdf(point, geometric, direction)
                result += dr.select(escaped, throughput * self.radiance.eval(direction) * pdf / (pdf + environment), 0)
            valid = ok & next_hit.is_valid()
            hit, incoming = next_hit, direction
            dr.eval(result, throughput, valid, hit, incoming)
            if not dr.any(valid):
                break
        return result

    def sample_indirect(self, primary, sampler):
        """One estimate of the indirect terms at every primary receiver."""
        mi, dr = self.mi, self.dr
        point, geometric, shading, view, albedo, metallic, roughness, valid = primary
        width = dr.width(point)
        terms = {name: dr.zeros(mi.Color3f, width) for name in SAMPLED}

        def add(weight, radiance, direction, active):
            diffuse, specular = self.brdf(shading, view, direction, albedo, metallic, roughness)
            cosine = dr.maximum(dr.dot(shading, direction), 0)
            terms['D_indirect'] += dr.select(active, radiance * weight * cosine / dr.pi, 0)
            terms['L_diffuse_indirect'] += dr.select(active, radiance * weight * diffuse * cosine, 0)
            terms['L_specular_indirect'] += dr.select(active, radiance * weight * specular * cosine, 0)

        # Fixed half cosine, half GGX at the receiver: both integrands share one path.
        probability = mi.Float(.5)
        if not self.black:
            direction, pdf, radiance, ok = self.environment_sample(point, geometric, shading, sampler, valid)
            add(1 / (pdf + self.mixture_pdf(shading, view, direction, roughness, probability)), radiance, direction, ok)
        direction = self.mixture_sample(shading, view, roughness, probability, sampler)
        pdf = self.mixture_pdf(shading, view, direction, roughness, probability)
        ok = valid & (dr.dot(shading, direction) > 0) & (dr.dot(geometric, direction) > 0) & (pdf > 0)
        hit = closest(mi, dr, self.scene, offset(mi, dr, point, geometric, direction), ok)
        escaped = ok & ~hit.is_valid()
        if not self.black:
            environment = self.environment_pdf(point, geometric, direction)
            add(1 / (pdf + environment), self.radiance.eval(direction), direction, escaped)
        surface = self.continue_path(hit, direction, ok & hit.is_valid(), sampler)
        add(1 / dr.maximum(pdf, 1e-30), surface, direction, ok & hit.is_valid())
        dr.eval(terms)
        return terms

    def primary(self, rows, columns=None, subpixels=1, y_origin=0):
        """Receiver-equivalent primary vertices for a band of render rows."""
        mi, dr = self.mi, self.dr
        y = y_origin + (np.arange(*rows) + .5) / subpixels
        eye, directions, origins = camera_rays(self.setup, columns, y)
        valid, points, ng, ns, _, hit = primary_hits(mi, dr, self.scene, eye, directions, origins=origins,
                                                     return_interaction=True)
        active = mi.Bool(valid)
        albedo, metallic, roughness, emission = self.materials.evaluate(hit, active)
        vector = lambda kind, values: kind(*[mi.Float(values[:, i].astype(np.float32)) for i in range(3)])
        point, geometric, shading = vector(mi.Point3f, points), vector(mi.Normal3f, ng), vector(mi.Normal3f, ns)
        view = vector(mi.Vector3f, -directions)
        state = (point, geometric, shading, view, albedo, metallic, roughness, active)
        dr.eval(state, emission)
        return state, emission, valid


def render(mi, setup, passes, seed, max_depth, bands, footprint, output, contract=False, lambert=False,
           crop=None, subpixels=1):
    import drjit as dr
    started = time.perf_counter()
    width, height = setup['run']['renderWidth'], setup['run']['renderHeight']
    x0, y0, x1, y1 = crop or (0, 0, width, height)
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height and subpixels >= 1):
        raise ValueError('Invalid reference crop or subpixel grid')
    width, height = (x1 - x0) * subpixels, (y1 - y0) * subpixels
    columns = x0 + (np.arange(width) + .5) / subpixels
    if passes < 2 or passes % 2 or height % bands or (height // bands) % footprint or width % footprint:
        raise ValueError('Passes must be even; bands and the footprint must divide the render extent')
    reference = PathReference(mi, setup, max_depth, contract, lambert)
    results = {name: np.zeros((height, width, 3), np.float32) for name in TERMS}
    halves = {name: np.zeros((2, height, width, 3), np.float32) for name in SAMPLED}
    valid_mask = np.zeros((height, width), bool)
    timing = dict(setup=time.perf_counter() - started)
    step = height // bands
    for band in range(bands):
        rows = (band * step, (band + 1) * step)
        primary, emission, valid = reference.primary(rows, columns, subpixels, y0)
        point, geometric, shading, view, albedo, metallic, roughness, active = primary
        irradiance, outgoing = reference.all_lights(point, geometric, shading, view, albedo, metallic, roughness, active)
        lanes = dr.width(point)
        sampler = mi.load_dict({'type': 'independent'})
        sums = [{name: dr.zeros(mi.Color3f, lanes) for name in SAMPLED} for _ in range(2)]
        for index in range(passes):
            sampler.seed(dr.opaque(mi.UInt32, (seed * 1000003 + band * 7919 + index) & 0xffffffff), lanes)
            terms = reference.sample_indirect(primary, sampler)
            for name in SAMPLED:
                sums[index % 2][name] += terms[name]
            dr.eval(sums[index % 2])
        shape = (step, width, 3)
        for name in SAMPLED:
            split = np.stack([np.array(sums[h][name]).T.reshape(shape) / (passes // 2) for h in range(2)])
            halves[name][:, rows[0]:rows[1]] = split
            results[name][rows[0]:rows[1]] = split.mean(0)
        results['D_direct'][rows[0]:rows[1]] = np.array(irradiance).T.reshape(shape)
        results['L_direct'][rows[0]:rows[1]] = np.array(outgoing).T.reshape(shape)
        results['L_emission'][rows[0]:rows[1]] = np.where(valid[:, None], np.array(emission).T, 0).reshape(shape)
        valid_mask[rows[0]:rows[1]] = valid.reshape(step, width)
        timing[f'band_{band}'] = time.perf_counter() - started
        print(f'band {band + 1}/{bands} done, {time.perf_counter() - started:.0f} s', flush=True)
    results['L_total'] = results['L_emission'] + results['L_direct'] + results['L_diffuse_indirect'] + results['L_specular_indirect']
    for name in TERMS:
        results[name][~valid_mask] = 0

    def box(values):
        h, w = values.shape[-3] // footprint, values.shape[-2] // footprint
        return values.reshape(*values.shape[:-3], h, footprint, w, footprint, values.shape[-1]).mean((-4, -2))

    np.savez_compressed(output, valid=valid_mask, footprint=footprint,
                        **{f'{name}_box': box(results[name]) for name in TERMS},
                        **{f'{name}_box_halves': box(halves[name]) for name in SAMPLED},
                        **{f'{name}_pixel': results[name].astype(np.float16) for name in TERMS})
    timing['total'] = time.perf_counter() - started
    return timing


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Output .npz')
    parser.add_argument('--passes', type=int, default=64, help='Paths per render pixel (even)')
    parser.add_argument('--max-depth', type=int, default=10)
    parser.add_argument('--bands', type=int, default=4, help='Row bands rendered one at a time to bound GPU memory')
    parser.add_argument('--footprint', type=int, default=4)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--crop', type=int, nargs=4, metavar=('X0', 'Y0', 'X1', 'Y1'),
                        help='Render an exclusive rectangle in the original camera view')
    parser.add_argument('--subpixels', type=int, choices=(1, 2, 4, 8), default=1,
                        help='Regular primary sample grid per axis; equal --footprint yields native-pixel box averages')
    parser.add_argument('--contract-one-bounce', action='store_true',
                        help='Validation only: the one-bounce Lambert contract of ground_truth_reference.py')
    parser.add_argument('--lambert-secondaries', action='store_true',
                        help="Diagnostic: the world cache's Lambert 0.96 * (1 - metallic) secondary material, all bounces")
    args = parser.parse_args()
    mi = setup_mitsuba('cuda')
    setup = json.loads(args.setup.read_text(encoding='utf-8'))
    timing = render(mi, setup, args.passes, args.seed, args.max_depth, args.bands, args.footprint, args.output,
                    args.contract_one_bounce, args.lambert_secondaries, args.crop, args.subpixels)
    meta = dict(setup=str(args.setup.resolve()), passes=args.passes, max_depth=args.max_depth, footprint=args.footprint,
                contract_one_bounce=args.contract_one_bounce, lambert_secondaries=args.lambert_secondaries,
                seed=args.seed, crop=args.crop, subpixels=args.subpixels, mitsuba=mi.__version__, variant=mi.variant(), timing=timing)
    args.output.with_suffix('.json').write_text(json.dumps(meta, indent=1), encoding='utf-8', newline='\n')
    print(json.dumps(meta, indent=1))


if __name__ == '__main__':
    main()
