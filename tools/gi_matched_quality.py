"""Matched 4K display-image accuracy and motion measurements through standalone ZenApp.

The reference is ZenApp's unfiltered, full-resolution Monte Carlo diagnostic, not an
independent renderer. It shares material shading with the candidate. Normal maps,
prepared materials, lights, exposure and framebuffer settings stay unchanged. Metrics
score captured RGB in display space; they are not linear irradiance error metrics.
"""
import argparse
import copy
import json
import math
from pathlib import Path
import time

import numpy as np
from PIL import Image

from gi_stability import DEFAULT_EXE, ROOT, digest, orbit, provenance, read_json, write_json, zenapp


def image(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32)


def average(folder, frames):
    result = None
    for frame in frames:
        value = image(folder / f'frame-{frame}.png')
        result = value if result is None else result + value
    return result / len(frames)


def metrics(actual, reference, mask=None):
    delta = actual - reference
    selected = np.ones(delta.shape[:2], bool) if mask is None else mask
    weights = np.ones(selected.shape, np.float64)
    if mask is not None:
        # The last 48 rows have six workgroups per stratum instead of eight.
        weights[-48:] = .75
    weights = weights[selected]
    mse = float(np.average(np.mean(np.square(delta[selected], dtype=np.float64), axis=1), weights=weights))
    rms = math.sqrt(mse)
    signal = math.sqrt(float(np.average(np.mean(np.square(reference[selected], dtype=np.float64), axis=1), weights=weights)))
    maximum = np.max(np.abs(delta), axis=2)[selected]
    return dict(rms_8bit=rms, relative_rms_percent=100 * rms / max(signal, 1e-30),
                psnr_db=20 * math.log10(255 / rms) if rms else None,
                mean_max_channel_8bit=float(np.average(maximum, weights=weights)), p99_max_channel_8bit=float(np.quantile(maximum, .99)),
                max_channel_8bit=float(maximum.max()), above_4_percent=float(np.average(maximum > 4, weights=weights) * 100),
                above_16_percent=float(np.average(maximum > 16, weights=weights) * 100),
                mean_bias_8bit=float(np.average(delta[selected].mean(1), weights=weights)))


def check_state(state, setup, frame):
    run, cone = setup['run'], setup['rendering']['gi']['cone']
    divisor = 1 if cone['referenceSamples'] else cone['pixelResolutionDivisor']
    expected = dict(sceneFrames=frame, width=run['renderWidth'], height=run['renderHeight'],
                    framebufferWidth=run['windowWidth'], framebufferHeight=run['windowHeight'],
                    fullscreen=run['fullscreen'], backend='hardware', gather='pixel', hardwareFeatureMask=63,
                    giWidth=run['renderWidth'] // divisor, giHeight=run['renderHeight'] // divisor,
                    fieldTraversal=False, voxelLighting=False, asyncCompute='available', gpuCullingPath='gpu_indirect')
    for key, value in expected.items():
        if state.get(key) != value:
            raise ValueError(f'Unexpected captured state {key}: {state.get(key)!r}, expected {value!r}')


def capture(args, folder, setup, start, end, keep=None, path=None, module=None):
    """Resume only completed, identity-matched jobs; keep lossless pixels and actual state."""
    folder.mkdir(parents=True, exist_ok=True)
    setup_path = folder / 'setup.zenrender'
    complete = folder / 'capture.json'
    keep = list(range(start, end + 1)) if keep is None else sorted(set(keep))
    if setup_path.exists() and read_json(setup_path) != setup:
        raise ValueError(f'Refusing to replace a different setup: {setup_path}')
    write_json(setup_path, setup)
    if complete.exists():
        record = read_json(complete)
        if (record['executable_sha256'] != digest(args.executable)
                or record['setup_sha256'] != digest(setup_path) or record['frames'] != keep):
            raise ValueError(f'Capture identity changed: {folder}')
        for filename, sha in record['files'].items():
            if digest(folder / filename) != sha:
                raise ValueError(f'Capture changed: {folder / filename}')
        return record
    command = ['--setup=' + str(setup_path), f'--frames={end}', f'--capture-start={start}',
               '--capture=' + str(folder / 'capture'), '--fixed-step', '--no-ui', '--rhi=threaded',
               '--async-compute=1', '--gpu-culling=1', '--validation=1', '--fullscreen', '--width=3840', '--height=2160']
    if path is not None:
        write_json(folder / 'camera-path.json', path)
        command.append('--camera-path=' + str(folder / 'camera-path.json'))
    if module is not None:
        command.append('--hybrid-trace-buffered-module=' + str(module.resolve()))
    print('Capture', folder.relative_to(args.output), flush=True)
    started = time.perf_counter()
    exact = zenapp(args, command, folder / 'zenapp.log')
    log = (folder / 'zenapp.log').read_text(errors='replace')
    for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
        if marker in log:
            raise ValueError(f'Capture validation marker {marker!r}: {folder}')
    hashes = {}
    for frame in range(start, end + 1):
        ppm = folder / f'capture.frame-{frame}.ppm'
        state_path = Path(str(ppm) + '.json')
        state = read_json(state_path)
        check_state(state, setup, frame)
        if frame in keep:
            png = folder / f'frame-{frame}.png'
            with Image.open(ppm) as source:
                source.save(png, compress_level=3)
                with Image.open(png) as saved:
                    if not np.array_equal(np.asarray(source), np.asarray(saved)):
                        raise ValueError(f'Lossless image conversion failed: {png}')
            write_json(folder / f'frame-{frame}.json', state)
            hashes[png.name] = digest(png)
            hashes[f'frame-{frame}.json'] = digest(folder / f'frame-{frame}.json')
        ppm.unlink()
        state_path.unlink()
    record = dict(arguments=exact, executable_sha256=digest(args.executable), setup_sha256=digest(setup_path),
                  frames=keep, seconds=time.perf_counter() - started, files=hashes,
                  module_sha256=digest(module) if module is not None else None)
    write_json(complete, record)
    return record


