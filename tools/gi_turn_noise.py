"""GI noise after a fast 180 degree turn: one ZenApp launch, 3840 x 2160 fullscreen, hardware RT.

After the start-up warm-up the camera settles at the setup pose (A), then makes three fast turns:
A to B (B = A + 180 degrees, first visit), B to A (return), and A to B again (revisit). Each turn sweeps
180 degrees over --turn-frames frames (1 is an instant cut) and lands on its pose, which the camera
then holds for --hold frames.

Every leg scores the first three frames after landing, which is the noise the eye sees, plus a few
later checkpoints that show when the image settles. Each frame is compared with the mean of the
last eight frames of the same hold. The eight frames before those give the settled noise floor,
which is also the still-camera check. Comparing the first visit with the revisit shows whether a
cold world cache or the new screen probes set the slow tail.

Noise counts pixels whose error exceeds 10% of their settled value and 2 display levels (of 255).
It also counts pixels more than 24 levels off, bright specks, and RGB RMS. A leg has settled at the
first scored frame from which every later one has at most 0.1% noisy pixels. Seconds use the median
frame time of the hold from ZenApp --profile.

Raw frames are deleted after scoring unless --keep-frames is given. Each leg keeps one error map of its
landing frame. Use build/venv/Scripts/python.exe from tools/. Data/engine.cfg is never edited.
"""
import argparse
import json
import statistics
import sys
from pathlib import Path

import numpy as np
from PIL import Image

import gi_quick_gates as gates
from gi_spark_metric import rotate_target, settled_turn_metrics
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from profile_hybrid_gi import running_renderers

SETTLE = 300
HOLD = 1024
TURN_FRAMES = 6
FIRST = (1, 2, 3)
CHECKPOINTS = (8, 16, 24, 32, 40, 44, 48, 64, 128, 256, 512)
SETTLED = 8
SETTLED_LIMIT = 0.1
# Targets (Doc/HybridGI/TurnRecoveryPlan.md): each of the first three frames under 1% noisy, settled within 1 s.
TARGET_FIRST = 1.0
TARGET_SECONDS = 1.0
LEGS = (('first visit', 0.0, 180.0), ('return', 180.0, 0.0), ('revisit', 0.0, 180.0))
EXTRA = ('--backend=hardware', '--async-compute=0')
EXPECTED = dict(backend='hardware', gather='screen_probes', width=3840, height=2160, framebufferWidth=3840,
                framebufferHeight=2160, giWidth=3840, giHeight=2160, fullscreen=True,
                asyncCompute='disabled by configuration', tierFallback='')


def pose(setup, angle):
    eye, target = gates.base_pose(setup)
    return eye, rotate_target(eye, target, angle)


def timeline(setup, hold, turn_frames):
    """Camera keys, capture frames and per-leg frame layout."""
    keys = [gates.key(1, pose(setup, 0.0)), gates.key(SETTLE, pose(setup, 0.0))]
    legs, captures = [], []
    frame = SETTLE
    for name, start, end in LEGS:
        for step in range(1, turn_frames + 1):
            keys.append(gates.key(frame + step, pose(setup, start + (end - start) * step / turn_frames)))
        landing = frame + turn_frames
        last = landing + hold - 1
        keys.append(gates.key(last, pose(setup, end)))
        offsets = [offset for offset in (*FIRST, *CHECKPOINTS) if offset <= hold - 2 * SETTLED]
        scored = [landing + offset - 1 for offset in offsets]
        reference = list(range(last - SETTLED + 1, last + 1))
        floor = list(range(last - 2 * SETTLED + 1, last - SETTLED + 1))
        legs.append(dict(name=name, angle=end, landing=landing, offsets=offsets, scored=scored, reference=reference,
                         floor=floor, hold=[landing + CHECKPOINTS[0], last], turn=[frame + 1, landing - 1]))
        captures += scored + floor + reference
        frame = last
    return dict(keys=keys), sorted(set(captures)), legs, frame


def load(folder, frame):
    path = gates.frame_path(folder, frame)
    state = read_json(str(path) + '.json')
    wrong = {key: state.get(key) for key, value in dict(EXPECTED, sceneFrames=frame).items() if state.get(key) != value}
    if wrong:
        raise ValueError(f'{path}: unexpected state {wrong}')
    with Image.open(path) as image:
        return np.asarray(image.convert('RGB'), np.float32), state


def score(image, reference):
    """Noise of one frame against the settled mean of its pose."""
    delta = image - reference
    signed = delta.mean(axis=2)
    noisy = np.abs(signed) > np.maximum(2.0, .1 * reference.mean(axis=2))
    specks = settled_turn_metrics(image, reference)
    # Noise splits evenly between darker and brighter pixels; a systematic young-probe bias does not.
    return dict(noisy=float(100 * noisy.mean()), noisy_dark=float(100 * (noisy & (signed < 0)).mean()),
                over24=specks['high_error_pct'], over4=specks['changed4_pct'], rms=specks['rms'],
                bright_pixels=specks['pixels'], bright_clusters=specks['clusters'],
                mean_signed=float(signed.mean()))


