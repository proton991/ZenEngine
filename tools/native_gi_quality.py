"""Native 4K motion, recovery, flicker and spark gates using canonical ZenApp.

Each transport mode has its own settled images: multi-bounce lighting must not be scored as
motion error against a one-bounce image. Static transport accuracy belongs to the CUDA gate.
No executable copies or engine.cfg edits. Keep captures and input/build hashes for review.
"""
import argparse
from pathlib import Path
import time

import numpy as np
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from gi_noise_probe import preset
from gi_matched_quality import metrics, image
from profile_hybrid_gi import running_renderers
import gi_quick_gates as gates


def capture_states(folder, frames):
    return {frame: read_json(str(gates.frame_path(folder, frame)) + '.json') for frame in frames}


def check_measurement_states(moving, reference, pairs, uses_cache):
    if uses_cache:
        capacities = {state['worldCacheCapacity'] for launch in (moving, reference) for state in launch.values()}
        if len(capacities) != 1 or 0 in capacities:
            raise ValueError(f'Moving and held native measurements require one cache capacity, got {sorted(capacities)}')
    for observed, held in pairs:
        if moving[observed]['projectionViewBits'] != reference[held]['projectionViewBits']:
            raise ValueError(f'Moving frame {observed} and held frame {held} have different camera poses')


def measure(args, name, setup):
    folder = args.output / name
    folder.mkdir(parents=True, exist_ok=True)
    lights = gates.light_positions(setup, args.light_id, args.light_distance)
    poses = gates.GATE_POSES
    # A different starting view can select a different cache capacity before either launch is measured.
    initial_hold = args.warmup if name != 'retained' else 0
    camera, light_keys, captures, layout, last = gates.reference_timeline(setup, lights, args.hold, poses, initial_hold)
    # Restore the original light and pose for the static settled/noise pair.
    light_keys.append(dict(frame=last + 1, position=lights[0]))
    camera['keys'].append(gates.key(last + 1, gates.base_pose(setup)))
    center = last + args.hold
    captures += [center - 16, center]
    reference_folder = folder / 'reference'
    commands = [gates.launch(args, reference_folder, setup, camera, light_keys, captures, center)]
    reference_states = capture_states(reference_folder, captures)
    recovery = (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024) if name == 'cache' else (1, 2, 4, 8, 16, 32, 64, 128)
    camera, light_keys, captures, layout_motion, last = gates.quality_timeline(
        setup, lights, static_end=args.warmup, recovery=recovery, poses=poses)
    moving_folder = folder / 'moving'
    commands.append(gates.launch(args, moving_folder, setup, camera, light_keys, captures, last))
    moving_states = capture_states(moving_folder, captures)
    pairs = [(frame, layout[kind][step]) for kind, samples in layout_motion['motion'].items()
             for step, frame in samples.items()]
    pairs.extend((frame, layout['light']) for frame in layout_motion['light'].values())
    pairs.extend((frame, center) for frame in layout_motion['flicker'])
    check_measurement_states(moving_states, reference_states, pairs, name != 'retained')
    mean = np.zeros((2160, 3840), np.float64)
    m2 = np.zeros_like(mean)
    for count, frame in enumerate(layout_motion['flicker'], 1):
        value = gates.load(moving_folder, setup, frame).max(axis=2)
        delta = value - mean
        mean += delta / count
        m2 += delta * (value - mean)
    std = np.sqrt(m2 / len(layout_motion['flicker']))
    result = dict(mean_std=float(std.mean()), p999_std=float(np.percentile(std, 99.9)))
    reference = gates.load(reference_folder, setup, center)
    prior = gates.load(reference_folder, setup, center - 16)
    result['settled_residual'] = metrics(prior, reference)
    result['static_convergence'] = metrics(gates.load(moving_folder, setup, layout_motion['flicker'][-1]), reference)
    result['sparks'] = {kind: gates.sparks(moving_folder, setup, layout_motion[key])
                        for kind, key in (('static', 'static_sparks'), ('moving', 'moving_sparks'))}
    result['motion'] = {}
    for kind, rows in layout_motion['motion'].items():
        result['motion'][kind] = [dict(pose=step, **metrics(gates.load(moving_folder, setup, frame),
            gates.load(reference_folder, setup, layout[kind][step]))) for step, frame in rows.items()]
    reference = gates.load(reference_folder, setup, layout['light'])
    result['light'] = [dict(recovery_frame=delay, **metrics(gates.load(moving_folder, setup, frame), reference))
                       for delay, frame in layout_motion['light'].items()]
    state = moving_states[last]
    report = dict(result=result, state=state, commands=commands,
                  provenance=provenance(args, moving_folder/'setup.zenrender', commands),
                  own_reference=True, warmup=args.warmup, hold=args.hold, reference_initial_hold=initial_hold,
                  recovery_frames=list(recovery))
    write_json(folder/'report.json', report)
    return result


