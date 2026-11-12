"""Matched-quality gate of one HybridTrace candidate against an existing matched dataset's reference.

The reference is the four-ray quality reference of `gi_matched_quality.py` (a high-sample,
unfiltered ZenApp reference); its images are read from --reference-dataset and never recaptured.
The candidate is an isolated environment-off buffered trace module (--module), or nothing for the
same-build control. Phases:

- static: five views at quarter and half GI, frame 256, against the reference means;
- motion: the eight-pose orbit/dolly of the matched protocol, each pose against its own settled preset;
- sparks: the matched spark protocol at GI divisors 4, 2 and 1, moving and static cameras;
- noise: the 4K noise-protocol term means (ground-truth batch of the center view, normal maps off).

Use build/venv/Scripts/python.exe. Every capture runs with Vulkan validation and records its
executable, setup, module and exact arguments (gi_matched_quality.capture).
"""
import argparse
import copy
import math
from pathlib import Path

import numpy as np
from PIL import Image

from gi_matched_quality import capture, configurations, image, metrics, preset
from gi_spark_metric import camera_path, find_sparks, summarize
from gi_stability import DEFAULT_EXE, TERMS, digest, orbit, read_json, write_json, zenapp

RESOLUTIONS = {'quarter': 4, 'half': 2, 'native': 1}


def with_divisor(setup, divisor):
    result = copy.deepcopy(setup)
    result['rendering']['gi']['cone']['pixelResolutionDivisor'] = divisor
    return result


def reference_mean(args, view):
    folder = args.reference_dataset / 'static' / view
    extended = sorted(folder.glob('reference-*/mean.npy'))
    return np.load(extended[-1] if extended else folder / 'reference-mean.npy')


def static(args, views):
    mask = np.asarray(Image.open(args.reference_mask)) > 0
    rows = []
    for name, setup in views.items():
        reference = reference_mean(args, name)
        for resolution in ('quarter', 'half'):
            folder = args.output / 'static' / name / resolution
            capture(args, folder, preset(setup, resolution), 240, 256, keep=[256], module=args.module)
            row = dict(view=name, preset=resolution, frame_256=metrics(image(folder / 'frame-256.png'), reference, mask))
            rows.append(row)
            print(name, resolution, f"relative RMS {row['frame_256']['relative_rms_percent']:.3f}%",
                  f"PSNR {row['frame_256']['psnr_db']:.3f} dB", flush=True)
    pooled = {}
    for resolution in ('quarter', 'half'):
        selected = [row['frame_256'] for row in rows if row['preset'] == resolution]
        mse = np.mean([row['rms_8bit'] ** 2 for row in selected])
        signal = np.mean([(row['rms_8bit'] / (row['relative_rms_percent'] / 100)) ** 2 for row in selected])
        pooled[resolution] = dict(rms_8bit=math.sqrt(mse), relative_rms_percent=100 * math.sqrt(mse / signal),
                                  psnr_db=20 * math.log10(255 / math.sqrt(mse)))
    return dict(rows=rows, pooled=pooled)


def motion(args, setup):
    eye, target = setup['camera']['eye'], setup['camera']['target']
    keys = [dict(frame=1, eye=eye, target=target), dict(frame=160, eye=eye, target=target)]
    for step in range(1, 9):
        t = step / 8
        rotated = np.asarray(orbit(eye, target, .5 * t))
        pose = np.asarray(target) + (rotated - target) * (1 - .005 * t)
        keys.append(dict(frame=160 + step, eye=pose.tolist(), target=target))
    path = dict(keys=keys)
    reports = {}
    for resolution in ('quarter', 'half'):
        folder = args.output / 'motion' / resolution
        moving = preset(setup, resolution)
        capture(args, folder / 'moving', moving, 161, 168, path=path, module=args.module)
        rows = []
        for key in keys[2:]:
            still = copy.deepcopy(moving)
            still['camera'].update(eye=key['eye'], target=key['target'])
            capture(args, folder / f"still-{key['frame']}", still, 240, 256, keep=[240, 256], module=args.module)
            rows.append(metrics(image(folder / 'moving' / f"frame-{key['frame']}.png"),
                                image(folder / f"still-{key['frame']}" / 'frame-256.png')))
        reports[resolution] = {name: float(np.mean([row[name] for row in rows])) for name in rows[0]}
        print('motion', resolution, f"mean max-channel {reports[resolution]['mean_max_channel_8bit']:.4f}/255",
              flush=True)
    return reports


