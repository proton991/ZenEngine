"""Count GI sparks (one-frame bright outliers) in standalone ZenApp captures.

Use build/venv/Scripts/python.exe. Each run renders a derived copy of the setup with fixed timesteps:
the camera holds the setup pose for --warmup frames, then for --frames frames either alternates between
two poses --angle degrees apart (moving) or holds still (static). Every frame of that window is captured
at the render extent. Data/engine.cfg is never edited.

A spark is a pixel whose maximum channel exceeds both the same pose two frames earlier and two frames
later by more than --threshold (8-bit levels). Sparks are grouped into 8-connected components and counted
per analysed frame by size: one pixel, two to three pixels, and dots of four or more pixels.

capture   Run every --divisors x --cameras combination into <output>/d<divisor>-<camera>/ and write
          report.json there and a summary in <output>/report.json. Provenance records the executable,
          exact argument lists, setup, shader and scene-input hashes and the actual extents, fullscreen,
          backend and async-compute state that ZenApp reported.
          --views renders the same path again once per ZenApp GI debug output and joins it with the
          sparks: fallback (reconstruction fallback receivers), rejection (diffuse and reflection history
          rejection reasons) and length (diffuse and reflection history lengths). Each view's per-frame
          codes are kept in views.npz.
accumulate
          Static-camera reference: averages --frames captured frames per pixel and renders the
          reconstruction-fallback view at the same pose. With --reference <accumulate output>, also
          reports this run's per-frame and averaged error against that reference on its fallback mask
          (display-space 8-bit levels), separating noise from bias.
analyse   Recount a run kept with --keep-captures, optionally against per-pose masks (white = inside):
          --mask pose0.png pose1.png reports how many sparks touch each mask.
"""
import argparse
import copy
import json
from pathlib import Path
import sys

import numpy as np
from PIL import Image, ImageDraw
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json, zenapp

SIZE_CLASSES = {'one': (1, 2), 'two_three': (2, 4), 'four_plus': (4, 1 << 30)}
# RenderingSettings.h DebugOutput values and debug_hybrid.frag palettes (channel levels 0, mid, 255).
DEBUG_OUTPUTS = {'length': 9, 'rejection': 11, 'fallback': 12}
FALLBACK = {(1, 1, 1): 0, (2, 0, 0): 1, (0, 0, 2): 2, (2, 0, 2): 3}
REASONS = {(0, 2, 0): 0, (2, 0, 0): 1, (2, 2, 0): 2, (2, 0, 2): 3, (0, 2, 2): 4, (0, 0, 2): 5, (2, 1, 0): 6, (2, 2, 2): 7,
           (1, 1, 1): 8}
REASON_NAMES = ['accepted', 'reset', 'outside_view', 'identity', 'plane', 'shading_normal', 'geometric_normal',
                'roughness', 'view']
SHORT_HISTORY = 4


def rotate_target(eye, target, degrees):
    """Turns the view direction around the vertical axis through the eye."""
    angle = np.deg2rad(degrees)
    offset = np.asarray(target, float) - np.asarray(eye, float)
    c, s = np.cos(angle), np.sin(angle)
    rotated = np.array([c*offset[0] + s*offset[2], offset[1], -s*offset[0] + c*offset[2]])
    return (np.asarray(eye, float) + rotated).tolist()


def derive_setup(source, divisor, restir, window):
    setup = copy.deepcopy(source)
    cone = setup['rendering']['gi']['cone']
    if divisor:
        cone['pixelResolutionDivisor'] = divisor
    if restir != 'setup':
        cone['restirGI'] = restir == 'on'
    setup['animationEnabled'] = False
    setup['run'].update(windowWidth=window[0], windowHeight=window[1], fullscreen=False, followWindow=False, vsync=False)
    return setup


def camera_path(setup, camera, warmup, frames, angle):
    eye, target = setup['camera']['eye'], setup['camera']['target']
    moved = rotate_target(eye, target, angle)
    keys = [dict(frame=1, eye=eye, target=target), dict(frame=warmup, eye=eye, target=target)]
    for frame in range(warmup + 1, warmup + frames + 1):
        # Odd frames take the second pose, so frame i and i +/- 2 always share a pose.
        second = camera == 'moving' and frame % 2 == 1
        keys.append(dict(frame=frame, eye=eye, target=moved if second else target))
    return dict(keys=keys)


def components(mask):
    """8-connected components of a sparse mask as arrays of (y, x)."""
    remaining = set(map(tuple, np.argwhere(mask)))
    result = []
    while remaining:
        seed = remaining.pop()
        stack, members = [seed], [seed]
        while stack:
            y, x = stack.pop()
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    q = (y + dy, x + dx)
                    if q in remaining:
                        remaining.remove(q)
                        stack.append(q)
                        members.append(q)
        result.append(np.array(members))
    return result


