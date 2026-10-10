"""Independent GGX lobe reference on CUDA for P6 reflections: S, hit fraction and reflected emission.

Mitsuba traces each receiver's specular lobe against the triangles (alpha masks continue, both faces
block) and reads no engine voxels or radiance. Receivers are taken from a capture's G-buffer
(position, shading and geometric normals, roughness), the same inputs as the engine's own estimate,
on every `stride`-th pixel in both directions.

Following the lighting contract, S is the escaped part of the lobe f*cos over the shading normal's
upper hemisphere: visible-normal samples carry the weight G1, directions below the shading normal
are outside the integral, and directions below the geometric normal are blocked. C = (1 - S) H is
the weighted mean, over the same samples, of the emission at front-facing hits, in the cache's
radiance units (emissive factor times strength) for scenes whose only light is emission. This tool
requires the CUDA backend and checks itself against analytic cases before any comparison.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import load
from environment_reference import Environment
from ground_truth_bounce import closest, offset
from ground_truth_mitsuba import PRIMITIVE_ATTRIBUTE, build_scene, load_gltf_primitives, setup_mitsuba


def ggx_lobe(mi, dr, frame, view, alpha, u):
    """Visible-normal GGX sample (Heitz 2018) and its weight G1 with respect to the shading normal."""
    v = frame.to_local(view)
    vh = dr.normalize(mi.Vector3f(alpha * v.x, alpha * v.y, v.z))
    length2 = vh.x * vh.x + vh.y * vh.y
    t1 = dr.select(length2 > 0, mi.Vector3f(-vh.y, vh.x, 0) * dr.rsqrt(dr.maximum(length2, 1e-30)), mi.Vector3f(1, 0, 0))
    t2 = dr.cross(vh, t1)
    r, phi = dr.sqrt(u.x), 2 * dr.pi * u.y
    x, y = r * dr.cos(phi), r * dr.sin(phi)
    s = 0.5 * (1 + vh.z)
    y = (1 - s) * dr.sqrt(dr.maximum(1 - x * x, 0)) + s * y
    nh = x * t1 + y * t2 + dr.sqrt(dr.maximum(1 - x * x - y * y, 0)) * vh
    h = dr.normalize(mi.Vector3f(alpha * nh.x, alpha * nh.y, dr.maximum(nh.z, 0)))
    local = 2 * dr.dot(v, h) * h - v
    c = local.z
    a2 = alpha * alpha
    weight = dr.select(c > 0, 2 * c / (c + dr.sqrt(a2 + (1 - a2) * c * c)), 0)
    return frame.to_world(local), weight


def trace_lobes(mi, dr, scene, emission, points, shading, geometric, views, roughness,
                samples=65536, batch=64, seed=1):
    """Per receiver: weighted escaped, hit and below-geometric fractions, and reflected emission C."""
    if samples % batch:
        raise ValueError('Samples must be a multiple of the batch size')
    count, lanes = len(points), len(points) * batch
    repeat = lambda a: mi.Float(np.repeat(a, batch).astype(np.float32))
    point = mi.Point3f(*[repeat(points[:, i]) for i in range(3)])
    normal = mi.Normal3f(*[repeat(shading[:, i]) for i in range(3)])
    ng = mi.Normal3f(*[repeat(geometric[:, i]) for i in range(3)])
    view = mi.Vector3f(*[repeat(views[:, i]) for i in range(3)])
    alpha = repeat(np.maximum(roughness ** 2, .0016))
    frame = mi.Frame3f(normal)
    table = [mi.Float(np.asarray(emission, np.float32)[:, c]) for c in range(3)]
    sampler = mi.load_dict({'type': 'independent'})
    totals = [dr.zeros(mi.Float, lanes) for _ in range(4)]
    reflected = dr.zeros(mi.Color3f, lanes)
    for index in range(samples // batch):
        sampler.seed(seed * 1000003 + index, lanes)
        direction, weight = ggx_lobe(mi, dr, frame, view, alpha, sampler.next_2d())
        above = (weight > 0) & (dr.dot(direction, ng) > 0)
        hit = closest(mi, dr, scene, offset(mi, dr, point, ng, direction), above)
        hits = above & hit.is_valid()
        # The index is a per-vertex attribute: interpolation returns e.g. 0.99999994, so round it.
        primitive = mi.UInt32(dr.round(hit.shape.eval_attribute_1(PRIMITIVE_ATTRIBUTE, hit, hits)))
        facing = hits & (dr.dot(hit.n, direction) < 0)
        value = mi.Color3f(*[dr.gather(mi.Float, t, primitive, facing) for t in table])
        totals[0] += weight
        totals[1] += dr.select(above & ~hit.is_valid(), weight, 0)
        totals[2] += dr.select(hits, weight, 0)
        totals[3] += dr.select((weight > 0) & ~above, weight, 0)
        reflected += dr.select(facing, value * weight, 0)
        dr.eval(totals, reflected)
    per = lambda a: np.array(a).reshape(count, batch).sum(1)
    weight = np.maximum(per(totals[0]), 1e-30)
    colour = np.array(reflected).T.reshape(count, batch, 3).sum(1) / weight[:, None]
    return dict(S=per(totals[1]) / weight, hit=per(totals[2]) / weight, below=per(totals[3]) / weight, C=colour)


def material_emission(scene_path, primitives):
    document = json.loads(Path(scene_path).read_text())
    materials = document.get('materials', [])
    rows = []
    for p in primitives:
        material = materials[p['material']] if p['material'] >= 0 else {}
        strength = material.get('extensions', {}).get('KHR_materials_emissive_strength', {}).get('emissiveStrength', 1)
        rows.append(np.asarray(material.get('emissiveFactor', [0, 0, 0]), float) * strength)
    return np.asarray(rows)


def quad(height, facing_down, half=1000.0):
    corners = np.array([[-half, height, -half], [half, height, -half], [half, height, half], [-half, height, half]])
    # Counter-clockwise winding gives the face normal: (0,1,2),(0,2,3) faces -y here.
    faces = np.array([[0, 1, 2], [0, 2, 3]] if facing_down else [[0, 2, 1], [0, 3, 2]], np.uint32)
    return dict(positions=corners, faces=faces, normals=None, double_sided=True, masked=False, cutoff=.5, alpha=1,
                vertex_alpha=np.ones(4), texture=None, uv=np.zeros((4, 2)), material=0)


def self_test(mi):
    """Analytic cases: open sky, the G1-weighted lobe's closed-form cap, an emissive ceiling and its back."""
    import drjit as dr
    point, up = np.zeros((1, 3)), np.array([[0., 1, 0]])
    emission = np.array([[2, .2, .05]])
    empty, _, _ = build_scene(mi, [], None, Environment(intensity=0), 8)
    result = trace_lobes(mi, dr, empty, np.zeros((1, 3)), point, up, up, up, np.array([.5]), 16384, 256)
    assert result['S'][0] == 1 and result['hit'][0] == 0, result
    # Roughness 1 at normal incidence: the weighted lobe's cosine density is 2z/(1+z), so its mass
    # above z=0.5 is (0.5 - ln(4/3)) / (1 - ln 2) (the native HybridSpecularDirection regression).
    frame = mi.Frame3f(mi.Normal3f(0, 1, 0))
    sampler = mi.load_dict({'type': 'independent'})
    sampler.seed(7, 1 << 22)
    direction, weight = ggx_lobe(mi, dr, frame, mi.Vector3f(0, 1, 0), mi.Float(1.0), sampler.next_2d())
    weight = np.array(weight)
    fraction = float(weight[np.array(direction.y) > .5].sum() / weight.sum())
    expected = (.5 - np.log(4 / 3)) / (1 - np.log(2))
    assert abs(fraction - expected) < 1e-3, (fraction, expected)
    ceiling, _, _ = build_scene(mi, [quad(1, True)], None, Environment(intensity=0), 8)
    result = trace_lobes(mi, dr, ceiling, emission, point, up, up, up, np.array([.5]), 16384, 256)
    assert result['S'][0] < 1e-3 and np.allclose(result['C'][0] / emission[0], 1, atol=1e-3), result
    # A second primitive checks per-hit primitive identification (an interpolated index must round).
    two, _, _ = build_scene(mi, [quad(5, False), quad(1, True)], None, Environment(intensity=0), 8)
    result = trace_lobes(mi, dr, two, np.array([[0, 0, 0], [2, .2, .05]]), point, up, up, up, np.array([.5]), 16384, 256)
    assert np.allclose(result['C'][0] / emission[0], 1, atol=1e-3), result
    back, _, _ = build_scene(mi, [quad(1, False)], None, Environment(intensity=0), 8)
    result = trace_lobes(mi, dr, back, emission, point, up, up, up, np.array([.5]), 16384, 256)
    assert result['S'][0] < 1e-3 and np.all(result['C'][0] == 0), result
    return dict(open_sky='pass', lobe_cap_closed_form='pass', emissive_ceiling='pass', second_primitive='pass',
                back_face_rejected='pass')