def configurations(args):
    quarter, half = read_json(args.setup), read_json(args.half_setup)
    expected_half = copy.deepcopy(quarter)
    expected_half['rendering']['gi']['cone']['pixelResolutionDivisor'] = 2
    if expected_half != half or quarter['rendering']['gi']['cone']['pixelResolutionDivisor'] != 4:
        raise ValueError('Performance presets must differ only in GI resolution divisor (4 versus 2)')
    if not quarter['rendering']['normalMaps'] or quarter['rendering']['environment']['lighting']:
        raise ValueError('This comparison expects the profiled normal-mapped, environment-off setup')
    views = {'center': quarter}
    for camera in range(2, 6):
        view = copy.deepcopy(quarter)
        source = read_json(args.cameras / 'frames' / f'C{camera}_L1' / 'scene.zenrender')
        view['camera'].update(eye=source['camera']['eye'], target=source['camera']['target'])
        views[f'C{camera}'] = view
    return views


def preset(setup, name):
    result = copy.deepcopy(setup)
    cone = result['rendering']['gi']['cone']
    if name == 'reference':
        cone.update(referenceSamples=4096, pixelResolutionDivisor=1, restirGI=False,
                    restirSpatial=False, restirSpecular=False, filter=False, adaptiveSampling=False)
    else:
        cone['pixelResolutionDivisor'] = 4 if name == 'quarter' else 2
    return result


def static(args, views):
    for name, setup in views.items():
        folder = args.output / 'static' / name
        control = preset(setup, 'reference')
        control['rendering']['gi']['cone']['referenceSamples'] = 64
        capture(args, folder / 'full-control', control, 1, 1)
        capture(args, folder / 'sampled-control', control, 1, 1, module=args.reference_module)
        mask = np.asarray(Image.open(args.reference_mask)) > 0
        difference = np.max(np.abs(image(folder / 'full-control/frame-1.png')
                                   - image(folder / 'sampled-control/frame-1.png')), axis=2)
        validity = dict(sampled_pixels=int(mask.sum()), different_pixels=int(np.count_nonzero(difference[mask])),
                        maximum=float(difference[mask].max()), byte_identical=bool(not difference[mask].any()),
                        rms_8bit=metrics(image(folder / 'full-control/frame-1.png'),
                                         image(folder / 'sampled-control/frame-1.png'), mask)['rms_8bit'])
        validity['passed'] = validity['rms_8bit'] <= .05 and validity['maximum'] <= 2
        write_json(folder / 'sampling-validation.json', validity)
        if not validity['passed']:
            raise ValueError(f'Sparse reference exceeded the precision bound: {name}: {validity}')
        for resolution in ('quarter', 'half'):
            capture(args, args.output / 'static' / name / resolution, preset(setup, resolution), 240, 256)
        capture(args, args.output / 'static' / name / 'reference', preset(setup, 'reference'), 128, 256,
                keep=[*range(128, 145), *range(240, 257)], module=args.reference_module)
        static_report(args, [name])


def heatmap(actual, reference, path):
    delta = np.max(np.abs(actual - reference), axis=2)
    Image.fromarray(np.clip(delta * 8, 0, 255).astype(np.uint8)).save(path)


def static_report(args, names):
    rows = []
    for name in names:
        folder = args.output / 'static' / name
        mask = np.asarray(Image.open(args.reference_mask)) > 0
        reference = average(folder / 'reference', range(240, 257))
        earlier = average(folder / 'reference', range(128, 145))
        drift = metrics(earlier, reference, mask)
        Image.fromarray(np.rint(reference).astype(np.uint8)).save(folder / 'reference-mean.png')
        np.save(folder / 'reference-mean.npy', reference, allow_pickle=False)
        for resolution in ('quarter', 'half'):
            final = image(folder / resolution / 'frame-256.png')
            mean = average(folder / resolution, range(240, 257))
            single = metrics(final, reference, mask)
            averaged = metrics(mean, reference, mask)
            row = dict(view=name, preset=resolution, frame_256=single, frames_240_256_mean=averaged,
                       reference_drift=drift, sampled_pixels=int(mask.sum()),
                       reference_drift_below_quarter_error=drift['rms_8bit'] < single['rms_8bit'] / 4)
            rows.append(row)
            heatmap(np.where(mask[..., None], final, reference), reference, folder / f'{resolution}-error-x8.png')
            print(name, resolution, f"RMS {single['relative_rms_percent']:.3f}% PSNR {single['psnr_db']:.3f} dB",
                  f"reference drift {drift['rms_8bit']:.4f}/255", flush=True)
        write_json(folder / 'report.json', rows[-2:])
    return rows