def load_frames(folder, first, count):
    paths = [folder/f'frame.frame-{first + i}.ppm' for i in range(count)]
    missing = [str(p) for p in paths if not p.exists()]
    if missing:
        raise FileNotFoundError(f'Missing captures: {missing[:3]}')
    return np.stack([np.asarray(Image.open(p), dtype=np.int16) for p in paths]), paths


def find_sparks(values, threshold):
    """Rows of (size, height, width, r, g, b, frame index, y, x) and spark pixels per analysed frame."""
    peak = values.max(axis=3)
    rows, pixels = [], []
    for i in range(2, len(values) - 2):
        rise = np.minimum(peak[i] - peak[i - 2], peak[i] - peak[i + 2])
        mask = rise > threshold
        pixels.append(int(mask.sum()))
        for members in components(mask):
            y0, x0 = members.min(axis=0)
            y1, x1 = members.max(axis=0)
            rgb = values[i][members[:, 0], members[:, 1]].mean(axis=0)
            rows.append((len(members), y1 - y0 + 1, x1 - x0 + 1, *rgb, i, y0, x0))
    return np.array(rows, float).reshape(-1, 9), pixels


def summarize(rows, pixels, analysed):
    sizes = rows[:, 0]
    per_frame = {name: [int(((rows[:, 6] == i) & (sizes >= a) & (sizes < b)).sum()) for i in range(2, 2 + analysed)]
                 for name, (a, b) in SIZE_CLASSES.items()}
    return dict(analysed_frames=analysed, spark_pixels_per_frame=float(np.mean(pixels)),
                per_frame_mean={name: float(np.mean(counts)) for name, counts in per_frame.items()},
                per_frame=per_frame,
                mean_rgb=rows[:, 3:6].mean(axis=0).round(2).tolist() if len(rows) else None,
                median_bbox_hw=[float(np.median(rows[:, 1])), float(np.median(rows[:, 2]))] if len(rows) else None)


def overlay(values, rows, path):
    """Last analysed frame with every spark of the window marked: green one pixel, yellow 2-3, magenta 4+."""
    image = Image.fromarray(np.uint8(values[-3])).convert('RGB')
    draw = ImageDraw.Draw(image)
    colours = {1: (0, 255, 0), 2: (255, 255, 0), 4: (255, 0, 255)}
    for size, h, w, *_, frame, y, x in rows:
        colour = colours[4 if size >= 4 else 2 if size >= 2 else 1]
        draw.ellipse([x - 12, y - 12, x + w + 12, y + h + 12], outline=colour, width=3)
    height = values.shape[1]
    scale = min(1.0, 1080 / height)
    image.resize((round(values.shape[2]*scale), round(height*scale)), Image.LANCZOS).save(path)


def mask_hits(rows, masks):
    """Sparks whose bounding box touches the mask of their pose (frame parity)."""
    result = {}
    for name, (a, b) in SIZE_CLASSES.items():
        selected = rows[(rows[:, 0] >= a) & (rows[:, 0] < b)]
        hits = 0
        for size, h, w, *_, frame, y, x in selected:
            mask = masks[int(frame) % len(masks)]
            hits += bool(mask[int(y):int(y + h), int(x):int(x + w)].any())
        result[name] = dict(on_mask=hits, total=len(selected))
    fraction = [float(m.mean()) for m in masks]
    return dict(by_size=result, mask_fraction=fraction)


def decode_palette(image, palette):
    """Per-pixel code of a categorical debug image; 255 where nothing is drawn or the colour is unknown."""
    levels = np.where(image < 16, 0, np.where(image > 239, 2, 1)).astype(np.int32)
    key = levels[..., 0]*9 + levels[..., 1]*3 + levels[..., 2]
    table = np.full(27, 255, np.uint8)
    for (r, g, b), code in palette.items():
        table[r*9 + g*3 + b] = code
    codes = table[key]
    codes[image.max(axis=2) < 16] = 255
    return codes


def decode_view(kind, image, limit):
    if kind == 'fallback':
        return decode_palette(image, FALLBACK)
    if kind == 'rejection':
        return decode_palette(image, REASONS)
    # History length: linear grey, white at the configured limit. 255 marks pixels without a receiver.
    frames = np.minimum(np.round(image[..., 0].astype(np.float32) / 255 * limit), 254).astype(np.uint8)
    frames[image.max(axis=2) == 0] = 255
    return frames


