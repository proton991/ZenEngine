"""Independent full-image, uniform-albedo one-bounce validation for P5.

Uses Mitsuba primary/secondary triangle intersections and finite point-light shadow
rays, never engine voxels or radiance. Environment illumination at the secondary
hit uses an independent cosine sample of the captured level-zero environment.
The primary receiver is unmaterialed; the secondary Lambertian albedo is explicit.
This validates the cache approximation, not equality to a general material path
tracer. Two independent halves estimate reference noise. No renderer gate changes.
"""
import argparse
import hashlib
import json
from pathlib import Path
from urllib.parse import unquote

import numpy as np

from compare_hybrid_gi import load
from environment_reference import Environment
from ground_truth_mitsuba import (CubeRadiance, build_scene, camera_rays, continue_ray, depth_quantization_error,
                                 load_gltf_primitives, primary_hits, setup_mitsuba, transparent)


def closest(mi, dr, scene, ray, active):
    """Both faces block; alpha rejection continues with the remaining finite range."""
    si = scene.ray_intersect(ray, active)
    pending = active & si.is_valid() & transparent(mi, si, si.bsdf(ray), active & si.is_valid())

    def step(current, hit, pending, iteration):
        following = continue_ray(mi, hit, current.d)
        following.maxt = dr.maximum(current.maxt - hit.t - 1e-6, 0)
        current = dr.select(pending, following, current)
        next_hit = scene.ray_intersect(current, pending)
        hit = dr.select(pending, next_hit, hit)
        pending = pending & hit.is_valid() & transparent(mi, hit, hit.bsdf(current), pending & hit.is_valid())
        return current, hit, pending, iteration + 1

    _, si, pending, _ = dr.while_loop(
        state=(ray, si, pending, mi.UInt32(0)),
        cond=lambda ray, hit, pending, iteration: pending & (iteration < 64),
        body=step, mode='symbolic', label='bounce alpha continuation')
    if dr.any(pending):
        raise RuntimeError('Bounce alpha traversal exhausted 64 layers')
    return si


def offset(mi, dr, point, normal, direction, distance=None):
    epsilon = 4e-6 * (1 + dr.maximum(dr.maximum(dr.abs(point.x), dr.abs(point.y)), dr.abs(point.z)))
    ray = mi.Ray3f(point + normal * epsilon, direction)
    if distance is not None:
        ray.maxt = dr.maximum(distance - 2 * epsilon, 0)
    return ray