def receivers(capture, stride, floor_only=False):
    metadata, data = load(capture)
    covered = data[:, :, 8, 3] > 0
    if floor_only:
        covered &= data[:, :, 10, 1] > .99
    grid = np.zeros_like(covered)
    grid[stride // 2::stride, stride // 2::stride] = True
    pixels = np.argwhere(covered & grid)
    r = data[pixels[:, 0], pixels[:, 1]]
    camera = np.asarray(metadata['camera_position'], float)
    views = r[:, 8, :3] - camera
    views = -views / np.maximum(np.linalg.norm(views, axis=1, keepdims=True), 1e-30)
    return pixels, r, views


def compare_capture(mi, capture, scene_path, output, stride=2, samples=65536, emissive=False, fixture_gate=None):
    """Compare a reference capture's S (and, with emission, C) with the independent lobe integral."""
    import drjit as dr
    if Path(str(output) + '.json').exists():
        raise FileExistsError('Reference artifacts are immutable; choose a new output prefix')
    primitives, alpha_image, _ = load_gltf_primitives(scene_path)
    scene, _, _ = build_scene(mi, primitives, alpha_image, Environment(intensity=0), 8)
    emission = material_emission(scene_path, primitives)
    pixels, r, views = receivers(capture, stride, floor_only=emissive)
    truth = trace_lobes(mi, dr, scene, emission, r[:, 8, :3], r[:, 9, :3], r[:, 10, :3], views, r[:, 9, 3], samples)
    error = r[:, 5, 3] - truth['S']
    report = dict(capture=str(capture), receivers=len(pixels), stride=stride, samples_per_receiver=samples,
                  variant=mi.variant(), analytic_fixtures=fixture_gate,
                  S=dict(maximum_absolute=float(np.abs(error).max()), mean_absolute=float(np.abs(error).mean()),
                         p99_absolute=float(np.quantile(np.abs(error), .99)), mean_signed=float(error.mean())),
                  truth_hit_fraction_mean=float(truth['hit'].mean()), truth_below_geometric_mean=float(truth['below'].mean()),
                  reference_code=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    report['S']['passed'] = report['S']['maximum_absolute'] <= .02
    if emissive:
        actual, expected = r[:, 5, :3], truth['C']
        mean = max(float(expected.mean()), 1e-20)
        delta = actual - expected
        report['C'] = dict(truth_mean=mean, bias=float(delta.mean() / mean), rms=float(np.sqrt((delta ** 2).mean()) / mean))
        report['C']['passed'] = abs(report['C']['bias']) <= .005
    Path(output).parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(str(output) + '.npz', pixels=pixels, S=truth['S'], hit=truth['hit'], below=truth['below'], C=truth['C'])
    Path(str(output) + '.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--scene', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--stride', type=int, default=2)
    parser.add_argument('--samples', type=int, default=65536)
    parser.add_argument('--emissive', action='store_true')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    renderer = setup_mitsuba('cuda')
    gates = self_test(renderer)
    print(json.dumps(dict(analytic_fixtures=gates)), flush=True)
    if not args.self_test:
        report = compare_capture(renderer, args.capture, args.scene, args.output, args.stride, args.samples,
                                 args.emissive, gates)
        print(json.dumps(report, indent=2), flush=True)
        raise SystemExit(0 if report['S']['passed'] and report.get('C', dict(passed=True))['passed'] else 1)
