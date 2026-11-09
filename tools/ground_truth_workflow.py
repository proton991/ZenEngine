"""Build, verify, capture and compare the immutable New Sponza diffuse database.

Use build/venv/Scripts/python.exe. Run --help for commands. The template is a
native ZenApp launch setup for a prepared New Sponza package; no engine.cfg edits.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import copy
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TERMS = ('D_direct', 'D_sky', 'D_bounce')
TOOLS = ('ground_truth_workflow.py', 'ground_truth_reference.py', 'ground_truth_mitsuba.py',
         'ground_truth_bounce.py', 'environment_reference.py', 'ground_truth_fixtures.py')


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def digests(paths):
    """digest() of each path, concurrently: hashlib releases the GIL while hashing large reads."""
    paths = list(dict.fromkeys(paths))
    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as pool:
        return dict(zip(paths, pool.map(digest, paths)))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def safe_child(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f'Path escapes the dataset: {relative}')
    return path


def input_files(setup):
    scene = Path(setup['sourcePath']).resolve()
    doc = read_json(scene)
    files = {scene, Path(setup['packagePath']).resolve()}
    for entry in doc.get('buffers', []) + doc.get('images', []):
        uri = entry.get('uri', '')
        if uri and not uri.startswith('data:'):
            files.add((Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:') else scene.parent/unquote(uri)).resolve())
    return files


def verify(dataset):
    dataset = Path(dataset).resolve()
    manifest = read_json(dataset/'dataset.json')
    if manifest['version'] != 1 or len({frame['id'] for frame in manifest['frames']}) != len(manifest['frames']):
        raise ValueError('Unsupported or duplicate dataset frames')
    files = {relative: safe_child(dataset, relative) for relative in manifest['files']}
    actual = digests([*files.values(), *manifest['inputs']])
    for relative, expected in manifest['files'].items():
        if actual[files[relative]] != expected:
            raise ValueError(f'Dataset hash mismatch: {relative}')
    for path, expected in manifest['inputs'].items():
        if actual[path] != expected:
            raise ValueError(f'Input changed; build a new dataset version: {path}')
    for name, expected in manifest['tools'].items():
        if digest(ROOT/'tools'/name) != expected:
            raise ValueError(f'Reference algorithm changed; build a new dataset version: {name}')
    for frame in manifest['frames']:
        directory = safe_child(dataset, 'frames/'+frame['id'])
        for name in ('scene.zenrender', 'frame.json', 'truth.npz'):
            relative = (directory/name).relative_to(dataset).as_posix()
            if relative not in manifest['files']:
                raise ValueError(f'Unhashed required dataset file: {relative}')
        metadata = read_json(directory/'frame.json')
        if digest(directory/'scene.zenrender') != metadata['setup_sha256']:
            raise ValueError('Frame configuration identity mismatch')
        if not metadata['validation']['passed']:
            raise ValueError('Dataset contains an unvalidated camera')
    return manifest


def point_visibility(reference, eye):
    from ground_truth_bounce import closest
    mi, dr = reference.mi, reference.dr
    scale = reference.normalization['scale']
    rng = np.random.default_rng(941)
    sphere = rng.normal(size=(1024, 3))
    sphere /= np.linalg.norm(sphere, axis=1, keepdims=True)
    origin = mi.Point3f(*np.asarray(eye).tolist())
    hit = closest(mi, dr, reference.scene, mi.Ray3f(origin, mi.Vector3f(*sphere.T)), mi.Bool(True))
    found, distance = np.array(hit.is_valid()), np.array(hit.t)
    normals = np.array(hit.n).T
    back = found & (np.sum(normals*sphere, axis=1) > 0)
    clearance = float(np.min(distance))/scale
    return dict(backface_fraction=float(back.mean()), clearance_m=float(clearance) if np.isfinite(clearance) else None,
                finite=bool(not np.isnan(distance).any()), sphere_rays=1024)


def validate_camera(reference, setup, indoor, floor):
    from ground_truth_reference import camera_rays
    from ground_truth_bounce import closest
    mi, dr = reference.mi, reference.dr
    scale = reference.normalization['scale']
    eye, directions, origins = camera_rays(setup)
    point = point_visibility(reference, eye)
    origin = mi.Point3f(*eye.tolist())
    # Every pixel-center near-plane segment is checked against two-sided alpha geometry.
    view = closest(mi, dr, reference.scene, mi.Ray3f(origin, mi.Vector3f(*directions.T)), mi.Bool(True))
    t = np.array(view.t)
    near_clear = bool(np.all(t >= np.linalg.norm(origins-eye, axis=1)))
    valid, points, _, _, _ = reference.primary(setup)
    primary_distance = np.linalg.norm(points[valid]-eye, axis=1)/scale
    close_fraction = float(np.count_nonzero(primary_distance < .5)/len(valid))
    floor_hit = closest(mi, dr, reference.scene, mi.Ray3f(origin, mi.Vector3f(0, -1, 0)), mi.Bool(True))
    floor_distance = float(np.array(floor_hit.t)[0])/scale
    sky_fraction = float(np.mean(~valid))
    passed = (point['finite'] and point['backface_fraction'] <= .01
              and (point['clearance_m'] is None or point['clearance_m'] >= .3) and near_clear
              and close_fraction < .1 and (indoor or sky_fraction >= .02)
              and (not floor or 1.2 <= floor_distance <= 2.0))
    return dict(passed=bool(passed), **point, near_clear=near_clear,
                close_primary_fraction=close_fraction, sky_fraction=sky_fraction,
                floor_distance_m=floor_distance if np.isfinite(floor_distance) else None,
                indoor=indoor, floor_camera=floor)


def configurations(template, reference, width, height):
    base = copy.deepcopy(template)
    base['animationEnabled'] = False
    base['defaultEnvironment'] = str(ROOT/'Data/Textures/Environments/kloppenheim_06_puresky_1k.hdr')
    base['run'].update(windowWidth=width, windowHeight=height, renderWidth=width, renderHeight=height,
                       fullscreen=False, followWindow=False, vsync=False, gpuTimings=False)
    base['rendering'].update(normalMaps=False, lightMarkers=False, algorithm=1)
    base['rendering']['cameraLight']['enabled'] = False
    base['rendering']['gi']['cone'].update(rayProvider=2, featureProviders=[2]*6, historyFrames=128,
                                         previousFrameRadiance=False, emissiveLighting=False,
                                         indirectIntensity=1, analyticLighting=True, environmentLighting=True)
    base['camera'].update(aspect=width/height, verticalFovDegrees=60, orthographic=False)
    # Authored normalized candidates, each checked against the source geometry before use.
    cameras = [([-.05, -.199, -.07], [-.05, .2, -.02], False, True),
               ([-.15, -.199, -.2], [.25, -.19, -.2], True, True),
               ([-.17, -.055, -.2], [-.05, -.2, 0], True, True),
               ([-.15, -.199, .05], [.25, -.2, .05], True, True),
               ([-.32, -.199, -.07], [-.45, -.03, -.07], True, True)]
    selected = []
    for index, (eye, target, indoor, floor) in enumerate(cameras):
        reports = []
        for dx, dz in [(0, 0), (.02, 0), (-.02, 0), (0, .02), (0, -.02), (.04, .04), (-.04, -.04)]:
            setup = copy.deepcopy(base)
            setup['camera'].update(eye=(np.array(eye)+[dx, 0, dz]).tolist(), target=target)
            stats = validate_camera(reference, setup, indoor, floor)
            reports.append(stats)
            if stats['passed']:
                selected.append((setup, stats))
                break
        else:
            raise ValueError(f'C{index+1} has no valid candidate: {json.dumps(reports)}')

    def light(kind, intensity, position=(0, 0, 0), direction=(0, -1, 0)):
        return dict(type=kind, intensity=intensity, position=list(position), direction=list(direction),
                    color=[1, 1, 1], range=0, innerAngleDegrees=0, outerAngleDegrees=45,
                    enabled=True, castsShadows=True)
    lighting = [
        ('kloofendal_48d_partly_cloudy_puresky_1k', 1, 0, [light(0, 3, direction=(.25, -.95, .1))]),
        ('qwantani_noon_puresky_2k', 1, 65, [light(0, 2, direction=(.9, -.2, .3))]),
        ('kloppenheim_06_puresky_1k', 1, 0, []),
        ('small_empty_room_1_1k', .2, 0,
         [light(1, .02, position=(-.2, -.12, -.07)), light(1, .02, position=(.2, -.12, -.07)),
          light(1, .01, position=(0, -.04, -.14))])]
    frames = []
    for light_index, (environment, intensity, rotation, lights) in enumerate(lighting):
        light_validation = []
        for value in lights:
            if value['type'] == 1:
                stats = point_visibility(reference, value['position'])
                if not stats['finite'] or stats['backface_fraction'] > .01 or (stats['clearance_m'] is not None and stats['clearance_m'] < .1):
                    raise ValueError(f'Point light is enclosed or too close to geometry: {value["position"]}, {stats}')
                light_validation.append(stats)
        for camera_index, (camera, stats) in enumerate(selected):
            setup = copy.deepcopy(camera)
            setup['rendering']['environment'].update(texturePath=str(ROOT/'Data/Textures/Environments'/f'{environment}.hdr'),
                                                      intensity=intensity, rotationDegrees=rotation, lighting=True, skybox=True)
            setup['rendering']['lights'] = [dict(id=i+1, origin=1, light=value) for i, value in enumerate(lights)]
            frames.append((f'C{camera_index+1}_L{light_index+1}', setup, dict(stats, lights=light_validation)))
    return frames


def noise_rms(truth):
    valid = truth['valid']
    noise = {'D_direct': 0.0}
    for term in TERMS[1:]:
        difference = (truth[term+'_halves'][0]-truth[term+'_halves'][1])*.5
        noise[term] = float(np.sqrt(np.mean(difference[valid]**2)))
    return noise


def benchmark(args):
    from ground_truth_mitsuba import setup_mitsuba
    from ground_truth_reference import Reference
    from ground_truth_fixtures import fixtures
    started = time.perf_counter()
    mi = setup_mitsuba()
    result = dict(fixtures=fixtures(mi), frames={})
    template = read_json(args.template)
    reference = Reference(mi, template['sourcePath'])
    frames = configurations(template, reference, args.width, args.height)
    for index in (0, 16):
        name, setup, validation = frames[index]
        truth, timings = reference.render(setup, args.samples, args.batch, args.seed+index*7919)
        seconds = sum(value for key, value in timings.items() if key != 'material_load')
        result['frames'][name] = dict(timings=timings, noise_rms=noise_rms(truth), validation=validation,
                                      reference_seconds=seconds, within_budget=seconds <= 33)
        print(name, result['frames'][name], flush=True)
    result.update(seconds=time.perf_counter()-started, resolution=[args.width, args.height], samples=args.samples,
                  batch=args.batch, variant=mi.variant(), mitsuba=mi.__version__)
    write_json(args.output, result)


def build(args):
    from ground_truth_mitsuba import setup_mitsuba
    from ground_truth_reference import Reference
    target = args.dataset.resolve()
    if target.is_relative_to(ROOT/'build'):
        raise ValueError('Published ground-truth datasets must live outside build/')
    if target.exists():
        raise FileExistsError(f'Immutable version already exists: {target}')
    started = time.perf_counter()
    template = read_json(args.template)
    mi = setup_mitsuba()
    from ground_truth_fixtures import fixtures
    fixture_results = fixtures(mi)
    reference = Reference(mi, template['sourcePath'])
    frames = configurations(template, reference, args.width, args.height)
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=target.name+'.building-', dir=target.parent))
    inputs = input_files(template) | {Path(setup['rendering']['environment']['texturePath']) for _, setup, _ in frames}
    manifest = dict(version=1, frames=[], inputs={str(path): digest(path) for path in sorted(inputs)},
                    tools={name: digest(ROOT/'tools'/name) for name in TOOLS}, mitsuba=mi.__version__,
                    variant=mi.variant(), files={}, resolution=[args.width, args.height])
    manifest['fixtures'] = fixture_results
    for index, (name, setup, stats) in enumerate(frames):
        directory = staging/'frames'/name
        directory.mkdir(parents=True)
        write_json(directory/'scene.zenrender', setup)
        truth, timings = reference.render(setup, args.samples, args.batch, args.seed+index*7919)
        np.savez(directory/'truth.npz', **truth)
        noise = noise_rms(truth)
        write_json(directory/'frame.json', dict(setup_sha256=digest(directory/'scene.zenrender'),
                                                validation=stats, samples=args.samples, seed=args.seed+index*7919,
                                                timings=timings, noise_rms=noise))
        manifest['frames'].append(dict(id=name, noise_rms=noise))
        print(name, timings, noise, flush=True)
        if time.perf_counter()-started > args.budget_seconds:
            raise RuntimeError(f'Database build exceeded {args.budget_seconds}s; unpublished evidence is in {staging}')
    manifest['seconds'] = time.perf_counter()-started
    manifest['files'] = {path.relative_to(staging).as_posix(): digest(path) for path in staging.rglob('*') if path.is_file()}
    write_json(staging/'dataset.json', manifest)
    verify(staging)
    # rename refuses an existing destination on Windows; versions are never replaced.
    if target.exists():
        raise FileExistsError(target)
    staging.rename(target)
    for path in target.rglob('*'):
        if path.is_file():
            path.chmod(stat.S_IREAD)
    print(f'Published {target} in {manifest["seconds"]:.1f}s')


def run(args):
    manifest = verify(args.dataset)
    if args.output.exists():
        raise FileExistsError(args.output)
    exe = args.executable.resolve()
    if exe.name.lower() != 'zenapp.exe':
        raise ValueError('Captures require the standalone ZenApp executable')
    shaders = {str(path.relative_to(ROOT)): digest(path) for path in sorted((ROOT/'Data/SpvShaders').rglob('*.spv'))}
    executable_hash = digest(exe)
    command = [str(exe), '--ground-truth='+str(args.dataset.resolve()), '--output='+str(args.output.resolve())]
    started = time.perf_counter()
    result = subprocess.run(command, cwd=ROOT, check=False)
    provenance = dict(executable=str(exe), executable_sha256=executable_hash, arguments=command[1:],
                      shaders=shaders, dataset_sha256=digest(args.dataset/'dataset.json'),
                      seconds=time.perf_counter()-started, returncode=result.returncode, verified=False,
                      setups={frame['id']: digest(args.dataset/'frames'/frame['id']/'scene.zenrender') for frame in manifest['frames']})
    if not args.output.is_dir():
        raise RuntimeError(f'ZenApp failed before creating its output ({result.returncode})')
    write_json(args.output/'run.json', provenance)
    if result.returncode:
        raise RuntimeError(f'ZenApp failed ({result.returncode}); see {args.output}/ZenApp.log')
    if executable_hash != digest(exe) or any(digest(ROOT/path) != value for path, value in shaders.items()):
        raise ValueError('Executable or shader inputs changed during capture')
    # The dataset and its inputs, unchanged since before the capture; compare() reuses this check.
    manifest = verify(args.dataset)
    provenance['verified'] = True
    provenance['files'] = {path.name: value for path, value in digests(
        path for path in args.output.iterdir() if path.is_file() and path.name != 'run.json' and path.suffix != '.log').items()}
    write_json(args.output/'run.json', provenance)
    compare(args, manifest)


def metrics(engine, truth, half_difference, mask):
    if not mask.any():
        raise ValueError('No matched receiver pixels')
    difference = engine-truth
    signal = float(np.sqrt(np.mean(truth[mask]**2)))
    corrected = np.maximum(np.mean(difference[mask]**2)-np.mean(half_difference[mask]**2), 0)
    bias = float(np.mean(difference[mask])/max(float(np.mean(np.abs(truth[mask]))), 1e-8))
    h, w = mask.shape
    # Partial boundary blocks are supported; variance of a block is estimated from
    # the independent halves below, preserving spatial covariance.
    ys, xs = np.arange(0, h, 8), np.arange(0, w, 8)
    counts = np.add.reduceat(np.add.reduceat(mask.astype(float), ys, axis=0), xs, axis=1)
    def block_mean(values):
        sums = np.add.reduceat(np.add.reduceat(values*mask[..., None], ys, axis=0), xs, axis=1)
        return sums[counts > 0]/counts[counts > 0, None]
    blocks, block_noise = block_mean(difference), block_mean(half_difference)
    return dict(relative_bias=bias, noise_corrected_rms=float(np.sqrt(corrected)),
                relative_rms=float(np.sqrt(corrected)/max(signal, 1e-8)),
                p95_error=float(np.quantile(np.linalg.norm(difference[mask], axis=1), .95)),
                block_rms=float(np.sqrt(max(0, np.mean(np.square(blocks))-np.mean(np.square(block_noise))))),
                reference_noise_rms=float(np.sqrt(np.mean(half_difference[mask]**2))),
                noise_below_quarter_error=bool(np.mean(half_difference[mask]**2) <= corrected/16), pixels=int(mask.sum()))


def same_setup(expected, actual):
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(key in actual and same_setup(value, actual[key]) for key, value in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(expected) == len(actual) and all(same_setup(a, b) for a, b in zip(expected, actual))
    if isinstance(expected, (float, int)) and not isinstance(expected, bool):
        return isinstance(actual, (float, int)) and np.isclose(expected, actual, rtol=2e-6, atol=1e-7)
    return expected == actual


def environment_fixture(setup, summary, path):
    import mitsuba as mi
    from environment_reference import Environment, normalize
    size = summary['environmentCubeSize']
    cube = np.fromfile(path, '<f4').reshape(6, size, size, 4)[..., :3]
    pixels = np.array(mi.Bitmap(setup['rendering']['environment']['texturePath']).convert(
        mi.Bitmap.PixelFormat.RGB, mi.Struct.Type.Float32, False))
    environment = Environment(equirect=pixels)
    x, y = np.meshgrid((np.arange(size)+.5)*2/size-1, (np.arange(size)+.5)*2/size-1)
    one = np.ones_like(x)
    directions = normalize(np.stack([np.stack(value, -1) for value in
                                     ((one, -y, -x), (-one, -y, x), (x, one, y),
                                      (x, -one, -y), (x, -y, one), (-x, -y, -one))]))
    source = environment.sample(directions.reshape(-1, 3)).reshape(cube.shape)
    error = cube-source
    return dict(rms=float(np.sqrt(np.mean(error**2))),
                relative_rms=float(np.sqrt(np.mean(error**2)/max(np.mean(source**2), 1e-20))),
                relative_bias=float(error.mean()/max(source.mean(), 1e-20)),
                p95=float(np.quantile(np.linalg.norm(error, axis=-1), .95)), cube_size=size)


def compare(args, manifest=None):
    """manifest: the result of a verify() of args.dataset already made by this process."""
    started = time.perf_counter()
    manifest = manifest or verify(args.dataset)
    run = read_json(args.output/'run.json')
    if run['returncode'] or not run['verified'] or run['dataset_sha256'] != digest(args.dataset/'dataset.json'):
        raise ValueError('Run identity does not match the dataset')
    for path, expected in run['files'].items():
        if digest(safe_child(args.output, path)) != expected:
            raise ValueError(f'Engine capture changed: {path}')
    report = dict(frames={}, dataset_sha256=run['dataset_sha256'])
    for frame in manifest['frames']:
        name = frame['id']
        directory = args.dataset/'frames'/name
        setup = read_json(directory/'scene.zenrender')
        resolved = read_json(args.output/(name+'.zenrender'))
        summary = read_json(args.output/(name+'.json'))
        if not same_setup(setup, resolved):
            raise ValueError(f'Resolved ZenApp setup differs from the dataset: {name}')
        if run['setups'][name] != digest(directory/'scene.zenrender'):
            raise ValueError(f'Configuration hash mismatch: {name}')
        for key in ('eye', 'target', 'aspect', 'verticalFovDegrees', 'nearPlane', 'farPlane'):
            if not np.allclose(setup['camera'][key], resolved['camera'][key], rtol=1e-6, atol=1e-7):
                raise ValueError(f'Camera identity mismatch: {name}/{key}')
        w, h = setup['run']['renderWidth'], setup['run']['renderHeight']
        if [summary['width'], summary['height']] != [w, h]:
            raise ValueError(f'Capture extent mismatch: {name}')
        lighting = np.fromfile(args.output/(name+'.lighting.bin'), '<f4').reshape(h, w, 10, 4)
        hybrid = np.fromfile(args.output/(name+'.hybrid.bin'), '<f4').reshape(h, w, 13, 4)
        with np.load(directory/'truth.npz', allow_pickle=False) as archive:
            truth = dict(archive)
        eye = np.array(setup['camera']['eye'])
        distance = np.linalg.norm(truth['position']-eye, axis=2)
        footprint = 2*distance*np.tan(np.deg2rad(setup['camera']['verticalFovDegrees'])/2)/h
        mask = truth['valid'] & (hybrid[:, :, 8, 3] > 0) & (lighting[:, :, 7, 3] > 0)
        mask &= np.linalg.norm(hybrid[:, :, 8, :3]-truth['position'], axis=2) <= np.maximum(footprint, 2e-5)
        mask &= np.sum(hybrid[:, :, 10, :3]*truth['geometric'], axis=2) >= np.cos(np.deg2rad(25))
        if not mask.any() or not np.isfinite(lighting).all():
            raise ValueError(f'No matching surfaces or non-finite capture: {name}')
        per_frame = dict(match_rate=float(mask.sum()/max(1, truth['valid'].sum())), terms={},
                         environment_fixture=environment_fixture(setup, summary, args.output/(name+'.environment.bin')))
        median = np.median(distance[mask])
        truth['D'] = sum(truth[term] for term in TERMS)
        truth['D_halves'] = truth['D_sky_halves'] + truth['D_bounce_halves'] + truth['D_direct'][None]
        signals = [(term, lighting[:, :, 7+i, :3]) for i, term in enumerate(TERMS)]
        signals.append(('D', lighting[:, :, 7:10, :3].sum(axis=2)))
        for label, value in [('reference', truth['D']), ('engine', signals[-1][1])]:
            visual = np.maximum(truth['albedo']*value, 0)
            visual = np.power(visual/(1+visual), 1/2.2)
            visual[~mask] = 0
            Image.fromarray(np.uint8(np.clip(visual, 0, 1)*255)).save(args.output/(name+'-'+label+'-diffuse.png'))
        for term, engine in signals:
            half_difference = (truth[term+'_halves'][0]-truth[term+'_halves'][1])*.5 if term != 'D_direct' else np.zeros_like(engine)
            result = metrics(engine, truth[term], half_difference, mask)
            for label, selected in [('near', mask & (distance <= median)), ('far', mask & (distance > median))]:
                result[label] = metrics(engine, truth[term], half_difference, selected) if selected.any() else None
            per_frame['terms'][term] = result
            error = np.linalg.norm(engine-truth[term], axis=2)
            scale = max(float(np.quantile(error[mask], .95)), 1e-8)
            heat = np.clip(error/scale, 0, 1)
            image = np.stack([heat, heat*.2, 1-heat], axis=2)
            image[~mask] = 0
            Image.fromarray((image*255).astype(np.uint8)).save(args.output/(name+'-'+term+'.png'))
        report['frames'][name] = per_frame
    report['seconds'] = time.perf_counter()-started
    build_hash = hashlib.sha256((run['executable_sha256']+json.dumps(run['shaders'], sort_keys=True)).encode()).hexdigest()
    baseline = args.output.parent/'baselines'/run['dataset_sha256']/build_hash/'report.json'
    if baseline.exists():
        previous = read_json(baseline)
        report['baseline'] = str(baseline)
        report['rms_changes'] = {name: {term: values['terms'][term]['noise_corrected_rms']-
                                        previous['frames'][name]['terms'][term]['noise_corrected_rms'] for term in TERMS}
                                 for name, values in report['frames'].items()}
    write_json(args.output/'report.json', report)
    if args.accept_baseline and not baseline.exists():
        baseline.parent.mkdir(parents=True, exist_ok=True)
        with baseline.open('x', encoding='utf-8') as stream:
            json.dump(report, stream, indent=2, allow_nan=False)
        baseline.chmod(stat.S_IREAD)
    lines = ['# Ground-truth comparison', '', '| Frame | Matched | Direct RMS | Sky RMS | Bounce RMS |',
             '|---|---:|---:|---:|---:|']
    for name, values in report['frames'].items():
        rms = [values['terms'][term]['noise_corrected_rms'] for term in TERMS]
        lines.append(f'| {name} | {values["match_rate"]:.1%} | '+ ' | '.join(f'{value:.6g}' for value in rms)+' |')
    lines += ['', f'Comparison: {report["seconds"]:.2f}s. Engine: {run["seconds"]:.2f}s.',
              'Metrics describe changes; no arbitrary quality pass/fail threshold is applied.']
    (args.output/'summary.md').write_text('\n'.join(lines)+'\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    default = Path(os.environ.get('ZEN_GROUND_TRUTH_ROOT', 'E:/Dev/ZenGroundTruth'))/'newsponza/v2'
    for name in ('benchmark', 'build', 'verify', 'run', 'compare'):
        command = commands.add_parser(name)
        command.add_argument('--dataset', type=Path, default=default)
        if name in ('benchmark', 'build'):
            command.add_argument('--template', type=Path, required=True)
            command.add_argument('--width', type=int, default=960)
            command.add_argument('--height', type=int, default=540)
            command.add_argument('--samples', type=int, default=256)
            command.add_argument('--batch', type=int, default=16)
            command.add_argument('--seed', type=int, default=73)
            command.add_argument('--budget-seconds', type=float, default=720)
            if name == 'benchmark':
                command.add_argument('--output', type=Path, required=True)
        elif name in ('run', 'compare'):
            command.add_argument('--output', type=Path, required=True)
            command.add_argument('--accept-baseline', action='store_true')
            if name == 'run':
                command.add_argument('--executable', type=Path, default=ROOT/'build/x64-windows-msvc-release/bin/ZenApp.exe')
    args = parser.parse_args()
    if args.command in ('build', 'benchmark'):
        if not (0 < args.width <= 8192 and 0 < args.height <= 8192 and args.batch > 0
                and args.samples >= 2*args.batch and args.samples % (2*args.batch) == 0 and args.budget_seconds > 0):
            parser.error('Positive extents <=8192, positive budget and a sample count divisible by twice the batch are required')
    if args.command == 'verify':
        verify(args.dataset)
        print('Dataset hashes verified')
    else:
        globals()[args.command](args)


if __name__ == '__main__':
    main()
