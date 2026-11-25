"""Compare independent sampling phases of one native_gi_fast_motion production replay."""
import argparse
from collections import defaultdict
from itertools import combinations
from pathlib import Path

import numpy as np

from gi_stability import read_json, write_json
from gi_spark_metric import pixel_components, settled_turn_metrics
from gi_turn_replay import load_image, projection_view


def regions(image, crops):
    yield 'frame', image
    for name, box in crops.items():
        x0, y0, x1, y1 = box
        if not (0 <= x0 < x1 <= image.shape[1] and 0 <= y0 < y1 <= image.shape[0]):
            raise ValueError(f'Invalid {name} crop {box}')
        yield name, image[y0:y1, x0:x1]


def pair_rms(a, b, frame, crops):
    if projection_view(a, frame) != projection_view(b, frame):
        raise ValueError(f'Camera mismatch between {a} and {b} at frame {frame}')
    delta = np.subtract(load_image(a, frame), load_image(b, frame), dtype=np.float32)
    np.square(delta, out=delta)
    return {name: float(np.sqrt(np.mean(square, dtype=np.float64))) for name, square in regions(delta, crops)}


def recurring_components(rows):
    """Find exact-pixel overlap across seeds at the same camera and scene frame."""
    phases_by_pixel = defaultdict(set)
    for phase, row in rows:
        for feature in row['features']:
            if 'spans' not in feature:
                raise ValueError('Rescore this capture to recover exact component pixels')
            for y, x0, x1 in feature['spans']:
                for x in range(x0, x1 + 1):
                    phases_by_pixel[y, x].add(phase)
    groups = []
    for group in pixel_components(p for p, phases in phases_by_pixel.items() if len(phases) >= 2):
        y, x = min((tuple(map(int, p)) for p in group), key=lambda p: (-len(phases_by_pixel[p]), p))
        groups.append(dict(x=x, y=y, pixels=len(group), phases=sorted(phases_by_pixel[y, x])))
    return sorted(groups, key=lambda g: (-len(g['phases']), -g['pixels'], g['y'], g['x']))


def phase_mean_metrics(folders, schedule, references):
    """Average independent display captures at identical poses to expose phase-locked errors."""
    scored = {}
    for angle, samples in references.items():
        held = sum(load_image(folder / 'reference', frame).astype(np.float32)
                   for folder in folders for frame in samples['mean']) / (len(folders) * len(samples['mean']))
        for frame, pose, *_ in schedule:
            if pose == angle:
                moving = sum(load_image(folder / 'moving', frame).astype(np.float32)
                             for folder in folders) / len(folders)
                scored[frame] = settled_turn_metrics(moving, held)
    return scored


