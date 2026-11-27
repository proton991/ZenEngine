"""Measure a deterministic ZenApp scene edit against held scene and camera poses at native 4K.

Supply a --scene-path accepted by ZenApp. Score frames starting after the first key.
For a moving camera, supply explicit camera keys at every scored frame. --hold defaults
to 512 frames per unique reference pose. Each edited pose also gets a camera-matched
control at the first scene key. Animation uses target=animation and absolute seconds.
"""
import argparse
from copy import copy, deepcopy
from statistics import median
from pathlib import Path
import json

import numpy as np

import gi_quick_gates as gates
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from gi_turn_replay import age_errors, capture, load_image, projection_view
from profile_hybrid_gi import running_renderers


def held(scene, frame):
    return {k: v for k, v in next((key for key in reversed(scene['keys']) if key['frame'] <= frame),
                                   scene['keys'][0]).items() if k != 'frame'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--scene-path', type=Path, required=True)
    parser.add_argument('--camera-path', type=Path)
    parser.add_argument('--control-scene-path', type=Path, help='Explicit no-edit or no-light comparison')
    parser.add_argument('--temporal-start', type=int, help='Measure temporal deviation over 32 fixed-pose frames from here')
    parser.add_argument('--cycle', action='append', default=[], metavar='FIRST:LAST',
                        help='Measure mean HDR energy on a common affected mask across each fixed-camera motion cycle')
    parser.add_argument('--rescore', action='store_true', help='Verify and score the stored captures without launching ZenApp')
    parser.add_argument('--frames', required=True, help='Comma-separated scene frame numbers')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--linear', action='store_true', help='Measure affected-region mean from pre-display HDR luminance')
    parser.add_argument('--baseline', type=Path, help='Score a captured global replay on the same reference and mask')
    parser.add_argument('--capture-only', action='store_true',
                        help='Capture only the trajectory and control used as input to another replay with --baseline')
    parser.add_argument('--hold', type=int, default=512)
    parser.add_argument('--phase', type=int, choices=range(64), default=0)
    parser.add_argument('--cache-cells', type=int, default=0)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--compare-setup', type=Path, help='Run three interleaved baseline/candidate pairs and score shared references')
    args = parser.parse_args()
    if args.capture_only and (args.compare_setup or args.baseline or args.cycle or args.temporal_start is not None):
        parser.error('--capture-only records the frame list without scoring or paired comparisons')
    if args.compare_setup:
        if args.baseline:
            parser.error('--compare-setup and --baseline select different comparison inputs')
        compare(args)
    else:
        replay(args, baseline_only=args.capture_only)


def comparison_settings(setup):
    settings = deepcopy(setup)
    cone = settings['rendering']['gi']['cone']
    for variant in ('localHistoryResponse', 'worldCacheSeed', 'probeSamplingPattern', 'glossySurfaceHistory', 'roughFootprintHistory'):
        cone.pop(variant, None)
    return settings


def gate_checks(row, scene):
    checks = {}
    for field, limit in (('unaffected_rms_ratio', 1.10), ('affected_rms_ratio', .5),
                         ('unaffected_temporal_ratio', 1.10)):
        if field + '_within_limit' in row:
            checks[field] = row[field + '_within_limit']
        elif row.get(field) is not None:
            checks[field] = row[field] <= limit
    checks['over24_pct'] = row['over24_pct'] < .1
    for threshold in (4, 24):
        metric = f'unaffected_over{threshold}_pct'
        if row.get(metric + '_within_control') is not None:
            checks[metric] = row[metric + '_within_control']
    keys = [key['frame'] for key in scene['keys'][1:] if key['frame'] <= row['frame']]
    if keys and row.get('affected_mean_energy_ratio') is not None:
        observation = row['frame'] - keys[-1] + 1
        limit = .1 if observation == 8 else .02 if observation == 32 else .01 if observation >= 128 else None
        if limit is not None:
            checks['energy_recovery'] = abs(row['affected_mean_energy_ratio'] - 1) <= limit
    if keys and scene.get('target', 'light') == 'light' and row['frame'] - keys[-1] + 1 == 128:
        edit_index = next(i for i, key in enumerate(scene['keys']) if key['frame'] == keys[-1])
        old, new = scene['keys'][edit_index - 1:edit_index + 1]
        other = lambda key: {k: v for k, v in key.items() if k not in ('frame', 'position')}
        if old.get('position') != new.get('position') and other(old) == other(new) and 'full_frame_rms' in row:
            checks['light_move_rms_128'] = row['full_frame_rms'] <= .23653
    return checks


def compare(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=args.rescore)
    reports = []
    for repeat in range(3):
        baseline, candidate = copy(args), copy(args)
        baseline.setup = args.compare_setup
        baseline.output = root / f'pair-{repeat + 1}' / 'baseline'
        candidate.output = baseline.output.parent / 'candidate'
        # Reverse the order in the middle pair to expose drift between launches.
        if repeat == 1 and not args.rescore:
            replay(candidate)
            candidate.rescore = True
        replay(baseline, baseline_only=True)
        candidate.baseline = baseline.output
        replay(candidate)
        report = read_json(candidate.output / 'report.json')
        for field in ('executable_sha256', 'shaders_sha256'):
            if report['identity'][field] != report['baseline_identity'][field]:
                raise ValueError(f'{field} changed between the two launches')
        reports.append(report)
    identities = {(r['identity']['executable_sha256'], r['identity']['shaders_sha256']) for r in reports}
    if len(identities) != 1:
        raise ValueError('Executable or shaders changed between paired runs')
    rows = []
    scene = read_json(args.scene_path)
    for samples in zip(*(r['results'] for r in reports), strict=True):
        if len({s['frame'] for s in samples}) != 1:
            raise ValueError('Paired runs scored different frames')
        row = {'frame': samples[0]['frame']}
        for field in ('unaffected_rms_ratio', 'affected_rms_ratio', 'affected_mean_energy_ratio',
                      'affected_rms', 'full_frame_rms', 'over24_pct', 'unaffected_temporal_ratio'):
            if all(s.get(field) is not None for s in samples):
                row[field] = median(s[field] for s in samples)
        for key in ('unaffected_rms_ratio_within_limit', 'affected_rms_ratio_within_limit',
                    'unaffected_temporal_ratio_within_limit',
                    'unaffected_over4_pct_within_control', 'unaffected_over24_pct_within_control'):
            if all(key in s for s in samples):
                row[key] = sum(s[key] for s in samples) >= 2
        row['checks'] = gate_checks(row, scene)
        rows.append(row)
    write_json(root / 'report.json', dict(repetitions=3, statistic='median of paired ratios', scorer_sha256=digest(__file__),
               reports=[f'pair-{i}/candidate/report.json' for i in range(1, 4)], results=rows))


def capture_identity(args, setup_file, start, runs):
    commands = [r['command'] for r in runs]
    if args.rescore:
        if start['commands'] != commands:
            raise ValueError('Capture commands differ from the stored identity')
        return start
    end = provenance(args, setup_file, commands)
    if any(end[k] != start[k] for k in ('inputs', 'setup_sha256', 'shaders_sha256', 'executable_sha256')):
        raise ValueError('Measurement inputs changed during replay')
    write_json(args.output / 'identity.json', end)
    return end


def replay(args, baseline_only=False):
    args.executable = DEFAULT_EXE
    args.output = args.output.resolve()
    frames = sorted(set(int(f) for f in args.frames.split(',')))
    windows = [tuple(map(int, window.split(':'))) for window in args.cycle]
    if any(len(w) != 2 or w[0] < 1 or w[1] < w[0] for w in windows) or (windows and not args.linear):
        raise ValueError('Cycle scoring requires --linear and positive first:last frame ranges')
    frames = sorted(set(frames).union(*(range(first, last + 1) for first, last in windows)))
    if args.temporal_start is not None:
        frames = sorted(set(frames) | set(range(args.temporal_start, args.temporal_start + 32)))
    if frames[0] < 1 or args.hold < 32 or (not args.rescore and running_renderers()):
        raise ValueError('Need positive frames, a reference hold of at least 32, and an idle renderer')
    setup, scene = read_json(args.setup), read_json(args.scene_path)
    if setup['animationEnabled'] and scene.get('target') != 'animation':
        raise ValueError('An animated setup requires absolute animation poses for matched references')
    setup['rendering']['gi']['cone']['samplingPhase'] = args.phase
    camera = (read_json(args.camera_path) if args.camera_path else
              dict(keys=[gates.key(1, gates.base_pose(setup)), gates.key(max(2, frames[-1]), gates.base_pose(setup))]))
    if args.camera_path and not set(frames) <= {key['frame'] for key in camera['keys']}:
        raise ValueError('Provide the exact camera key at each scored frame')
    args.output.mkdir(parents=True, exist_ok=args.rescore)
    setup_file = args.output / 'setup.zenrender'
    if args.rescore:
        if read_json(setup_file) != setup:
            raise ValueError('Setup differs from the stored replay')
        identity = read_json(args.output / 'identity.json')
    else:
        write_json(setup_file, setup)
        identity = provenance(args, setup_file, [])
    runs = []
    control = read_json(args.control_scene_path) if args.control_scene_path else dict(scene, keys=[dict(held(scene, 1), frame=1)])
    for name, path in [('moving', scene), ('control', control)]:
        runs.append(capture(args, name, setup, camera, frames, path))
        args.cache_cells = runs[0]['cache_cells']
        cone = setup['rendering']['gi']['cone']
        requested = {key: cone.get(key, default) for key, default in
                     [('localHistoryResponse', True), ('probeSamplingPattern', 0), ('glossySurfaceHistory', False),
                      ('roughFootprintHistory', False)]}
        for f in frames:
            metadata = read_json(str(gates.frame_path(args.output / name, f)) + '.json')
            if not args.rescore or 'worldCacheSeed' in metadata:
                if metadata.get('worldCacheSeed') != cone.get('worldCacheSeed', False):
                    raise ValueError(f'{name}: active worldCacheSeed differs from setup at frame {f}')
            for key, value in requested.items():
                if metadata.get(key, False) != value:
                    raise ValueError(f'{name}: active {key} differs from setup at frame {f}')
    if baseline_only:
        capture_identity(args, setup_file, identity, runs)
        return
    references = {}
    samples = [1 + args.hold + 16 * i for i in range(8)]

    def reference(cam, path, frame):
        pose = dict(path, keys=[dict(held(path, frame), frame=1)])
        key = json.dumps([cam, pose], sort_keys=True)
        if key not in references:
            folder = f'held-{len(references)}'
            schedule = dict(keys=[dict(cam, frame=1), dict(cam, frame=samples[-1])])
            runs.append(capture(args, folder, setup, schedule, samples, pose))
            references[key] = folder
        return references[key]

    pairs = [(reference(held(camera, f), scene, f), reference(held(camera, f), control, f)) for f in frames]
    identity = capture_identity(args, setup_file, identity, runs)

    def mean(folder, group):
        total = load_image(args.output / folder, group[0]).astype(np.float32)
        for f in group[1:]:
            total += load_image(args.output / folder, f)
        return total / len(group)

    baseline_identity = None
    if args.baseline:
        baseline_identity = read_json(args.baseline / 'identity.json')
        def scene_inputs(record):
            # The requested setup identifies the intentional rendering variant. Its parsed settings
            # are checked below. All scene, lighting and cooked asset bytes must agree.
            return {p: checksum for p, checksum in record['inputs'].items() if Path(p).suffix.lower() != '.zenrender'}

        if scene_inputs(identity) != scene_inputs(baseline_identity):
            raise ValueError('Baseline and candidate used different scene or environment inputs')
        for name, path in [('moving', scene), ('control', control)]:
            if read_json(args.baseline / name / 'camera-path.json') != camera or read_json(args.baseline / name / 'scene-path.json') != path:
                raise ValueError('Baseline scene/camera inputs differ from this comparison')
            for f in frames:
                captured = read_json(str(gates.frame_path(args.baseline / name, f)) + '.json')
                if captured['worldCacheCapacity'] != args.cache_cells:
                    raise ValueError(f'World-cache capacity differs at {name}, frame {f}')
            if comparison_settings(read_json(args.baseline / name / 'setup.zenrender')) != comparison_settings(setup):
                raise ValueError('Baseline scene or non-denoiser settings differ')
    def luminance(folder, frame):
        path = str(gates.frame_path(args.output / folder, frame)) + '.luma.f32'
        values = np.fromfile(path, dtype='<f4').reshape(2160, 3840)
        if not np.isfinite(values).all():
            raise ValueError(f'Invalid linear luminance at {path}')
        return values

    cycle_frames = {frame for first, last in windows for frame in range(first, last + 1)}
    cycle_mask = None
    if windows:
        cameras = {json.dumps(held(camera, f), sort_keys=True) for f in cycle_frames}
        if len(cameras) != 1:
            raise ValueError('A motion-energy cycle must keep the camera fixed')
        cycle_mask = np.zeros((2160, 3840), dtype=bool)
        for target, before in {pair for f, pair in zip(frames, pairs) if f in cycle_frames}:
            if target != before:
                cycle_mask |= np.abs(mean(target, samples[::2]) - mean(before, samples[::2])).max(axis=2) >= 1
        if not cycle_mask.any():
            raise ValueError('The held poses contain no affected pixels for the motion cycles')
    cycle_energy = {}
    results = []
    previous_pair = None
    temporal = {}
    for frame, pair in zip(frames, pairs):
        target, before = pair
        matrix = projection_view(args.output / 'moving', frame)
        if (matrix != projection_view(args.output / 'control', frame) or
                any(matrix != projection_view(args.output / folder, f) for folder in set(pair) for f in samples)):
            raise ValueError(f'Reference pose mismatch at frame {frame}')
        if pair != previous_pair:
            # Separate both the warmup age and the estimates used for selecting and scoring pixels.
            stable = np.abs(mean(target, samples[::2]) - mean(before, samples[::2])).max(axis=2) < 1
            after, initial = mean(target, samples[1::2]), mean(before, samples[1::2])
            if args.linear:
                target_luminance = sum(luminance(target, f) for f in samples[1::2]) / len(samples[1::2])
            previous_pair = pair
        image = load_image(args.output / 'moving', frame).astype(np.float32)
        baseline = load_image(args.output / 'control', frame).astype(np.float32)
        error = image - after
        row = dict(frame=frame, unaffected_pixels=int(stable.sum()), full_frame_rms=float(np.sqrt(np.mean(error ** 2))),
                   over24_pct=float(100 * np.mean(np.abs(error).max(axis=2) > 24)))
        for name, mask, diff in [('affected', ~stable, error), ('unaffected', stable, error),
                                 ('control_unaffected', stable, baseline - initial)]:
            row[name + '_rms'] = float(np.sqrt(np.mean(diff[mask] ** 2))) if mask.any() else None
            for threshold in (4, 24):
                row[f'{name}_over{threshold}_pct'] = (float(100 * np.mean(np.max(np.abs(diff[mask]), axis=1) > threshold))
                                                      if mask.any() else None)
        if args.camera_path and stable.any():
            for threshold in (4, 24):
                metric = f'unaffected_over{threshold}_pct'
                # Compare counts directly when the camera-only control has zero outliers.
                row[metric + '_within_control'] = row[metric] <= 1.10 * row['control_' + metric]
        measured_luminance = luminance('moving', frame) if args.linear else None
        if frame in cycle_frames:
            cycle_energy[frame] = (measured_luminance[cycle_mask].sum(dtype=np.float64),
                                   target_luminance[cycle_mask].sum(dtype=np.float64))
        if args.linear and args.camera_path:
            ages = (args.baseline or args.output) / 'moving'
            if projection_view(ages, frame) != matrix:
                raise ValueError('Age-map camera differs from the scored pose')
            row['age_bands'] = age_errors(image, after, ages, frame)
            if args.baseline:
                row['baseline_age_bands'] = age_errors(load_image(ages, frame), after, ages, frame)
        if stable.any():
            row['unaffected_rms_ratio_within_limit'] = row['unaffected_rms'] <= 1.10 * row['control_unaffected_rms']
            if row['control_unaffected_rms'] > 0:
                row['unaffected_rms_ratio'] = row['unaffected_rms'] / row['control_unaffected_rms']
        if (~stable).any():
            if args.linear:
                total = float(target_luminance[~stable].sum(dtype=np.float64))
                row['affected_mean_energy_ratio'] = (float(measured_luminance[~stable].sum(dtype=np.float64)) / total
                                                     if total > 0 else None)
            if args.baseline:
                baseline_image = load_image(args.baseline / 'moving', frame)
                baseline_rms = float(np.sqrt(np.mean((baseline_image[~stable] - after[~stable]) ** 2)))
                row['global_affected_rms'] = baseline_rms
                row['affected_rms_ratio_within_limit'] = row['affected_rms'] <= .5 * baseline_rms
                row['affected_rms_ratio'] = row['affected_rms'] / baseline_rms if baseline_rms > 0 else None
            row['affected_mean_display_ratio'] = float(image[~stable].sum(dtype=np.float64) /
                                                       max(after[~stable].sum(dtype=np.float64), 1))
        if args.temporal_start is not None and args.temporal_start <= frame < args.temporal_start + 32:
            count = frame - args.temporal_start + 1
            if count == 1:
                temporal_pair = pair
            if pair != temporal_pair:
                raise ValueError('Temporal deviation requires a fixed camera and scene pose throughout the window')
            for name, values in [('moving', image), ('control', baseline)]:
                current = values[stable]
                if count == 1:
                    temporal[name] = [current.copy(), 0.]
                else:
                    running, variance = temporal[name]
                    delta = current - running
                    running += delta / count
                    temporal[name][1] = variance + float(np.sum(delta * (current - running), dtype=np.float64))
            if count == 32:
                row['unaffected_temporal_deviation'] = {name: float(np.sqrt(var / max(31 * running.size, 1)))
                                                        for name, (running, var) in temporal.items()}
                deviation = row['unaffected_temporal_deviation']
                if stable.any():
                    row['unaffected_temporal_ratio_within_limit'] = deviation['moving'] <= 1.10 * deviation['control']
                if deviation['control'] > 0:
                    row['unaffected_temporal_ratio'] = deviation['moving'] / deviation['control']
        row['checks'] = gate_checks(row, scene)
        results.append(row)
    cycles = []
    for first, last in windows:
        measured, reference = np.sum([cycle_energy[f] for f in range(first, last + 1)], axis=0)
        ratio = float(measured / reference) if reference > 0 else None
        cycles.append(dict(first=first, last=last, pixels=int(cycle_mask.sum()), mean_energy_ratio=ratio,
                           within_one_percent=abs(ratio - 1) <= .01 if ratio is not None else None))
    write_json(args.output / 'report.json', dict(cycles=cycles, identity=identity, baseline_identity=baseline_identity,
               scorer_sha256=digest(__file__), runs=runs, results=results,
               reference_folders=pairs, mask_frames=samples[::2], reference_frames=samples[1::2]))


if __name__ == '__main__':
    main()
