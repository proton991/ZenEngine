"""Noise and frame rate of one scene in three cases, one ZenApp launch each.

Cases: static camera with static lights, a moving camera with static lights, and the same motion with
the camera light ball on. Each launch renders 3840 x 2160 fullscreen on hardware RT.

Frame rate is the median wall time per frame from ZenApp --profile over 600 frames without captures.
Noise compares captured frames with a settled image of the same pose, in display levels of 255:
the static case uses the mean of its other captures, and the moving cases use the mean of eight
frames held at that pose later in the same launch. Absolute levels understate noise in a dark scene,
so the report also counts pixels whose error exceeds 10% of their settled value (and 2 levels). The motion is a +-30 degree yaw at one degree
per frame around the setup camera.

Use build/venv/Scripts/python.exe from tools/. Data/engine.cfg is never edited.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image

import gi_quick_gates as gates
from gi_spark_metric import rotate_target
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from profile_hybrid_gi import running_renderers

WARMUP = 600
TIMING = 600
AMPLITUDE = 30
PERIOD = 4 * AMPLITUDE
SCORED = (10, 25, 45, 70, 95, 110)
HOLD = 384
SETTLED = 8
STATIC_CAPTURES = 32
EXTRA = ('--backend=hardware', '--async-compute=0')


def yaw(offset):
    phase = offset % PERIOD
    angle = phase if phase <= AMPLITUDE else 2 * AMPLITUDE - phase if phase <= 3 * AMPLITUDE else phase - PERIOD
    return float(angle)


def pose(setup, angle):
    eye, target = gates.base_pose(setup)
    return eye, rotate_target(eye, target, angle)


def static_timeline(setup):
    first = WARMUP + TIMING + 1
    captures = list(range(first, first + STATIC_CAPTURES))
    last = captures[-1]
    keys = [gates.key(1, pose(setup, 0.0)), gates.key(last, pose(setup, 0.0))]
    layout = dict(timing=[WARMUP + 1, WARMUP + TIMING], scored={str(f): captures for f in captures})
    return dict(keys=keys), captures, layout, last


def moving_timeline(setup):
    keys = [gates.key(1, pose(setup, 0.0)), gates.key(WARMUP, pose(setup, 0.0))]
    end = WARMUP + TIMING + PERIOD
    keys += [gates.key(frame, pose(setup, yaw(frame - WARMUP))) for frame in range(WARMUP + 1, end + 1)]
    cycle = WARMUP + TIMING
    scored, captures = {}, []
    for index, offset in enumerate(SCORED):
        start = end + 1 + index * HOLD
        angle = yaw(offset)
        keys += [gates.key(start, pose(setup, angle)), gates.key(start + HOLD - 1, pose(setup, angle))]
        settled = list(range(start + HOLD - SETTLED, start + HOLD))
        scored[str(cycle + offset)] = settled
        captures += [cycle + offset, *settled]
    last = end + len(SCORED) * HOLD
    layout = dict(timing=[WARMUP + 1, WARMUP + TIMING], scored=scored)
    return dict(keys=keys), sorted(captures), layout, last


def load(folder, frame):
    path = gates.frame_path(folder, frame)
    state = read_json(str(path) + '.json')
    expected = dict(sceneFrames=frame, backend='hardware', gather='screen_probes', width=3840, height=2160,
                    framebufferWidth=3840, framebufferHeight=2160, giWidth=3840, giHeight=2160,
                    fullscreen=True, asyncCompute='disabled by configuration', tierFallback='')
    wrong = {k: state.get(k) for k, v in expected.items() if state.get(k) != v}
    if wrong:
        raise ValueError(f'{path}: unexpected state {wrong}')
    with Image.open(path) as image:
        return np.asarray(image.convert('RGB'), np.float32), state


def noise(image, reference):
    delta = image - reference
    magnitude = np.abs(delta).max(axis=2)
    relative = np.abs(delta.mean(axis=2)) > np.maximum(2.0, .1 * reference.mean(axis=2))
    return dict(rms=float(np.sqrt(np.mean(np.square(delta, dtype=np.float64)))),
                above4=float(100 * np.mean(magnitude > 4)), above16=float(100 * np.mean(magnitude > 16)),
                relative10=float(100 * np.mean(relative)))


def frame_times(profile, first, last):
    times = {}
    for line in Path(profile).read_text(errors='replace').splitlines():
        if line.startswith('scene_frame='):
            fields = dict(item.split('=', 1) for item in line.split() if '=' in item)
            times[int(fields['scene_frame'])] = float(fields['frame_cpu_ms'])
    window = [times[f] for f in range(first, last + 1) if f in times]
    if len(window) < (last - first + 1) * .95:
        raise ValueError(f'{profile}: only {len(window)} timed frames in {first}-{last}')
    window.sort()
    median = statistics.median(window)
    return dict(frames=len(window), median_ms=median, p95_ms=window[int(.95 * (len(window) - 1))], fps=1000.0 / median)


def run_case(args, name, setup, moving):
    folder = args.output / name
    camera, captures, layout, last = moving_timeline(setup) if moving else static_timeline(setup)
    profile = folder / 'profile'
    if not args.rescore:
        command = gates.launch(args, folder, setup, camera, None, captures, last, extra=(*EXTRA, f'--profile={profile}'))
        identity = provenance(args, folder / 'setup.zenrender', [command])
        identity.update(benchmark_sha256=digest(__file__), camera_path_sha256=digest(folder / 'camera-path.json'))
        write_json(folder / 'provenance.json', identity)
    timing = frame_times(f'{profile}.rdg.log', *layout['timing'])
    images, states = {}, {}
    for frame in captures:
        images[frame], states[frame] = load(folder, frame)
    rows = []
    for frame, settled in layout['scored'].items():
        others = [f for f in settled if f != int(frame)]
        reference = sum(images[f] for f in others) / len(others)
        rows.append(dict(frame=int(frame), **noise(images[int(frame)], reference)))
    states = list(states.values())
    summary = {k: float(np.mean([r[k] for r in rows])) for k in ('rms', 'above4', 'above16', 'relative10')}
    flags = {k: states[0].get(k) for k in ('localHistoryResponse', 'movingLightHistory', 'worldCacheCapacity')}
    return dict(timing=timing, noise=summary, frames=rows, state=flags)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--setup', type=Path, required=True, help='Scene setup with static lights and the ball off')
    parser.add_argument('--output', type=Path, required=True, help='New directory')
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--set', action='append', default=[], metavar='KEY=JSON', help='Setup override for every case')
    parser.add_argument('--rescore', action='store_true', help='Score the existing captures in --output again')
    args = parser.parse_args()
    if running_renderers() and not args.rescore:
        raise RuntimeError('Another renderer is running')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=args.rescore)
    args.validation = 0
    base = read_json(args.setup)
    for item in args.set:
        path, value = item.split('=', 1)
        node = base
        for key in path.split('.')[:-1]:
            node = node[key]
        node[path.split('.')[-1]] = json.loads(value)
    ball = json.loads(json.dumps(base))
    base['rendering']['cameraLight']['enabled'] = False
    ball['rendering']['cameraLight']['enabled'] = True
    cases = [('static', base, False), ('moving', base, True), ('moving-ball', ball, True)]
    report = {name: run_case(args, name, setup, moving) for name, setup, moving in cases}
    write_json(args.output / 'report.json', dict(setup=str(args.setup), overrides=args.set, warmup=WARMUP,
                                                timing_frames=TIMING, amplitude_degrees=AMPLITUDE, scored=SCORED,
                                                hold=HOLD, cases=report))
    print(f'{"case":12s} {"FPS":>6s} {"ms":>7s} {"p95 ms":>7s} {"RMS":>6s} {">4 lv %":>8s} {">16 lv %":>9s} '
          f'{">10% off %":>11s}')
    for name, row in report.items():
        t, n = row['timing'], row['noise']
        print(f"{name:12s} {t['fps']:6.1f} {t['median_ms']:7.2f} {t['p95_ms']:7.2f} {n['rms']:6.3f} "
              f"{n['above4']:8.3f} {n['above16']:9.4f} {n['relative10']:11.3f}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