def compare(results):
    rows = {}
    if 'retained' in results and 'native' in results:
        old, new = results['retained'], results['native']
        for metric in ('mean_std', 'p999_std'):
            rows[metric] = dict(baseline=old[metric], candidate=new[metric], passed=new[metric] <= old[metric])
        baseline, candidate = (result['static_convergence']['rms_8bit'] for result in (old, new))
        rows['static/cross-launch'] = dict(baseline=baseline, candidate=candidate, passed=candidate <= baseline)
        baseline, candidate = (next(frame['rms_8bit'] for frame in result['light'] if frame['recovery_frame'] == 128)
                               for result in (old, new))
        rows['light/128'] = dict(baseline=baseline, candidate=candidate, passed=candidate <= baseline)
        for kind in gates.MOTIONS:
            for baseline, candidate in zip(old['motion'][kind], new['motion'][kind]):
                metric = 'above_4_percent'
                rows[f'{kind}/{candidate["pose"]}'] = dict(baseline=baseline[metric], candidate=candidate[metric],
                                                          passed=candidate[metric] <= baseline[metric])
        for kind in ('static', 'moving'):
            metric = 'spark_pixels_per_frame'
            baseline, candidate = old['sparks'][kind][metric], new['sparks'][kind][metric]
            rows[f'sparks/{kind}'] = dict(baseline=baseline, candidate=candidate, passed=candidate <= baseline)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--modes', nargs='+', choices=('retained', 'native', 'cache'), default=['retained', 'native'])
    parser.add_argument('--light-id', type=int, default=3)
    parser.add_argument('--light-distance', type=float, default=.04)
    parser.add_argument('--warmup', type=int, default=512)
    parser.add_argument('--hold', type=int, default=512)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--reflections-only', action='store_true',
                        help='Isolate reflections with matched one-bounce hit lighting and exact direct shadows')
    parser.add_argument('--baseline', type=Path, help='Compare with a frozen retained-mode report using identical inputs')
    args = parser.parse_args()
    if args.reflections_only and 'cache' in args.modes:
        parser.error('--reflections-only supports retained and native rendering, not the cache diagnostic')
    if running_renderers():
        parser.error('Close other Zen renderers before measuring')
    args.output, args.setup = args.output.resolve(), args.setup.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = read_json(args.setup)
    identity = provenance(args, args.setup, [])
    started = time.perf_counter()
    results = {}
    baseline = read_json(args.baseline) if args.baseline else None
    if baseline:
        if baseline.get('reflections_only', False) != args.reflections_only:
            raise ValueError('Frozen baseline measured a different lighting signal')
        for key in ('inputs', 'setup_sha256'):
            if identity[key] != baseline['provenance'][key]:
                raise ValueError(f'Frozen baseline differs: {key}')
        if 'retained' in args.modes or 'retained' not in baseline['results']:
            raise ValueError('Use --modes native with a baseline containing retained results')
        recorded = read_json(args.baseline.parent / 'retained' / 'report.json')
        expected_async = 'available' if source['rendering']['gi']['asyncCompute'] else 'disabled by configuration'
        if recorded['state']['asyncCompute'] != expected_async:
            raise ValueError('Frozen baseline used a different async-compute configuration')
        if recorded['warmup'] != args.warmup or recorded['hold'] != args.hold:
            raise ValueError('Frozen baseline used different warm-up or reference hold lengths')
        results['retained'] = baseline['results']['retained']
    for name in args.modes:
        setup = preset(source, 'hardware')
        # The general noise preset enables async compute. Qualification must retain
        # the explicit setup policy, including when comparing a frozen baseline.
        setup['rendering']['gi']['asyncCompute'] = source['rendering']['gi']['asyncCompute']
        setup['rendering']['gi']['cone'].update(screenProbeGI=name == 'native', worldRadianceCache=name != 'retained',
                                               worldCacheFeedback=True, nativeProbeBenchmark=False, radianceCache=False)
        if args.reflections_only:
            setup['rendering']['gi']['cone'].update(indirectDiffuse=False, specularOcclusion=True, reflections=2,
                worldCacheFeedback=False, nativeShadowReuse=False, environmentLighting=False)
            setup['rendering']['environment']['lighting'] = False
        if name == 'cache':
            # Phase 1 measures the cache itself, before probe/pixel reconstruction.
            setup['rendering']['debug'].update(output=14, face=0)
            setup['rendering']['gi']['cone']['pixelResolutionDivisor'] = 2
        print(name, flush=True)
        results[name] = measure(args, name, setup)
    final = provenance(args, args.setup, [])
    for key in ('executable_sha256', 'shaders_sha256', 'inputs', 'setup_sha256'):
        if final[key] != identity[key]:
            raise ValueError(f'Measurement input changed: {key}')
    report = dict(provenance=identity, tool_sha256=digest(__file__), results=results, gates=compare(results),
                  reflections_only=args.reflections_only,
                  frozen_baseline=str(args.baseline.resolve()) if args.baseline else None,
                  frozen_baseline_sha256=digest(args.baseline) if args.baseline else None,
                  seconds=time.perf_counter()-started)
    write_json(args.output/'report.json', report)
    print(report['gates'], flush=True)


if __name__ == '__main__':
    main()