def view_join(kind, rows, codes):
    """Codes under each spark (bounding box) by size class, against the same codes over all receivers."""
    analysed = codes[2:-2]
    receivers = analysed[analysed != 255]
    if kind == 'length':
        baseline = dict(short_fraction=float((receivers < SHORT_HISTORY).mean()) if receivers.size else 0.0)
    else:
        baseline = dict(fraction=[float((receivers == c).mean()) if receivers.size else 0.0 for c in range(9)])
    result = dict(receivers=baseline, by_size={})
    for name, (a, b) in SIZE_CLASSES.items():
        selected = rows[(rows[:, 0] >= a) & (rows[:, 0] < b)]
        counts = np.zeros(9, int)
        short = 0
        for size, h, w, *_, frame, y, x in selected:
            region = codes[int(frame), int(y):int(y + h), int(x):int(x + w)]
            region = region[region != 255]
            if not region.size:
                continue
            if kind == 'length':
                short += bool((region < SHORT_HISTORY).any())
            else:
                # The most specific code: the largest rejection or fallback code under the spark.
                counts[int(region.max())] += 1
        if kind == 'length':
            result['by_size'][name] = dict(total=len(selected), short_history=short)
        else:
            result['by_size'][name] = dict(total=len(selected), codes=counts.tolist())
    if kind == 'fallback':
        fallback = (analysed != 255) & (analysed != 0)
        result['fallback_fraction_per_frame'] = float(fallback.mean())
        result['fallback_pixels_per_frame'] = float(fallback.sum() / len(analysed))
        result['fallback_classes_per_frame'] = [float((analysed == c).sum() / len(analysed)) for c in (1, 2, 3)]
    return result


def capture_view(args, folder, setup, path_file, first, last, kind, face):
    """Re-renders the run's camera path with a GI debug output and returns its per-frame codes."""
    name = kind if kind == 'fallback' else f'{kind}-' + ('reflection' if face else 'diffuse')
    view = copy.deepcopy(setup)
    view['rendering']['debug'].update(output=DEBUG_OUTPUTS[kind], face=face)
    view_setup = folder/f'view-{name}.zenrender'
    write_json(view_setup, view)
    command = zenapp(args, ['--setup=' + str(view_setup), '--camera-path=' + str(path_file), f'--frames={last}',
                            f'--capture-start={first}', '--capture=' + str(folder/f'view-{name}'), '--fixed-step',
                            '--no-ui'], folder/f'view-{name}.log')
    limit = setup['rendering']['gi']['cone']['historyFrames']
    codes = []
    for frame in range(first, last + 1):
        path = folder/f'view-{name}.frame-{frame}.ppm'
        codes.append(decode_view(kind, np.asarray(Image.open(path)), limit))
        path.unlink()
        Path(str(path) + '.json').unlink()
    return name, np.stack(codes), command


def analyse_folder(folder, run, threshold, masks=None):
    values, paths = load_frames(folder, run['capture_start'], run['frames'])
    states = [read_json(str(p) + '.json') for p in paths]
    expected = list(range(run['capture_start'], run['capture_start'] + run['frames']))
    if [s['sceneFrames'] for s in states] != expected:
        raise ValueError('Captured frames do not match the requested window')
    rows, pixels = find_sparks(values, threshold)
    np.save(folder/'sparks.npy', rows)
    overlay(values, rows, folder/'overlay.png')
    result = summarize(rows, pixels, run['frames'] - 4)
    if masks is not None:
        result['mask'] = mask_hits(rows, masks)
    return result, values, paths, states