def compare(folders, crops):
    first = read_json(folders[0] / 'report.json')
    if first.get('noise_gate', {}).get('metric') != 'high_error_pct':
        raise ValueError('Rescore the captures to compute the per-frame high-error pixel gate')
    gate_limit = first['noise_gate']['limit_pct']
    phase_peaks = {}
    identity = read_json(folders[0] / 'identity.json')
    setups, phases = [], []
    frames = [(r['frame'], str(r['angle']), r['visit'], r['recovery']) for r in first['moving']]
    floor = {angle: {} for angle in first['references']}
    moving = {frame: {} for frame, *_ in frames}
    envelope = {angle: dict(clusters=[], largest=[]) for angle in floor}
    observations = defaultdict(list)
    for folder in folders:
        run, build = read_json(folder / 'report.json'), read_json(folder / 'identity.json')
        setup = read_json(folder / 'moving/setup.zenrender')
        phase = setup['rendering']['gi']['cone'].pop('samplingPhase')
        if run['phase'] != phase or phase in phases:
            raise ValueError(f'Repeated phase or incorrect phase metadata in {folder}')
        if run.get('noise_gate', {}).get('metric') != 'high_error_pct' or run['noise_gate']['limit_pct'] != gate_limit:
            raise ValueError(f'Noise threshold differs in {folder}; rescore with the same limit')
        phase_peaks[phase] = run['noise_gate']['worst_pct']
        phases.append(phase)
        setups.append(setup)
        if (setup != setups[0] or any(build[k] != identity[k] for k in ('executable_sha256', 'shaders_sha256', 'inputs'))
                or any(run[k] != first[k] for k in ('cache_cells', 'references', 'degrees', 'kind'))
                or [s['camera_sha256'] for s in run['runs']] != [s['camera_sha256'] for s in first['runs']]
                or frames != [(r['frame'], str(r['angle']), r['visit'], r['recovery']) for r in run['moving']]):
            raise ValueError(f'Build, camera, scene or capture schedule differs in {folder}')
        for angle, rows in run['floor'].items():
            for row in rows:
                envelope[angle]['clusters'].append(row['clusters'])
                envelope[angle]['largest'].append(max((c['size'] for c in row['features']), default=0))
        for row in run['moving']:
            observations[row['frame']].append((phase, row))
    for a, b in combinations(folders, 2):
        for angle, ref in first['references'].items():
            for f in ref['floor']:
                for name, value in pair_rms(a / 'reference', b / 'reference', f, crops.get(angle, {})).items():
                    floor[angle].setdefault(name, []).append(value)
        for frame, angle, *_ in frames:
            for name, value in pair_rms(a / 'moving', b / 'moving', frame, crops.get(angle, {})).items():
                moving[frame].setdefault(name, []).append(value)
    limits = {angle: {name: float(np.percentile(values, 95)) + .10 for name, values in measures.items()}
              for angle, measures in floor.items()}
    failures = []
    for frame, angle, visit, offset in frames:
        for name, values in moving[frame].items():
            if max(values) > limits[angle][name]:
                failures.append(dict(frame=frame, angle=angle, visit=visit, recovery=offset, region=name,
                                     worst_rms=max(values), limit=limits[angle][name]))
    for folder, phase in zip(folders, phases):
        for row in read_json(folder / 'report.json')['moving']:
            env = envelope[str(row['angle'])]
            size = max((c['size'] for c in row['features']), default=0)
            if row['clusters'] > max(env['clusters']) or size > max(env['largest']):
                failures.append(dict(frame=row['frame'], phase=phase, clusters=row['clusters'], largest=size,
                                     max_settled_clusters=max(env['clusters']), max_settled_size=max(env['largest'])))
    sufficiently_sampled = len(phases) >= 5 and all(len(v['clusters']) >= 30 for v in envelope.values())
    ratio_pass = max(phase_peaks.values()) < gate_limit
    return dict(phases=phases, settled=envelope, paired_floor_rms=floor, paired_moving_rms=moving,
                recurring={frame: recurring_components(rows) for frame, rows in observations.items()},
                phase_mean=phase_mean_metrics(folders, frames, first['references']),
                rms_limits=limits, failures=failures, enough_phases=sufficiently_sampled,
                settled_envelope_match=not failures,
                noise_gate=dict(limit_pct=gate_limit, phase_peaks=phase_peaks, passed=ratio_pass),
                temporal_metrics_pass=sufficiently_sampled and ratio_pass)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', type=Path, nargs='+')
    parser.add_argument('--crops', type=Path, help='JSON mapping signed pose degrees to named [x0,y0,x1,y1] boxes')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if len(args.runs) < 2:
        parser.error('Supply at least two distinct phases')
    result = compare(args.runs, read_json(args.crops) if args.crops else {})
    write_json(args.output, result)
    print(f"Worst high-error area {max(result['noise_gate']['phase_peaks'].values()):.5f}% "
          f"across {len(result['phases'])} phases. {len(result['failures'])} comparisons exceed the settled envelope.")
    raise SystemExit(0 if result['temporal_metrics_pass'] else 1)