def render_bounce(mi, dr, scene, points, normals, albedo, lights, environment,
                  samples=4096, batch=4, seed=1):
    if samples < 2 * batch or samples % (2 * batch):
        raise ValueError('Samples must be a positive multiple of twice the batch size')
    count, lanes = len(points), len(points) * batch
    repeat = lambda a: mi.Float(np.repeat(a, batch).astype(np.float32))
    point = mi.Point3f(*[repeat(points[:, i]) for i in range(3)])
    normal = mi.Normal3f(*[repeat(normals[:, i]) for i in range(3)])
    frame = mi.Frame3f(normal)
    sampler = mi.load_dict({'type': 'independent'})
    halves = [dr.zeros(mi.Color3f, lanes), dr.zeros(mi.Color3f, lanes)]
    passes = samples // batch
    for index in range(passes):
        sampler.seed(seed * 1000003 + index, lanes)
        direction = frame.to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
        hit = closest(mi, dr, scene, offset(mi, dr, point, normal, direction), mi.Bool(True))
        valid = hit.is_valid()
        hit_normal = dr.select(dr.dot(hit.n, direction) < 0, hit.n, -hit.n)
        # Invalid lanes use finite neutral data before evaluating divisions/frames.
        hit_point = dr.select(valid, hit.p, mi.Point3f(0))
        hit_normal = dr.select(valid, hit_normal, mi.Normal3f(0, 1, 0))
        value = dr.zeros(mi.Color3f, lanes)
        for position, intensity in lights:
            delta = mi.Point3f(*position) - hit_point
            distance = dr.norm(delta)
            to_light = delta / dr.maximum(distance, 1e-20)
            cosine = dr.maximum(dr.dot(hit_normal, to_light), 0)
            active = valid & (cosine > 0) & (distance > 1e-5)
            shadow = closest(mi, dr, scene, offset(mi, dr, hit_point, hit_normal, to_light, distance), active)
            value += dr.select(active & ~shadow.is_valid(),
                               mi.Color3f(*intensity) * cosine / (dr.pi * dr.maximum(distance * distance, 1e-20)), 0)
        if environment is not None:
            incoming = mi.Frame3f(hit_normal).to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
            shadow = closest(mi, dr, scene, offset(mi, dr, hit_point, hit_normal, incoming), valid)
            value += dr.select(valid & ~shadow.is_valid(), environment.eval(incoming), 0)
        halves[index % 2] += dr.select(valid, value * albedo, 0)
        dr.eval(halves[index % 2])
    split = np.stack([np.array(h).T.reshape(count, batch, 3).mean(1) / (passes // 2) for h in halves])
    return split.mean(0), split


def summarize(actual, expected, halves, mask):
    a, b = actual[mask], expected[mask]
    mean = max(float(b.mean()), 1e-20)
    error = a - b
    noise = (halves[0][mask] - halves[1][mask]) * .5
    rms = float(np.sqrt(np.mean(error * error)) / mean)
    noise_rms = float(np.sqrt(np.mean(noise * noise)) / mean)
    return dict(pixels=int(mask.sum()), truth_mean=float(b.mean()),
                bias=float(error.mean() / mean), rms=rms, truth_noise_rms=noise_rms,
                excess_rms=float(np.sqrt(max(rms * rms - noise_rms * noise_rms, 0))))


def fingerprint(path):
    return dict(path=str(path), bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def validate_capture(mi, capture, output, samples=4096, batch=4, seed=1, albedo=.5, scene_path=None, fixture_gate=None):
    if Path(str(output) + '.npz').exists() or Path(str(output) + '.json').exists():
        raise FileExistsError('Reference artifacts are immutable; choose a new output prefix')
    metadata, data = load(capture)
    scene_path = scene_path or Path(str(capture) + '.gltf')
    primitives, alpha_image, normalization = load_gltf_primitives(scene_path)
    # This comparison only applies to the uniform fixture material contract.
    document = json.loads(scene_path.read_text())
    dependencies = [scene_path, Path(str(capture) + '.environment.bin'), Path(str(capture) + '.lighting.json')]
    for entry in document.get('buffers', []) + document.get('images', []):
        uri = entry.get('uri', '')
        if uri and not uri.startswith('data:'):
            dependencies.append(Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:') else scene_path.parent / unquote(uri))
    for material in document.get('materials', []):
        pbr = material.get('pbrMetallicRoughness', {})
        assert 'baseColorTexture' not in pbr and 'normalTexture' not in material
        assert np.allclose(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3], albedo)
        assert pbr.get('metallicFactor', 1) == 0 and not np.any(material.get('emissiveFactor', [0, 0, 0]))
    state = metadata['environment_intensity_rotation_enabled_visible']
    size = metadata['environment_cube_size']
    cube = np.fromfile(str(capture) + '.environment.bin', '<f4').reshape(6, size, size, 4)
    env = Environment(cube=cube, rotation=state[1], orientation=metadata.get('environment_orientation', [0, 0, 0, 1]),
                      intensity=state[0] * state[2])
    scene, dr, black = build_scene(mi, primitives, alpha_image, env, resolution=32)
    eye, directions = camera_rays(metadata, data)
    height, width = directions.shape[:2]
    directions = directions.reshape(-1, 3)
    pv = np.array(metadata['projection_view_column_major'], np.float32).astype(float).reshape(4, 4).T
    near_t = -(pv[2, :3] @ eye + pv[2, 3]) / (directions @ pv[2, :3])
    assert np.all(near_t > 0), 'The bounce oracle requires a forward Vulkan near plane'
    origins = eye + directions * near_t[:, None]
    valid, points, ng, _, _ = primary_hits(mi, dr, scene, eye, directions, origins=origins)
    lights = []
    for light in metadata['lights']:
        assert light['direction_type'][3] == 1, 'The independent P5 fixture oracle supports point lights only'
        assert light['position_range'][3] == 0, 'Finite point-light attenuation is outside this fixture oracle'
        value = light['color_intensity']
        lights.append((light['position_range'][:3], (np.array(value[:3]) * value[3]).tolist()))
    expected, halves = np.zeros((height * width, 3)), np.zeros((2, height * width, 3))
    expected[valid], halves[:, valid] = render_bounce(
        mi, dr, scene, points[valid], ng[valid], albedo, lights,
        None if black else CubeRadiance(mi, dr, env), samples, batch, seed)
    expected = expected.reshape(height, width, 3)
    halves = halves.reshape(2, height, width, 3)
    points = points.reshape(height, width, 3)
    ng = ng.reshape(height, width, 3)
    covered = data[:, :, 8, 3] > 0
    # Use the frozen sky-reference surface-agreement tolerance, including face normals.
    matched = valid.reshape(height, width) & covered
    engine = data[:, :, 8, :3]
    tolerance = 4 * depth_quantization_error(metadata, engine) + 1e-6 * (1 + np.abs(engine).max(-1))
    matched &= np.linalg.norm(points - engine, axis=-1) <= tolerance
    cosine = np.abs(np.sum(ng * data[:, :, 10, :3], axis=-1))
    cosine /= np.maximum(np.linalg.norm(ng, axis=-1) * np.linalg.norm(data[:, :, 10, :3], axis=-1), 1e-20)
    matched &= cosine > np.cos(np.deg2rad(.5))
    regions = dict(all_receivers=matched, floor=matched & (ng[:, :, 1] > .99))
    report = dict(capture=str(capture), samples=samples, seed=seed, albedo=albedo,
                  analytic_fixture_gate=fixture_gate, mitsuba=mi.__version__, variant=mi.variant(),
                  source_inputs=[fingerprint(path.resolve()) for path in dependencies],
                  reference_code=[fingerprint(Path(__file__).resolve()),
                                  fingerprint(Path(__file__).with_name('ground_truth_mitsuba.py').resolve())],
                  primary_rays='pixel centers, starting on the Vulkan near plane',
                  capture_metadata=metadata,
                  primary_mismatch=int(np.sum(covered & ~matched)),
                  scene_sha256=hashlib.sha256(scene_path.read_bytes()).hexdigest(),
                  normalization=normalization,
                  regions={name: summarize(data[:, :, 3, :3], expected, halves, mask)
                           for name, mask in regions.items() if mask.any()},
                  interpretation='Validation of isotropic voxel radiance against triangle transport; RMS is report-only')
    report['bias_gate_passed'] = all(abs(r['bias']) <= .10 for r in report['regions'].values())
    output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(str(output) + '.npz', bounce=expected.astype(np.float32), halves=halves.astype(np.float32),
                        position=points.astype(np.float32), geometric=ng.astype(np.float32), matched=matched)
    Path(str(output) + '.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def self_test(mi):
    primitive = dict(positions=np.array([[-10, 1, -10], [10, 1, -10], [10, 1, 10], [-10, 1, 10]]),
                     faces=np.array([[0, 1, 2], [0, 2, 3]], np.uint32), normals=None, double_sided=True,
                     masked=False, cutoff=.5, alpha=1, vertex_alpha=np.ones(4), texture=None, uv=np.zeros((4, 2)))
    scene, dr, _ = build_scene(mi, [primitive], None, Environment(intensity=0), 8)
    p, n = np.array([[0., 0, 0]]), np.array([[0., 1, 0]])
    analytic = .5 * 2 / (5 * np.pi)
    value, _ = render_bounce(mi, dr, scene, p, n, .5, [([0, 0, 0], [1, 1, 1])], None, 65536, 256)
    assert np.max(np.abs(value / analytic - 1)) < .005, (value, analytic)
    env = Environment(cube=np.ones((6, 4, 4, 4), np.float32), intensity=1)
    value, _ = render_bounce(mi, dr, scene, p, n, .5, [], CubeRadiance(mi, dr, env), 65536, 256)
    # Cosine-weighted solid angle of a square of half-width 10 at distance 1.
    side = 10 / np.sqrt(101)
    expected = .5 * 4 / np.pi * side * np.arctan(side)
    assert np.max(np.abs(value / expected - 1)) <= .005, (value, expected)
    value, _ = render_bounce(mi, dr, scene, p, -n, .5, [([0, 0, 0], [1, 1, 1])], None, 65536, 256)
    assert np.max(value) == 0, value
    masked = dict(primitive, masked=True, vertex_alpha=np.zeros(4))
    behind = dict(primitive, positions=primitive['positions'] + np.array([0, 1, 0]))
    layered, _, _ = build_scene(mi, [masked, behind], None, Environment(intensity=0), 8)
    ray = mi.Ray3f(mi.Point3f(0), mi.Vector3f(0, 1, 0))
    ray.maxt = mi.Float([1.5, 3])
    hit = closest(mi, dr, layered, ray, mi.Bool(True))
    np.testing.assert_array_equal(np.array(hit.is_valid()), [False, True])
    np.testing.assert_allclose(np.array(hit.p.y)[1], 2, atol=1e-6)
    return dict(point_light_plane='pass', uniform_environment_bounce='pass', empty_bounce='pass',
                alpha_continuation_and_finite_shadow_range='pass')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--scene', type=Path, help='Source fixture glTF when it is not beside the capture')
    parser.add_argument('--samples', type=int, default=4096)
    parser.add_argument('--batch', type=int, default=4)
    parser.add_argument('--backend', choices=('auto', 'cuda', 'llvm'), default='auto')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    renderer = setup_mitsuba(args.backend)
    gates = self_test(renderer)
    print(json.dumps(dict(analytic_fixtures=gates)), flush=True)
    if not args.self_test:
        if args.capture is None or args.output is None:
            parser.error('--capture and --output are required unless --self-test is used')
        result = validate_capture(renderer, args.capture, args.output, args.samples, args.batch,
                                  scene_path=args.scene, fixture_gate=gates)
        print(json.dumps(result, indent=2), flush=True)
        raise SystemExit(0 if result['bias_gate_passed'] else 1)