def capture(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source = read_json(args.setup)
    window = tuple(args.window)
    first = args.warmup + 1
    last = args.warmup + args.frames
    runs = {}
    identity = None
    for divisor in args.divisors:
        for camera in args.cameras:
            name = f'd{divisor}-{camera}'
            folder = output/name
            folder.mkdir()
            setup = derive_setup(source, divisor, args.restir, window)
            setup_path = folder/'setup.zenrender'
            write_json(setup_path, setup)
            path_file = folder/'camera-path.json'
            write_json(path_file, camera_path(setup, camera, args.warmup, args.frames, args.angle))
            if identity is None:
                identity = provenance(args, setup_path, [])
            arguments = ['--setup=' + str(setup_path), '--camera-path=' + str(path_file), f'--frames={last}',
                         f'--capture-start={first}', '--capture=' + str(folder/'frame'), '--fixed-step', '--no-ui',
                         '--profile=' + str(folder/'run')]
            command = zenapp(args, arguments, folder/'zenapp.log')
            run = dict(capture_start=first, frames=args.frames)
            result, values, paths, states = analyse_folder(folder, run, args.threshold)
            actual = read_json(folder/'run.summary.json')
            rows = np.load(folder/'sparks.npy')
            views, view_commands, view_codes = {}, [], {}
            for kind in args.views:
                for face in ((0,) if kind == 'fallback' else (0, 1)):
                    view_name, codes, view_command = capture_view(args, folder, setup, path_file, first, last, kind, face)
                    views[view_name] = view_join(kind, rows, codes)
                    view_codes[view_name] = codes
                    view_commands.append(view_command)
            if view_codes:
                np.savez_compressed(folder/'views.npz', **view_codes)
            result.update(views=views, view_commands=view_commands)
            report = dict(kind='gi_sparks', divisor=divisor, camera=camera, restir=args.restir, angle_degrees=args.angle,
                          warmup=args.warmup, threshold_8bit=args.threshold, command=command, setup=str(setup_path),
                          setup_sha256=digest(setup_path), camera_path_sha256=digest(path_file),
                          actual=actual, capture_state=states[0], **run, **result)
            write_json(folder/'report.json', report)
            if not args.keep_captures:
                for path in paths:
                    path.unlink()
            del values
            runs[name] = report
            mean = result['per_frame_mean']
            print(f"{name}: one-pixel {mean['one']:.1f}, 2-3 px {mean['two_three']:.1f}, dots 4+ {mean['four_plus']:.1f} "
                  f"per frame (GI {actual['giWidth']}x{actual['giHeight']}, render {actual['width']}x{actual['height']})",
                  flush=True)
            if 'fallback' in views:
                fallback = views['fallback']
                hits = {k: v['codes'][:4] for k, v in fallback['by_size'].items()}
                print(f"  fallback receivers {fallback['fallback_pixels_per_frame']:.0f} px/frame "
                      f"({100*fallback['fallback_fraction_per_frame']:.3f}%); sparks by fallback code "
                      f"[none, diffuse, reflection, both]: {hits}", flush=True)
    final = provenance(args, Path(identity['setup']), [])
    for key in ('executable_sha256', 'shaders_sha256', 'inputs', 'tool_sha256'):
        if final[key] != identity[key]:
            raise ValueError(f'Measurement input changed during capture: {key}')
    summary = dict(kind='gi_sparks_summary', source_setup=str(args.setup.resolve()),
                   source_setup_sha256=digest(args.setup), spark_tool_sha256=digest(__file__),
                   runs={name: dict(divisor=r['divisor'], camera=r['camera'], per_frame_mean=r['per_frame_mean'],
                                    spark_pixels_per_frame=r['spark_pixels_per_frame'], gi=[r['actual']['giWidth'],
                                                                                           r['actual']['giHeight']])
                         for name, r in runs.items()},
                   commands=[r['command'] for r in runs.values()], **{k: v for k, v in identity.items() if k != 'commands'})
    write_json(output/'report.json', summary)


def mask_error(values, reference, mask):
    difference = values[mask] - reference[mask]
    return float(np.sqrt(np.mean(difference*difference)))


def accumulate(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    setup = derive_setup(read_json(args.setup), args.divisor, args.restir, tuple(args.window))
    if args.camera_from:
        # Only the pose and lens; scene, package and rendering settings stay those of --setup.
        camera = read_json(args.camera_from)['camera']
        setup['camera'].update({k: camera[k] for k in ('eye', 'target', 'verticalFovDegrees')})
    setup_path = output/'setup.zenrender'
    write_json(setup_path, setup)
    path_file = output/'camera-path.json'
    write_json(path_file, camera_path(setup, 'static', args.warmup, args.frames, 0.0))
    identity = provenance(args, setup_path, [])
    first, last = args.warmup + 1, args.warmup + args.frames
    command = zenapp(args, ['--setup=' + str(setup_path), '--camera-path=' + str(path_file), f'--frames={last}',
                            f'--capture-start={first}', '--capture=' + str(output/'frame'), '--fixed-step', '--no-ui',
                            '--profile=' + str(output/'run')], output/'zenapp.log')
    _, codes, view_command = capture_view(args, output, setup, path_file, last, last, 'fallback', 0)
    reference = None
    mask = (codes[0] != 0) & (codes[0] != 255)
    if args.reference:
        reference = np.load(args.reference/'mean.npy')
        mask = np.load(args.reference/'mask.npy')
    total = None
    errors = []
    for frame in range(first, last + 1):
        path = output/f'frame.frame-{frame}.ppm'
        values = np.asarray(Image.open(path)).astype(np.float64)
        total = values if total is None else total + values
        if reference is not None:
            errors.append(mask_error(values, reference, mask))
        path.unlink()
    mean = (total / args.frames).astype(np.float32)
    np.save(output/'mean.npy', mean)
    np.save(output/'mask.npy', mask)
    report = dict(kind='gi_spark_accumulate', divisor=args.divisor, restir=args.restir, warmup=args.warmup,
                  frames=args.frames, mask_pixels=int(mask.sum()), mask_fraction=float(mask.mean()),
                  actual=read_json(output/'run.summary.json'), spark_tool_sha256=digest(__file__),
                  **dict(identity, commands=[command, view_command]))
    if reference is not None:
        report.update(reference=str(args.reference), per_frame_rmse=errors,
                      mean_frame_rmse=float(np.mean(errors)), averaged_rmse=mask_error(mean, reference, mask),
                      averaged_mean_difference=float(np.mean(mean[mask] - reference[mask])))
        print(f"mask {int(mask.sum())} px: single-frame RMSE {report['mean_frame_rmse']:.2f}, "
              f"{args.frames}-frame average RMSE {report['averaged_rmse']:.2f}, "
              f"mean difference {report['averaged_mean_difference']:+.2f} (8-bit levels)", flush=True)
    else:
        print(f"reference: {args.frames} frames, fallback mask {int(mask.sum())} px ({100*mask.mean():.3f}%)", flush=True)
    write_json(output/'report.json', report)


def analyse(args):
    folder = args.run.resolve()
    report = read_json(folder/'report.json')
    masks = None
    if args.mask:
        masks = [np.asarray(Image.open(p).convert('L')) > 127 for p in args.mask]
    result, *_ = analyse_folder(folder, report, args.threshold or report['threshold_8bit'], masks)
    print(json.dumps(result if args.verbose else {k: v for k, v in result.items() if k != 'per_frame'}, indent=2))
    if args.save:
        write_json(args.save, dict(run=str(folder), masks=[str(p) for p in args.mask or []], **result))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    run = commands.add_parser('capture')
    run.add_argument('--setup', type=Path, required=True)
    run.add_argument('--output', type=Path, required=True, help='New directory')
    run.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    run.add_argument('--divisors', type=int, nargs='+', default=[4], choices=(1, 2, 4))
    run.add_argument('--cameras', nargs='+', default=['moving', 'static'], choices=('moving', 'static'))
    run.add_argument('--restir', choices=('setup', 'on', 'off'), default='setup')
    run.add_argument('--warmup', type=int, default=128)
    run.add_argument('--frames', type=int, default=16, help='Captured frames after warmup')
    run.add_argument('--angle', type=float, default=0.5, help='Degrees between the two moving poses')
    run.add_argument('--threshold', type=int, default=48)
    run.add_argument('--window', type=int, nargs=2, default=[1280, 720])
    run.add_argument('--keep-captures', action='store_true')
    run.add_argument('--views', nargs='*', default=[], choices=tuple(DEBUG_OUTPUTS),
                     help='GI debug outputs to render along the same path and join with the sparks')
    average = commands.add_parser('accumulate')
    average.add_argument('--setup', type=Path, required=True)
    average.add_argument('--output', type=Path, required=True, help='New directory')
    average.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    average.add_argument('--divisor', type=int, default=4, choices=(1, 2, 4))
    average.add_argument('--restir', choices=('setup', 'on', 'off'), default='setup')
    average.add_argument('--warmup', type=int, default=128)
    average.add_argument('--frames', type=int, default=256)
    average.add_argument('--window', type=int, nargs=2, default=[1280, 720])
    average.add_argument('--camera-from', type=Path, help='Take the camera pose from another .zenrender')
    average.add_argument('--reference', type=Path, help='An accumulate output whose mean and mask to compare against')
    again = commands.add_parser('analyse')
    again.add_argument('run', type=Path, help='A run directory captured with --keep-captures')
    again.add_argument('--mask', type=Path, nargs='+', help='Per-pose masks, pose 0 (even frames) first')
    again.add_argument('--threshold', type=int, default=0)
    again.add_argument('--save', type=Path)
    again.add_argument('--verbose', action='store_true')
    args = parser.parse_args()
    if args.command in ('capture', 'accumulate'):
        args.backend, args.gather, args.lights = 'setup', 'setup', 'all'
    if args.command == 'capture':
        if args.warmup < 2 or args.frames < 5 or not 0 < args.threshold < 255 or not np.isfinite(args.angle):
            parser.error('Need warmup >= 2, frames >= 5, threshold in (0, 255) and a finite angle')
        if not all(1 <= v <= 8192 for v in args.window):
            parser.error('Invalid window extent')
    globals()[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