def sparks(args, setup):
    reports = {}
    for resolution, divisor in RESOLUTIONS.items():
        configured = with_divisor(setup, divisor)
        reports[resolution] = {}
        for camera in ('moving', 'static'):
            folder = args.output / 'sparks' / resolution / camera
            capture(args, folder, configured, 129, 144, path=camera_path(configured, camera, 128, 16, .5),
                    module=args.module)
            values = np.stack([np.asarray(Image.open(folder / f'frame-{frame}.png'), dtype=np.int16)
                               for frame in range(129, 145)])
            rows, pixels = find_sparks(values, 48)
            result = summarize(rows, pixels, 12)
            write_json(folder / 'report.json', result)
            reports[resolution][camera] = result['per_frame_mean']
            # The PNG hashes stay in capture.json; the 16 full frames per run are not retained.
            for frame in range(129, 145):
                (folder / f'frame-{frame}.png').unlink()
            print('sparks', resolution, camera, result['per_frame_mean'], flush=True)
            del values
    return reports


def noise(args, setup):
    """The 4K noise protocol of gi_stability.py: settled ground-truth batch, normal maps off
    (the batch rejects normal-mapped setups)."""
    output = args.output / 'noise'
    output.mkdir(parents=True, exist_ok=True)
    setup = copy.deepcopy(setup)
    width, height = setup['run']['renderWidth'], setup['run']['renderHeight']
    setup['animationEnabled'] = False
    setup['rendering']['normalMaps'] = False
    setup['run'].update(windowWidth=min(width, 1920), windowHeight=min(height, 1080), renderWidth=width,
                        renderHeight=height, fullscreen=False, followWindow=False)
    setup['camera']['aspect'] = width / height
    dataset = output / 'dataset'
    (dataset / 'frames' / 'view').mkdir(parents=True, exist_ok=True)
    write_json(dataset / 'frames' / 'view' / 'scene.zenrender', setup)
    write_json(dataset / 'dataset.json', dict(version=1, frames=[dict(id='view', noise_rms=dict(
        D_direct=1.0, D_sky=1.0, D_bounce=1.0))]))
    run = output / 'run'
    if not (run / 'view.json').exists():
        extra = [f'--hybrid-trace-buffered-module={args.module.resolve()}'] if args.module else []
        zenapp(args, ['--ground-truth=' + str(dataset), '--output=' + str(run), '--frames=256', *extra],
               output / 'zenapp.log')
    lighting = np.memmap(run / 'view.lighting.bin', '<f4', mode='r', shape=(height, width, 10, 4))
    valid = lighting[:, :, 0, 3] > 0
    terms = {name: float(lighting[:, :, index, :3].mean(2)[valid].mean()) for name, index in TERMS.items()}
    del lighting
    for path in run.glob('*.bin'):
        path.unlink()
    print('noise means', {name: terms[name] for name in ('color', 'specular_gi', 'D_bounce')}, flush=True)
    return dict(pixels=int(valid.sum()), means=terms)


def main(args):
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    identity = read_json(args.reference_dataset / 'identity.json')
    if digest(args.reference_mask) != identity['reference_mask_sha256']:
        raise ValueError('The sampling mask is not the reference dataset\'s mask')
    views = configurations(args)
    report_path = args.output / 'report.json'
    report = read_json(report_path) if report_path.exists() else {}
    report.update(executable_sha256=digest(args.executable), module=str(args.module) if args.module else None,
                  module_sha256=digest(args.module) if args.module else None,
                  reference_dataset=str(args.reference_dataset), setup_sha256=digest(args.setup),
                  half_setup_sha256=digest(args.half_setup), tool_sha256=digest(__file__))
    phases = args.phase or ['static', 'motion', 'sparks', 'noise']
    if 'static' in phases:
        report['static'] = static(args, views)
    if 'motion' in phases:
        report['motion'] = motion(args, views['center'])
    if 'sparks' in phases:
        report['sparks'] = sparks(args, views['center'])
    if 'noise' in phases:
        report['noise'] = noise(args, views['center'])
    if digest(args.executable) != report['executable_sha256']:
        raise ValueError('The executable changed during the measurement')
    write_json(report_path, report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--half-setup', type=Path, required=True)
    parser.add_argument('--reference-dataset', type=Path, required=True)
    parser.add_argument('--reference-mask', type=Path, required=True)
    parser.add_argument('--module', type=Path, help='Environment-off buffered candidate module; omit for the control')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cameras', type=Path, default=Path('E:/Dev/ZenGroundTruth/newsponza/v3'))
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--phase', action='append', choices=('static', 'motion', 'sparks', 'noise'))
    main(parser.parse_args())
