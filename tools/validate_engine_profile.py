"""Validate portable scene_renderer_demo profile exports and recompute their summaries."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path
import statistics


STATUSES = {'available', 'disabled', 'pending', 'unsupported', 'dropped', 'discarded', 'error'}

PHASES = ('cold', 'warmup', 'measured')


def statistics_for(values):
    values = sorted(values)
    if not values:
        return dict(count=0, min=None, max=None, mean=None, median=None, p95=None)
    return dict(count=len(values), min=values[0], max=values[-1],
                mean=sum(values) / len(values), median=statistics.median(values),
                p95=values[(len(values) * 95 + 99) // 100 - 1])


def check_statistics(actual, values):
    expected = statistics_for(values)
    assert actual.keys() == expected.keys(), (actual, expected)
    for key, value in expected.items():
        if value is None:
            assert actual[key] is None, (key, actual[key], value)
        elif key == 'count':
            assert actual[key] == value, (key, actual[key], value)
        else:
            assert math.isfinite(actual[key]) and math.isclose(actual[key], value, rel_tol=1e-10, abs_tol=1e-10), (key, actual[key], value)


def finite_nonnegative(text):
    value = float(text)
    assert math.isfinite(value) and value >= 0, text
    return value


def validate_gpu_frames(manifest, frames, require_frame_gpu):
    if manifest['schema_version'] == 1:
        assert not require_frame_gpu, 'Schema v1 does not contain GPU frame measurements'
        return {}

    assert isinstance(manifest['gpu_frame_scope'], str) and manifest['gpu_frame_scope']
    expected_frames = manifest['requested_frames'] + manifest['requested_warmup_frames']
    assert expected_frames > 0 and len(frames) == expected_frames, 'Incomplete application frames'
    gpu_frames = collections.defaultdict(list)
    unavailable = collections.Counter()
    statuses = collections.Counter()
    previous = None
    for position, frame in enumerate(frames):
        index = int(frame['frame_index'])
        assert index >= 0 and (previous is None or index == previous + 1), 'Missing or reordered application frame'
        previous = index
        warmup = position < manifest['requested_warmup_frames']
        local = position if warmup else position - manifest['requested_warmup_frames']
        assert int(frame['phase_frame']) == local, 'Stale application frame identity'
        phase = 'warmup' if warmup else 'measured'
        gi_start = manifest['gi_start_frame']
        if not warmup and gi_start:
            phase = 'warmup' if local < gi_start else 'cold' if local == gi_start else 'measured'
        elif position == 0:
            phase = 'cold'
        assert frame['phase'] == phase, 'Stale application frame phase'
        assert frame['succeeded'] == 'true', 'Failed application frame'
        status = frame['gpu_status']
        assert status in ('available', 'unsupported'), ('Incomplete GPU frame measurement', status)
        statuses[status] += 1
        intervals = int(frame['gpu_intervals'])
        excluded = int(frame['gpu_excluded_intervals'])
        assert intervals >= 0 and excluded >= 0, 'Invalid GPU frame interval counts'
        if status == 'available':
            assert intervals > 0, 'Available GPU frame has no native intervals'
            assert frame['gpu_frame_ms'], 'Available GPU frame missing duration'
            gpu_frames[phase].append(finite_nonnegative(frame['gpu_frame_ms']))
        else:
            assert frame['gpu_frame_ms'] == '', 'Unavailable GPU frame must be empty, not zero'
            unavailable[phase] += 1

    assert set(manifest['gpu_frame_ms']) == set(PHASES)
    assert set(manifest['gpu_frame_unavailable']) == set(PHASES)
    for phase in PHASES:
        check_statistics(manifest['gpu_frame_ms'][phase], gpu_frames[phase])
        assert manifest['gpu_frame_unavailable'][phase] == unavailable[phase], ('Stale GPU frame unavailable count', phase)
    if require_frame_gpu:
        assert statuses['available'] == expected_frames, 'Not all application frames have GPU measurements'
    return dict(statuses)


def validate_frame_intervals(manifest, frames):
    if manifest['schema_version'] >= 4:
        previous_end = 0.0
        for frame in frames:
            start = finite_nonnegative(frame['frame_start_ms'])
            end = finite_nonnegative(frame['frame_end_ms'])
            assert previous_end <= start <= end, 'Overlapping or reordered frame wall intervals'
            previous_end = end
            assert frame['gi_method'] in ('cone', 'hybrid', 'none')


def validate(prefix, require_gpu=False, require_frame_gpu=False):
    prefix = str(prefix)
    manifest = json.loads(Path(prefix + '.profile.json').read_text(encoding='utf-8'))
    with Path(prefix + '.frames.csv').open(encoding='utf-8', newline='') as source:
        frames = list(csv.DictReader(source))
    with Path(prefix + '.passes.csv').open(encoding='utf-8', newline='') as source:
        passes = list(csv.DictReader(source))
    assert manifest['schema_version'] in (1, 2, 3, 4), 'Unsupported profile schema'
    assert manifest['run_succeeded'] and manifest['input_files_unchanged']
    assert len(frames) == manifest['frame_count'] and len(passes) == manifest['pass_count']
    assert not any(manifest['dropped'].values()), manifest['dropped']
    assert manifest['instrumented']
    by_frame = {}
    cpu_frames = collections.defaultdict(list)
    for frame in frames:
        assert frame['run_id'] == manifest['run_id'], 'Mixed capture runs'
        index = int(frame['frame_index'])
        assert index not in by_frame, ('Duplicate application frame', index)
        by_frame[index] = frame
        assert frame['phase'] in PHASES
        duration = finite_nonnegative(frame['cpu_frame_ms'])
        if frame['succeeded'] == 'true':
            cpu_frames[frame['phase']].append(duration)
    for phase in PHASES:
        check_statistics(manifest['cpu_frame_ms'][phase], cpu_frames[phase])
    frame_statuses = validate_gpu_frames(manifest, frames, require_frame_gpu)
    validate_frame_intervals(manifest, frames)
    graphs = {entry['record']: entry for entry in manifest['graphs']}
    assert len(graphs) == len(manifest['graphs'])
    for graph in graphs.values():
        assert graph['omitted_nodes'] == 0 and graph['omitted_submissions'] == 0, ('Truncated graph', graph)
    graph_passes = collections.Counter()
    node_ids = set()
    samples = collections.defaultdict(lambda: dict(cpu=[], gpu=[], unavailable=0))
    statuses = collections.Counter()
    for row in passes:
        assert row['run_id'] == manifest['run_id'], 'Mixed capture runs'
        graph = graphs[int(row['graph_record'])]
        identity = (int(row['graph_record']), int(row['node_id']))
        assert identity not in node_ids, ('Duplicate pass', identity)
        node_ids.add(identity)
        graph_passes[identity[0]] += 1
        frame_index = int(row['frame_index']) if row['frame_index'] else None
        assert graph['frame_index'] == frame_index
        assert (graph['graph'], graph['execution'], graph['phase']) == (row['graph'], int(row['execution']), row['phase'])
        assert row['phase'] == ('startup' if frame_index is None else by_frame[frame_index]['phase'])
        assert row['gpu_status'] in STATUSES and row['gpu_status'] != 'pending', row
        statuses[row['gpu_status']] += 1
        equivalent = int(row['queue_equivalence']) if row['queue_equivalence'] else None
        key = (row['phase'], row['graph'], row['pass'], row['queue'], equivalent)
        sample = samples[key]
        if row['cpu_record_us']:
            sample['cpu'].append(finite_nonnegative(row['cpu_record_us']))
        if row['gpu_status'] == 'available':
            assert row['gpu_us'], 'Available result missing GPU duration'
            sample['gpu'].append(finite_nonnegative(row['gpu_us']))
        else:
            assert row['gpu_us'] == '', 'Unavailable GPU measurement must be empty, not zero'
            sample['unavailable'] += 1
    assert len(samples) == len(manifest['pass_statistics'])
    for entry in manifest['pass_statistics']:
        key = (entry['phase'], entry['graph'], entry['pass'], entry['queue'], entry['queue_equivalence'])
        sample = samples.pop(key)
        check_statistics(entry['cpu_record_us'], sample['cpu'])
        check_statistics(entry['gpu_us'], sample['gpu'])
        assert entry['gpu_unavailable'] == sample['unavailable']
    assert not samples
    for record, graph in graphs.items():
        assert graph_passes[record] == graph['node_count'], ('Missing pass rows', record)
    assert not any(statuses[status] for status in ('dropped', 'discarded', 'error')), statuses
    if require_gpu:
        assert statuses['available'] > 0, 'No available GPU measurements'
    fingerprint = manifest['inputs']['configuration']
    if fingerprint['readable']:
        config = Path(prefix + '.config.cfg').read_bytes()
        hash_value = 14695981039346656037
        for byte in config:
            hash_value = ((hash_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
        assert fingerprint['bytes'] == len(config)
        assert fingerprint['fnv1a64'] == f'{hash_value:016x}'
    return dict(run_id=manifest['run_id'], frames=len(frames), graphs=len(graphs),
                passes=len(passes), gpu_statuses=dict(statuses),
                gpu_frame_statuses=frame_statuses, verified=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--require-gpu', action='store_true')
    parser.add_argument('--require-frame-gpu', action='store_true',
                        help='Require a valid GPU duration for every application frame (schema v2)')
    args = parser.parse_args()
    print(json.dumps(validate(args.prefix, args.require_gpu, args.require_frame_gpu), indent=2))


if __name__ == '__main__':
    main()