def landing_split(frames, reference):
    """Splits the error of consecutive landing frames into a changing part and a stable part.

    Noise changes from frame to frame and shows as flicker. Blur and bias stay put, so they survive
    averaging the frames. Each part uses the noisy threshold (10% of the settled value and 2 levels).
    """
    threshold = np.maximum(2.0, .1 * reference.mean(axis=2))
    errors = [(frame - reference).mean(axis=2) for frame in frames]
    stable = sum(errors) / len(errors)
    changes = [np.abs(errors[i + 1] - errors[i]) > threshold for i in range(len(errors) - 1)]
    deviation = np.sqrt(sum(np.square(error - stable, dtype=np.float64) for error in errors) / len(errors))
    return dict(flicker=float(100 * np.mean(changes)), stable_noisy=float(100 * (np.abs(stable) > threshold).mean()),
                stable_dark=float(100 * (stable < -threshold).mean()),
                stable_rms=float(np.sqrt(np.mean(np.square(stable, dtype=np.float64)))),
                changing_rms=float(np.sqrt(np.mean(np.square(deviation)))))


def error_map(path, image, reference):
    """Absolute error x4 at half resolution, for looking at where the landing noise is."""
    error = np.clip(np.abs(image - reference).max(axis=2) * 4.0, 0, 255).astype(np.uint8)
    Image.fromarray(error).resize((error.shape[1] // 2, error.shape[0] // 2), Image.BOX).save(path)


def frame_times(profile):
    times = {}
    for line in Path(profile).read_text(errors='replace').splitlines():
        if line.startswith('scene_frame='):
            fields = dict(item.split('=', 1) for item in line.split() if '=' in item)
            times[int(fields['scene_frame'])] = float(fields['frame_cpu_ms'])
    return times


def score_leg(folder, leg, times, captured):
    images, states = {}, {}
    for frame in leg['scored'] + leg['floor'] + leg['reference']:
        images[frame], states[frame] = load(folder, frame)
    reference = sum(images[frame] for frame in leg['reference']) / SETTLED
    # newProbeCache: age-zero probes, exact warm seeds, warm parent seeds (only with worldCacheSeed on).
    rows = [dict(after=offset, frame=frame, seeds=states[frame].get('newProbeCache'), **score(images[frame], reference))
            for offset, frame in zip(leg['offsets'], leg['scored'])]
    floor = [score(images[frame], reference) for frame in leg['floor']]
    split = landing_split([images[frame] for frame in leg['scored'][:len(FIRST)]], reference)
    error_map(folder / f'landing-error-{leg["name"].replace(" ", "-")}.png', images[leg['landing']], reference)
    hold = sorted(times[f] for f in range(leg['hold'][0], leg['hold'][1] + 1) if f in times and f not in captured)
    frame_ms = statistics.median(hold) if hold else None
    # The uncaptured frames of the turn itself, where each frame reveals new surfaces.
    turn = [times[f] for f in range(leg['turn'][0], leg['turn'][1] + 1) if f in times and f not in captured]
    turn_ms = statistics.median(turn) if turn else None
    # The first scored frame from which every later one stays within the limit; a late flare un-settles it.
    settled = None
    for row in reversed(rows):
        if row['noisy'] > SETTLED_LIMIT:
            break
        settled = row['after']
    return dict(name=leg['name'], angle=leg['angle'], landing=leg['landing'], frames=rows,
                first3={key: float(np.mean([row[key] for row in rows[:len(FIRST)]]))
                        for key in ('noisy', 'over24', 'over4', 'rms', 'bright_pixels')},
                floor={key: float(np.mean([row[key] for row in floor])) for key in ('noisy', 'over24', 'over4', 'rms')},
                frame_ms=frame_ms, turn_ms=turn_ms, settled_after=settled, landing_split=split,
                settled_seconds=None if settled is None or frame_ms is None else settled * frame_ms / 1000.0)


def print_report(legs):
    print(f'{"leg":12s} {"after":>5s} {"noisy %":>8s} {"dark %":>7s} {">24 lv %":>9s} {">4 lv %":>8s} {"RMS":>6s} '
          f'{"mean lv":>8s} {"bright px":>9s}')
    for leg in legs:
        for row in leg['frames']:
            print(f'{leg["name"]:12s} {row["after"]:5d} {row["noisy"]:8.3f} {row["noisy_dark"]:7.3f} '
                  f'{row["over24"]:9.4f} {row["over4"]:8.3f} {row["rms"]:6.3f} {row["mean_signed"]:8.3f} '
                  f'{row["bright_pixels"]:9d}')
        settled = ('not within the hold' if leg['settled_after'] is None
                   else f'frame {leg["settled_after"]}' + (f' (~{leg["settled_seconds"]:.1f} s)'
                                                           if leg['settled_seconds'] is not None else ''))
        print(f'{leg["name"]:12s} settled (<= {SETTLED_LIMIT}% noisy) at {settled}; still-camera floor '
              f'{leg["floor"]["noisy"]:.3f}% noisy, RMS {leg["floor"]["rms"]:.3f}; frame ms: hold '
              f'{leg["frame_ms"] or 0:.2f}, turn {leg["turn_ms"] or 0:.2f}')
        split = leg['landing_split']
        print(f'{leg["name"]:12s} frames 1-3: flicker {split["flicker"]:.3f}% (changes between frames), stable error '
              f'{split["stable_noisy"]:.3f}% of which dark {split["stable_dark"]:.3f}% (blur or bias); RMS stable '
              f'{split["stable_rms"]:.3f}, changing {split["changing_rms"]:.3f}')
    first, seconds = targets(legs)
    worst = max(row['noisy'] for leg in legs for row in leg['frames'][:len(FIRST)])
    slowest = [leg['settled_seconds'] for leg in legs]
    print(f'target: frames 1-3 < {TARGET_FIRST}% noisy on every leg: {"PASS" if first else "FAIL"} (worst {worst:.3f}%)')
    print(f'target: settled within {TARGET_SECONDS:.1f} s on every leg: {"PASS" if seconds else "FAIL"} '
          f'({", ".join("not settled" if s is None else f"{s:.2f} s" for s in slowest)})')


def targets(legs):
    first = all(row['noisy'] < TARGET_FIRST for leg in legs for row in leg['frames'][:len(FIRST)])
    seconds = all(leg['settled_seconds'] is not None and leg['settled_seconds'] <= TARGET_SECONDS for leg in legs)
    return first, seconds


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--setup', type=Path, required=True, help='Scene setup; its camera is pose A')
    parser.add_argument('--output', type=Path, required=True, help='New directory')
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--set', action='append', default=[], metavar='KEY=JSON', help='Setup override')
    parser.add_argument('--zenapp-arg', action='append', default=[], metavar='ARG',
                        help='Extra ZenApp argument, e.g. --zenapp-arg=--gi-world-cache-seed=true. A --gi- override '
                             'keeps the optimal configuration, unlike a setup edit of a preset-controlled option')
    parser.add_argument('--ball', action='store_true', help='Turn the camera light ball on')
    parser.add_argument('--turn-frames', type=int, default=TURN_FRAMES, help='Frames per 180 degree turn; 1 cuts')
    parser.add_argument('--hold', type=int, default=HOLD, help='Frames held after each landing')
    parser.add_argument('--keep-frames', action='store_true', help='Keep the raw captures for --rescore')
    parser.add_argument('--rescore', action='store_true', help='Score kept captures in --output again')
    args = parser.parse_args()
    if args.turn_frames < 1 or args.hold < CHECKPOINTS[0] + 2 * SETTLED:
        raise ValueError('--turn-frames must be at least 1 and --hold long enough for a checkpoint and the references')
    if running_renderers() and not args.rescore:
        raise RuntimeError('Another renderer is running')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=args.rescore)
    args.validation = 0
    setup = read_json(args.setup)
    for item in args.set:
        path, value = item.split('=', 1)
        node = setup
        for key in path.split('.')[:-1]:
            node = node[key]
        node[path.split('.')[-1]] = json.loads(value)
    setup['rendering']['cameraLight']['enabled'] = args.ball
    setup.setdefault('run', {})['validation'] = False
    folder = args.output / 'turn'
    camera, captures, legs, last = timeline(setup, args.hold, args.turn_frames)
    profile = folder / 'profile'
    if not args.rescore:
        command = gates.launch(args, folder, setup, camera, None, captures, last,
                               extra=(*EXTRA, *args.zenapp_arg, f'--profile={profile}'))
        identity = provenance(args, folder / 'setup.zenrender', [command])
        identity.update(tool_sha256=digest(__file__), camera_path_sha256=digest(folder / 'camera-path.json'))
        write_json(folder / 'provenance.json', identity)
    if args.rescore and not (Path(f'{profile}.rdg.log').is_file() and gates.frame_path(folder, captures[0]).is_file()):
        raise RuntimeError(f'{folder} has no kept captures; --rescore needs a run made with --keep-frames')
    times = frame_times(f'{profile}.rdg.log')
    report = [score_leg(folder, leg, times, set(captures)) for leg in legs]
    first, seconds = targets(report)
    write_json(args.output / 'report.json', dict(setup=str(args.setup), overrides=args.set,
                                                zenapp_arguments=args.zenapp_arg, ball=args.ball,
                                                settle=SETTLE, hold=args.hold, turn_frames=args.turn_frames,
                                                settled_limit_pct=SETTLED_LIMIT,
                                                targets=dict(first_frames_pct=TARGET_FIRST, first_frames_pass=first,
                                                             settle_seconds=TARGET_SECONDS, settle_pass=seconds),
                                                legs=report))
    if not args.keep_frames:
        for frame in captures:
            for path in (gates.frame_path(folder, frame), Path(str(gates.frame_path(folder, frame)) + '.json')):
                path.unlink(missing_ok=True)
        Path(f'{profile}.rdg.log').unlink(missing_ok=True)
        # ZenApp also writes its log next to the setup, a byte copy of zenapp.log.
        (folder / 'setup.log').unlink(missing_ok=True)
    print_report(report)
    return 0


if __name__ == '__main__':
    sys.exit(main())