def motion(args, setup):
    eye, target = setup['camera']['eye'], setup['camera']['target']
    keys = [dict(frame=1, eye=eye, target=target), dict(frame=160, eye=eye, target=target)]
    for step in range(1, 9):
        t = step / 8
        rotated = np.asarray(orbit(eye, target, .5 * t))
        pose = np.asarray(target) + (rotated - target) * (1 - .005 * t)
        keys.append(dict(frame=160 + step, eye=pose.tolist(), target=target))
    path = dict(keys=keys)
    write_json(args.output / 'motion-path.json', path)
    for resolution in ('quarter', 'half'):
        folder = args.output / 'motion' / resolution
        moving = preset(setup, resolution)
        capture(args, folder / 'moving', moving, 161, 168, path=path)
        for key in keys[2:]:
            still = copy.deepcopy(moving)
            still['camera'].update(eye=key['eye'], target=key['target'])
            capture(args, folder / f"still-{key['frame']}", still, 240, 256)
        motion_report(args, [resolution])


def motion_report(args, resolutions):
    reports = {}
    for resolution in resolutions:
        folder = args.output / 'motion' / resolution
        rows = []
        for frame in range(161, 169):
            moving = image(folder / 'moving' / f'frame-{frame}.png')
            reference = image(folder / f'still-{frame}' / 'frame-256.png')
            previous = image(folder / f'still-{frame}' / 'frame-240.png')
            mean = average(folder / f'still-{frame}', range(240, 257))
            rows.append(dict(frame=frame, versus_static_frame=metrics(moving, reference),
                             versus_static_mean=metrics(moving, mean), static_residual=metrics(previous, reference)))
            heatmap(moving, mean, folder / f'motion-{frame}-error-x8.png')
        report = dict(per_frame=rows)
        for metric in ('versus_static_frame', 'versus_static_mean', 'static_residual'):
            report[metric] = {key: float(np.mean([row[metric][key] for row in rows])) for key in rows[0][metric]}
        reports[resolution] = report
        write_json(folder / 'report.json', report)
        print('Motion', resolution, json.dumps(report['versus_static_frame']), flush=True)
    return reports


def run(args):
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    views = configurations(args)
    identity = provenance(args, args.setup.resolve(), [])
    identity.update(half_setup_sha256=digest(args.half_setup), matched_tool_sha256=digest(__file__),
                    reference_module_sha256=digest(args.reference_module), reference_mask_sha256=digest(args.reference_mask))
    identity_path = args.output / 'identity.json'
    if identity_path.exists() and read_json(identity_path) != identity:
        raise ValueError('Measurement inputs changed; use a new output directory')
    write_json(identity_path, identity)
    write_json(args.output / 'protocol.json', dict(
        reference='ZenApp native-resolution unfiltered Monte Carlo; shared engine material shading, not independent truth',
        reference_samples_per_frame=4096, reference_frames=256, reference_composite_frames=[240, 256],
        sampling='One 8x8 tile per 64x64 stratum; 130560 native pixels, 1.574%; inverse-probability weighted RMS/PSNR',
        reference_check_frames=[128, 144], candidate_frames=[240, 256],
        metric_space='captured display RGB, 0..255; relative RMS normalized by reference RGB RMS; PSNR peak 255',
        motion=dict(warmup=160, frames=8, orbit_degrees=.5, dolly_fraction=.005, static_settle=256),
        limitations=['Reference mode converges GI receivers; forward layers still use their ordinary samples and reuse.',
                     'Reference checkpoint drift is a convergence check, not an independent noise estimate.',
                     'Display metrics include direct light and materials; they are not linear diffuse-only metrics.']))
    if args.phase in ('all', 'static'):
        static(args, {key: value for key, value in views.items() if not args.view or key == args.view})
    if args.phase in ('all', 'motion'):
        motion(args, views['center'])
    if args.phase in ('all', 'report'):
        report = dict(static=static_report(args, views), motion=motion_report(args, ('quarter', 'half')))
        write_json(args.output / 'report.json', report)
    final_identity = provenance(args, args.setup.resolve(), [])
    for key in final_identity:
        if identity[key] != final_identity[key]:
            raise ValueError(f'Measurement input changed during capture: {key}')
    write_json(args.output / f'verified-{args.phase}{"-" + args.view if args.view else ""}.json', identity)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--half-setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference-module', type=Path, required=True)
    parser.add_argument('--reference-mask', type=Path, required=True)
    parser.add_argument('--cameras', type=Path, default=Path('E:/Dev/ZenGroundTruth/newsponza/v3'))
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--phase', choices=('all', 'static', 'motion', 'report'), default='all')
    parser.add_argument('--view', choices=('center', 'C2', 'C3', 'C4', 'C5'))
    run(parser.parse_args())
