"""Render-pipeline hardening baselines through standalone ZenApp.

Run with build/venv/Scripts/python.exe. Replays the hardening presets, with reduced
Compute after half-diffuse removal, repeats A/A measurements and retains provenance.
Pan rotates the target about a fixed eye: the investigation's target-centred orbit
moved the camera through Sponza geometry and invalidated its last-pose metric.
The dolly translates eye and target by 0.5% of sceneScale over 64 frames.

All results are display-space 8-bit measurements. The matched-4k reference is a
sparse ZenApp Monte Carlo reference, not independent Mitsuba ground truth.
"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import sys

import numpy as np
from PIL import Image

from gi_matched_quality import capture, image, metrics
from gi_spark_metric import camera_path, find_sparks, rotate_target, summarize
from gi_stability import DEFAULT_EXE, ROOT, digest, provenance, read_json, write_json, zenapp
from profile_hybrid_gi import running_renderers

PRESETS = ('hardware', 'compute', 'editor')
MOTIONS = ('pan-slow', 'pan-fast', 'dolly')
POSES = (8, 32, 64)
WARMUP = 160


def load_trace_breakdown(path):
    if Path(path).stat().st_size % 4:
        raise ValueError('Invalid GI trace breakdown size')
    words = np.fromfile(path, '<u4')
    if len(words) < 6 or tuple(words[:2]) != (0x52544947, 1) or words[4] != 8 or words[5] != 0:
        raise ValueError('Unsupported GI trace breakdown header')
    width, height = int(words[2]), int(words[3])
    if width < 1 or height < 1 or len(words) != 6 + width * height * 8:
        raise ValueError('Invalid GI trace breakdown size')
    return words[6:].reshape(height, width, 8)


def denoiser_error_blocks(data, actual, expected, block_size=32, threshold=4, traces=None):
    """Summarize the actual diagnostic texels under display-space error blocks.

    Capture channels 13..17 use sky/bounce/reflection/visibility order. A dump
    repeats GI texels at output resolution, so statistics are pixel-weighted.
    """
    if data.shape[2] != 18 or not np.any(data[:, :, 13, :] > 0):
        raise ValueError('Capture needs ReLAX with denoiserValidation enabled')
    if actual.shape != expected.shape or actual.shape[:2] != data.shape[:2]:
        raise ValueError('Diagnostic capture and error images must have matching extents')
    if block_size < 1:
        raise ValueError('Block size must be positive')
    if traces is not None and traces.shape != (*data.shape[:2], 8):
        raise ValueError('Trace and diagnostic captures must have matching extents')
    error = np.max(np.abs(actual[..., :3] - expected[..., :3]), axis=-1)
    valid = data[:, :, 8, 3] > 0
    reasons = ('accepted', 'reset', 'outside_view', 'identity', 'plane', 'normal', 'geometric', 'roughness', 'view')
    sources = ('own_history', 'pyramid_1', 'pyramid_2', 'pyramid_3', 'cache_seed')
    blocks = []
    height, width = error.shape
    for y in range(0, height, block_size):
        for x in range(0, width, block_size):
            mask = valid[y:y + block_size, x:x + block_size] & (error[y:y + block_size, x:x + block_size] > threshold)
            if not mask.any():
                continue
            texels = data[y:y + block_size, x:x + block_size][mask]
            signals = {}
            for index, signal in enumerate(('sky', 'bounce', 'reflection', 'visibility')):
                reprojection = texels[:, 15, index].astype(np.uint32)
                repair = texels[:, 17, index].astype(np.uint32)
                signals[signal] = dict(
                    slow_age_mean=float(texels[:, 13, index].mean()),
                    slow_age_min=float(texels[:, 13, index].min()),
                    fast_age_mean=float(texels[:, 14, index].mean()),
                    clamp_sigmas_mean=float(texels[:, 16, index].mean()),
                    clamp_sigmas_max=float(texels[:, 16, index].max()),
                    rejection={name: int(np.sum((reprojection & 15) == code)) for code, name in enumerate(reasons)},
                    footprint={name: int(np.sum((reprojection >> 4) == code))
                               for code, name in enumerate(('none', 'bilinear', 'bicubic'))},
                    repair={name: int(np.sum(repair == code)) for code, name in enumerate(sources)})
            block = dict(x=x, y=y, error_pixels=int(mask.sum()), signals=signals)
            if traces is not None:
                values = traces[y:y + block_size, x:x + block_size][mask]
                block['trace_means'] = {name: float(values[:, index].mean()) for index, name in enumerate(
                    ('gi', 'hit_light', 'hit_sky', 'reflection', 'cache_queries', 'cache_hits'))}
            blocks.append(block)
    return dict(threshold=threshold, block_size=block_size, blocks=blocks)


def preset(source, name, compute_pixel_divisor=2):
    setup = copy.deepcopy(source)
    # Replay archived pre-removal setups without passing deleted controls to ZenApp.
    for removed in ('gather', 'probeSpacing', 'bounceSource', 'coneAngleDegrees', 'stepScale',
                    'maxDistanceGridLengths', 'coneCount', 'maxSteps', 'diffuseResolution', 'featureProviders', 'adaptiveSampling', 'previousFrameRadiance'):
        setup['rendering']['gi']['cone'].pop(removed, None)
    provider = 1 if name == 'compute' else 2
    setup['rendering']['gi']['cone'].update(
        rayProvider=provider,
        pixelResolutionDivisor=2 if name == 'hardware' else compute_pixel_divisor if name == 'compute' else 1,
        restirGI=name == 'hardware', restirSpatial=True, restirSpecular=True,
        referenceSamples=0, referenceTileStride=1,
        temporal=True, filter=True, historyFrames=32)
    setup['rendering']['gi'].pop('voxelizer', None)
    # A setup saved in the optimal configuration would re-resolve and ignore the explicit fields above.
    setup['rendering']['gi']['optimal'] = False
    setup['rendering']['gi']['asyncCompute'] = 1
    setup['run'].update(windowWidth=3840, windowHeight=2160, renderWidth=3840,
                        renderHeight=2160, fullscreen=True, followWindow=False,
                        vsync=False, rhiMode=1, validation=True)
    setup['camera']['aspect'] = 3840 / 2160
    setup['animationEnabled'] = False
    return setup


def motion_path(setup, kind, last=65):
    eye = np.asarray(setup['camera']['eye'], dtype=float)
    target = np.asarray(setup['camera']['target'], dtype=float)
    direction = (target - eye) / np.linalg.norm(target - eye)
    keys = [dict(frame=frame, eye=eye.tolist(), target=target.tolist()) for frame in (1, WARMUP)]
    for step in range(1, last + 1):
        if kind == 'dolly':
            shift = direction * setup['camera']['sceneScale'] * .005 * step / 64
            pose_eye, pose_target = (eye + shift).tolist(), (target + shift).tolist()
        else:
            speed = .25 if kind == 'pan-slow' else 1.
            pose_eye, pose_target = eye.tolist(), rotate_target(eye, target, speed * step)
        keys.append(dict(frame=WARMUP + step, eye=pose_eye, target=pose_target))
    return dict(keys=keys)


def check_state(state, setup, frame):
    cone = setup['rendering']['gi']['cone']
    compute = cone['rayProvider'] == 1
    probes = cone.get('screenProbeGI', False) and cone.get('referenceSamples', 0) == 0
    # 0 selects the GI resolution automatically: quarter per axis at 2160 lines (ResolvePixelResolutionDivisor).
    divisor = 1 if probes else cone['pixelResolutionDivisor'] or 4
    expected = dict(sceneFrames=frame, width=3840, height=2160,
                    framebufferWidth=3840, framebufferHeight=2160, fullscreen=True,
                    backend='compute' if compute else 'hardware', gather='screen_probes' if probes else 'pixel',
                    giWidth=3840 // divisor, giHeight=2160 // divisor,
                    hardwareFeatureMask=0 if compute else 63, fieldTraversal=compute,
                    voxelLighting=compute,
                    asyncCompute='available' if setup['rendering']['gi']['asyncCompute'] else 'disabled by configuration',
                    gpuCullingPath='gpu_indirect')
    # The GI denoiser always runs outside reference diagnostics (relaxDenoiser was removed).
    expected['giDenoiser'] = not probes and cone.get('referenceSamples', 0) == 0
    if probes:
        specular = bool(cone.get('specularOcclusion', True))
        expected.update(glossyDenoiser=specular, screenProbeCount=162000 if specular else 259200)
    if cone.get('rayTracingPipeline', False):
        expected['rayTracingPipeline'] = not compute
    for key, value in expected.items():
        if state.get(key) != value:
            raise ValueError(f'Unexpected captured {key}: {state.get(key)!r}, expected {value!r}')


def record(args, folder, setup, start, end, keep=None, path=None):
    return capture(args, folder, setup, start, end, keep=keep, path=path, state_check=check_state)


def flicker(args, folder, setup):
    record(args, folder, setup, 289, 320)
    # Online variance avoids a 3 GB float stack at 4K.
    mean = np.zeros((2160, 3840), np.float64)
    m2 = np.zeros_like(mean)
    previous = None
    lag_sum = lag_above = 0
    for count, frame in enumerate(range(289, 321), 1):
        value = image(folder / f'frame-{frame}.png')
        peak = value.max(axis=2)
        delta = peak - mean
        mean += delta / count
        m2 += delta * (peak - mean)
        if previous is not None:
            lag = np.abs(value - previous).max(axis=2)
            lag_sum += float(lag.sum(dtype=np.float64))
            lag_above += int(np.count_nonzero(lag > 4))
        previous = value
    std = np.sqrt(m2 / 32)
    Image.fromarray(np.clip(std * 32, 0, 255).astype(np.uint8)).save(folder / 'std-x32.png')
    reference = np.load(args.reference / 'static/center/reference-mean.npy', allow_pickle=False)
    mask = np.asarray(Image.open(args.reference_mask)) > 0
    reference_setup = read_json(args.reference / 'static/center/half/setup.zenrender')
    # Reference scoring is meaningful only for exactly the same camera and scene shading.
    for key in ('camera', 'packagePath', 'sourcePath'):
        if setup[key] != reference_setup[key]:
            raise ValueError(f'Reference {key} differs from candidate')
    for key in ('lights', 'environment', 'normalMaps', 'cameraLight'):
        if setup['rendering'][key] != reference_setup['rendering'][key]:
            raise ValueError(f'Reference rendering.{key} differs from candidate')
    return dict(frames=32, mean_std=float(std.mean()), p99_std=float(np.percentile(std, 99)),
                p999_std=float(np.percentile(std, 99.9)), pct_std_gt_1=float((std > 1).mean() * 100),
                pct_std_gt_2=float((std > 2).mean() * 100), mean_lag1=lag_sum / (31 * std.size),
                pct_lag1_gt_4=100 * lag_above / (31 * std.size),
                static_reference=metrics(previous, reference, mask))


def motion(args, folder, setup, kind):
    path = motion_path(setup, kind)
    # Capture frame 224 while the path and application continue through frame 225.
    record(args, folder / 'moving', setup, WARMUP + min(POSES), WARMUP + 65,
           keep=[WARMUP + step for step in POSES], path=path)
    rows = []
    for step in POSES:
        key = path['keys'][step + 1]
        still = copy.deepcopy(setup)
        still['camera'].update(eye=key['eye'], target=key['target'])
        record(args, folder / f'still-{step}', still, 256, 256)
        moving = image(folder / 'moving' / f'frame-{WARMUP + step}.png')
        settled = image(folder / f'still-{step}' / 'frame-256.png')
        error = np.abs(moving - settled).max(axis=2)
        Image.fromarray(np.clip(error * 8, 0, 255).astype(np.uint8)).save(folder / f'error-x8-{step}.png')
        rows.append(dict(pose=step, **metrics(moving, settled),
                         black_percent=float((moving.max(axis=2) == 0).mean() * 100)))
    return dict(kind=kind, poses=rows, degrees_per_frame={'pan-slow': .25, 'pan-fast': 1.}.get(kind),
                dolly_scene_scale_fraction=.005 if kind == 'dolly' else 0)


def endpoint(args, folder, setup):
    """Same pose at the last key and an interior key, with identical motion history."""
    short = motion_path(setup, 'pan-slow', last=64)
    extended = motion_path(setup, 'pan-slow', last=65)
    for name, path, end in (('last-key', short, 224), ('interior-key', extended, 225)):
        record(args, folder / name, setup, 224, end, keep=[224], path=path)
    return metrics(image(folder / 'last-key/frame-224.png'), image(folder / 'interior-key/frame-224.png'))


def sparks(args, folder, setup):
    reports = {}
    for camera in ('moving', 'static'):
        destination = folder / camera
        record(args, destination, setup, 129, 144, path=camera_path(setup, camera, 128, 16, .5))
        values = np.stack([np.asarray(Image.open(destination / f'frame-{frame}.png'), dtype=np.int16)
                           for frame in range(129, 145)])
        rows, pixels = find_sparks(values, 48)
        reports[camera] = summarize(rows, pixels, 12)
        np.savez_compressed(destination / 'spark-components.npz', components=rows)
    return reports


def profile(args, folder, setup):
    folder.mkdir(parents=True, exist_ok=True)
    setup_path = folder / 'setup.zenrender'
    write_json(setup_path, setup)
    if not (folder / 'rdg.json').exists():
        command = zenapp(args, [f'--setup={setup_path}', '--frames=320', '--fixed-step', '--no-ui',
                               '--rhi=threaded', '--async-compute=1', '--gpu-culling=1', '--validation=1',
                               '--fullscreen', '--width=3840', '--height=2160', f'--profile={folder / "rdg"}'],
                         folder / 'rdg.log')
        check_state(read_json(folder / 'rdg.summary.json'), setup, 320)
        log = (folder / 'rdg.log').read_text(errors='replace')
        if any(marker in log for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error')):
            raise ValueError(f'Validation failed: {folder}')
        write_json(folder / 'rdg.json', dict(command=command, sha256=digest(folder / 'rdg.rdg.log')))
    if not (folder / 'nsight/summary.json').exists():
        backend = 'compute' if setup['rendering']['gi']['cone']['rayProvider'] == 1 else 'hardware'
        subprocess.run([sys.executable, str(ROOT / 'tools/profile_hybrid_gi.py'), f'--exe={args.executable}',
                        f'--setup={setup_path}', f'--backend={backend}', f'--output={folder / "nsight"}'],
                       cwd=ROOT, check=True)
    return read_json(folder / 'nsight/summary.json')


def main(args):
    if running_renderers():
        raise ValueError('Close other Zen renderers before measuring')
    args.output = args.output.resolve()
    args.setup = args.setup.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    identity = provenance(args, args.setup, [])
    identity.update(probe_sha256=digest(__file__),
                    compute_pixel_divisor=args.compute_pixel_divisor,
                    capture_tool_sha256=digest(ROOT / 'tools/gi_matched_quality.py'),
                    profile_tool_sha256=digest(ROOT / 'tools/profile_hybrid_gi.py'),
                    spark_tool_sha256=digest(ROOT / 'tools/gi_spark_metric.py'),
                    reference_sha256=digest(args.reference / 'static/center/reference-mean.npy'),
                    reference_mask_sha256=digest(args.reference_mask))
    identity_path = args.output / 'identity.json'
    if identity_path.exists() and read_json(identity_path) != identity:
        raise ValueError('Baseline inputs changed; use a new output directory')
    write_json(identity_path, identity)
    source = read_json(args.setup)
    modes = ('flicker', *MOTIONS, 'sparks', 'profile') if args.mode == 'all' else (args.mode,)
    reports = {}
    for name in args.presets:
        setup = preset(source, name, args.compute_pixel_divisor)
        write_json(args.output / f'{name}.zenrender', setup)
        for repeat in range(args.repeats):
            for mode in modes:
                folder = args.output / name / f'run-{repeat}' / mode
                report_path = folder / 'report.json'
                if report_path.exists():
                    result = read_json(report_path)
                else:
                    if mode in MOTIONS:
                        result = motion(args, folder, setup, mode)
                    else:
                        result = {'flicker': flicker, 'sparks': sparks, 'profile': profile,
                                  'endpoint': endpoint}[mode](args, folder, setup)
                    write_json(report_path, result)
                reports[f'{name}/{repeat}/{mode}'] = result
                print(name, repeat, mode, json.dumps(result)[:400], flush=True)
    current = provenance(args, args.setup, [])
    if any(identity[key] != value for key, value in current.items()):
        raise ValueError('Measurement inputs changed during the run')
    write_json(args.output / f'report-{args.mode}.json', reports)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--presets', nargs='+', choices=PRESETS, default=list(PRESETS))
    parser.add_argument('--mode', choices=('all', 'flicker', *MOTIONS, 'sparks', 'profile', 'endpoint'), default='all')
    parser.add_argument('--repeats', type=int, choices=(1, 2), default=2)
    parser.add_argument('--compute-pixel-divisor', type=int, choices=(1, 2, 4), default=2,
                        help='Compute GI divisor (default: 2); archived Phase 0 retains the native/half-diffuse baseline')
    parser.add_argument('--reference', type=Path,
                        default=Path('E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2'))
    parser.add_argument('--reference-mask', type=Path,
                        default=Path('E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-reference-module/mask.png'))
    parser.add_argument('--diagnostic-capture', type=Path, help='Linear capture prefix with denoiserValidation enabled')
    parser.add_argument('--diagnostic-actual', type=Path, help='Display-space PNG corresponding to the capture')
    parser.add_argument('--diagnostic-expected', type=Path, help='Matched display-space reference PNG')
    parser.add_argument('--diagnostic-traces', type=Path, help='Optional GITR trace dump with the same texel extent')
    args = parser.parse_args()
    if args.diagnostic_capture:
        if not args.diagnostic_actual or not args.diagnostic_expected:
            parser.error('Diagnostic reports require --diagnostic-actual and --diagnostic-expected')
        from compare_hybrid_gi import load
        metadata, data = load(args.diagnostic_capture)
        traces = load_trace_breakdown(args.diagnostic_traces) if args.diagnostic_traces else None
        report = denoiser_error_blocks(data, image(args.diagnostic_actual), image(args.diagnostic_expected), traces=traces)
        report['capture'] = str(args.diagnostic_capture)
        report['capture_sha256'] = digest(Path(str(args.diagnostic_capture) + '.lighting.json'))
        args.output.mkdir(parents=True, exist_ok=True)
        write_json(args.output / 'denoiser-error-blocks.json', report)
    else:
        if not args.setup:
            parser.error('--setup is required for measurement runs')
        main(args)
