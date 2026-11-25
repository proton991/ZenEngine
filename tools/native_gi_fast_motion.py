"""Native GI image quality during and just after quick camera motion, using canonical ZenApp.

The Phase 2 gates move the camera slowly (0.25-1 degree per frame, a dolly of about 1e-4 scene
units per frame) and score frames 8-64 after the motion starts. This tool scores every frame class a
user sees during a quick turn, a quick walk and a quick strafe at interactive navigation speed, and
the first frames after the camera stops. Each scored frame is compared with a settled image of the
same pose, held in a separate reference launch.

Use build/venv/Scripts/python.exe from the repository root. Data/engine.cfg is never edited.

Metrics per scored frame (display levels of 255, max over RGB):
  above4/above16      share of pixels differing by more than 4 / 16 levels
  bright16/dark16     share more than 16 levels brighter / darker than settled
  sparkle             bright16 pixels with at most four bright16 pixels in their 5x5 window
  blotch              the remaining bright16 pixels (wider over-bright regions and trails)
"""
import argparse
import json
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
from PIL import Image

from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json
from gi_spark_metric import rotate_target
from profile_hybrid_gi import running_renderers
import gi_quick_gates as gates

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SETUP = ROOT / 'build/checks/native-gi/sponza-turn-120/full/setup.zenrender'
SKY = ROOT / 'Data/Textures/Environments/kloofendal_48d_partly_cloudy_puresky_1k.hdr'
SCORED = (1, 3, 6, 9, 12)
AFTER = (1, 2, 4, 8, 16, 32, 64)
# A quick mouse turn of 7.5 degrees per frame. Default navigation moves 1 scene unit per second and
# ZenApp renders this view at 16-40 fps; the courtyard bounds the moves to about 0.1 scene units.
MOTIONS = {'turn': ('yaw', 7.5), 'turn180': ('yaw', 30.0), 'cut180': ('yaw', 180.0),
           'forward': ('forward', .008), 'strafe': ('right', .004)}
EXTRA = ['--backend=hardware', '--async-compute=0']


def scored_steps(motion):
    # The six-frame flick preserves overlapping history. A one-frame cut also exercises history rejection.
    return {'turn180': (1, 3, 6), 'cut180': (1,)}.get(motion, SCORED)


def pose(setup, motion, step):
    eye = np.asarray(setup['camera']['eye'], float)
    target = np.asarray(setup['camera']['target'], float)
    kind, rate = MOTIONS[motion]
    if kind == 'yaw':
        return eye.tolist(), rotate_target(eye, target, rate * step)
    forward = target - eye
    forward[1] = 0.0
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0.0, 1.0, 0.0])
    shift = (forward if kind == 'forward' else right) * rate * step
    return (eye + shift).tolist(), (target + shift).tolist()


def derive(source, overrides):
    setup = json.loads(json.dumps(source))
    setup['rendering']['environment']['texturePath'] = str(SKY.resolve())
    setup['animationEnabled'] = False
    for item in overrides:
        path, value = item.split('=', 1)
        node = setup
        keys = path.split('.')
        for name in keys[:-1]:
            node = node[name]
        if keys[-1] not in node:
            raise ValueError(f'Unknown setup key {path}')
        node[keys[-1]] = json.loads(value)
    return setup


def moving_timeline(setup, motions, warmup, settle):
    base = gates.base_pose(setup)
    keys = [gates.key(1, base)]
    layout, captures, frame = {}, [], warmup
    for motion in motions:
        scored = scored_steps(motion)
        steps = scored[-1]
        keys.append(gates.key(frame, base))
        keys += [gates.key(frame + step, pose(setup, motion, step)) for step in range(1, steps + 1)]
        end = frame + steps + max(AFTER)
        keys.append(gates.key(end, pose(setup, motion, steps)))
        rows = {f'move/{step}': frame + step for step in scored}
        rows.update({f'stop/{after}': frame + steps + after for after in AFTER})
        layout[motion] = rows
        captures += rows.values()
        keys.append(gates.key(end + 1, base))
        frame = end + 1 + settle
    return dict(keys=keys), layout, sorted(captures), frame


def reference_timeline(setup, motions, warmup, hold):
    keys, layout, captures, frame = [], {}, [], 1
    for motion in motions:
        layout[motion] = {}
        for step in scored_steps(motion):
            length = hold + (warmup if frame == 1 else 0)
            target = pose(setup, motion, step)
            keys += [gates.key(frame, target), gates.key(frame + length - 1, target)]
            layout[motion][step] = frame + length - 1
            captures.append(frame + length - 1)
            frame += length
    return dict(keys=keys), layout, captures, frame - 1


def check(path, frame):
    state = read_json(str(path) + '.json')
    expected = dict(sceneFrames=frame, backend='hardware', gather='screen_probes', giWidth=3840, giHeight=2160,
                    tierFallback='')
    wrong = {k: state.get(k) for k, v in expected.items() if state.get(k) != v}
    if wrong or not state.get('worldCacheCapacity'):
        raise ValueError(f'{path}: unexpected state {wrong}')
    return state


def load(folder, frame):
    path = gates.frame_path(folder, frame)
    check(path, frame)
    return np.asarray(Image.open(path).convert('RGB'), np.int16)


def box_count(mask, radius):
    padded = np.pad(mask.astype(np.int32), radius)
    table = padded.cumsum(0).cumsum(1)
    table = np.pad(table, ((1, 0), (1, 0)))
    size = 2 * radius + 1
    return table[size:, size:] - table[:-size, size:] - table[size:, :-size] + table[:-size, :-size]


def score(moving, settled):
    delta = moving - settled
    magnitude = np.abs(delta).max(axis=2)
    bright = delta.max(axis=2) > 16
    dark = delta.min(axis=2) < -16
    isolated = bright & (box_count(bright, 2) <= 4)
    count = magnitude.size / 100.0
    return dict(rms=float(np.sqrt(np.mean(np.square(delta, dtype=np.float64)))),
                mean_bias=float(delta.mean()), above4=float((magnitude > 4).sum() / count),
                above16=float((magnitude > 16).sum() / count), bright16=float(bright.sum() / count),
                dark16=float(dark.sum() / count), sparkle=float(isolated.sum() / count),
                blotch=float((bright & ~isolated).sum() / count)), delta


def preview(delta, moving, settled, path):
    """Half-resolution moving | settled | signed difference x4 (red brighter, blue darker)."""
    def half(value):
        return value.reshape(1080, 2, 1920, 2, 3).mean(axis=(1, 3))
    signed = half(delta.astype(np.float32)).mean(axis=2)
    difference = np.zeros((1080, 1920, 3), np.float32)
    difference[..., 0] = np.clip(signed * 4, 0, 255)
    difference[..., 2] = np.clip(-signed * 4, 0, 255)
    sheet = np.concatenate([half(moving), half(settled), difference], axis=1)
    Image.fromarray(np.clip(sheet, 0, 255).astype(np.uint8)).save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, required=True, help='New directory')
    parser.add_argument('--setup', type=Path, default=DEFAULT_SETUP)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--motions', default='turn,forward,strafe', help='Comma-separated choices: ' + ', '.join(MOTIONS))
    parser.add_argument('--set', action='append', default=[], metavar='KEY=JSON', help='Setup override')
    parser.add_argument('--warmup', type=int, default=1024)
    parser.add_argument('--settle', type=int, default=512, help='Frames at the base pose between motions')
    parser.add_argument('--hold', type=int, default=512, help='Reference hold per scored pose')
    parser.add_argument('--references', type=Path, help='Reuse another run\'s reference folder')
    parser.add_argument('--previews', default='move/3,move/12,stop/1,stop/4,stop/16')
    parser.add_argument('--smoke', action='store_true', help='Short validated moving launch, no scoring')
    parser.add_argument('--rescore', action='store_true', help='Score an existing run\'s moving captures again')
    parser.add_argument('--validation', type=int, default=0)
    parser.add_argument('--production-turn', choices=('sweep', 'cut'),
                        help='Replay the setup at native 4K, score every turn and recovery frame in both travel directions')
    parser.add_argument('--degrees', type=int, choices=(-180, -120, -90, 90, 120, 180), default=180)
    parser.add_argument('--phase', type=int, choices=range(64), default=0, metavar='0..63')
    parser.add_argument('--cache-cells', type=int, default=4194304,
                        help='Required world-cache capacity; 0 measures the capacity and requires it throughout both runs')
    parser.add_argument('--max-noise-pct', type=float, default=.1,
                        help='Strict per-frame limit for pixels exceeding 24 display levels in either direction, in percent')
    args = parser.parse_args()
    if args.production_turn:
        from gi_turn_replay import replay
        return replay(args)
    if running_renderers() and not args.rescore:
        raise RuntimeError('Another renderer is running')
    motions = args.motions.split(',')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=args.rescore)
    setup = derive(read_json(args.setup), args.set)
    args.output = output
    args.light_id = None
    if args.smoke:
        args.validation, args.warmup, args.settle = 1, 32, 32
    camera, layout, captures, last = moving_timeline(setup, motions, args.warmup, args.settle)
    moving_folder = output / 'moving'
    if args.rescore:
        if read_json(moving_folder / 'camera-path.json') != json.loads(json.dumps(camera)):
            raise ValueError('The existing moving captures used another camera path')
        commands = [read_json(moving_folder / 'zenapp.log.command.json')]
    else:
        commands = [gates.launch(args, moving_folder, setup, camera, None, captures, last, EXTRA)]
    if args.smoke:
        for motion in motions:
            frame = layout[motion]['stop/1']
            check(gates.frame_path(moving_folder, frame), frame)
            image = Image.open(gates.frame_path(moving_folder, frame)).convert('RGB').reduce(2)
            image.save(output / f'{motion}-end.png')
        print('smoke ok', flush=True)
        return 0
    if args.references:
        reference_folder = args.references.resolve()
        reference_layout = read_json(reference_folder / 'layout.json')
        if reference_layout['motions'] != json.loads(json.dumps({m: MOTIONS[m] for m in motions})):
            raise ValueError('Reused references were rendered for other motions')
        reference_layout = reference_layout['layout']
    else:
        reference_folder = output / 'reference'
        ref_camera, reference_layout, ref_captures, ref_last = reference_timeline(setup, motions, args.warmup, args.hold)
        commands.append(gates.launch(args, reference_folder, setup, ref_camera, None, ref_captures, ref_last, EXTRA))
        write_json(reference_folder / 'layout.json', dict(motions={m: MOTIONS[m] for m in motions},
                                                          layout=reference_layout, hold=args.hold))
        reference_layout = read_json(reference_folder / 'layout.json')['layout']
    results, wanted = {}, set(args.previews.split(','))
    for motion in motions:
        rows = {}
        for name, frame in layout[motion].items():
            phase, value = name.split('/')
            step = int(value) if phase == 'move' else scored_steps(motion)[-1]
            settled_frame = reference_layout[motion][str(step)]
            moving = load(moving_folder, frame)
            settled = load(reference_folder, settled_frame)
            rows[name], delta = score(moving, settled)
            if name in wanted:
                preview(delta, moving, settled, output / f'{motion}-{name.replace("/", "")}.png')
        results[motion] = rows
        move = [rows[f'move/{s}'] for s in scored_steps(motion)]
        stop = [rows[f'stop/{a}'] for a in AFTER[:4]]
        print(f'{motion:8s} during: above16 {np.mean([r["above16"] for r in move]):.3f}%  '
              f'sparkle {np.mean([r["sparkle"] for r in move]):.4f}%  blotch {np.mean([r["blotch"] for r in move]):.3f}%  '
              f'dark16 {np.mean([r["dark16"] for r in move]):.3f}% | first 8 after stop: above16 '
              f'{np.mean([r["above16"] for r in stop]):.3f}%  sparkle {np.mean([r["sparkle"] for r in stop]):.4f}%', flush=True)
    identity = provenance(SimpleNamespace(executable=args.executable, setup=args.setup.resolve()),
                          moving_folder / 'setup.zenrender', commands)
    schedules = {motion: scored_steps(motion) for motion in motions}
    # Keep the original report fields when every motion uses the same schedule.
    common = schedules[motions[0]]
    timing = (dict(scored=common, steps=common[-1]) if all(s == common for s in schedules.values())
              else dict(scored_by_motion=schedules, steps_by_motion={m: s[-1] for m, s in schedules.items()}))
    write_json(output / 'report.json', dict(results=results, overrides=args.set, motions={m: MOTIONS[m] for m in motions},
                                            after=AFTER, warmup=args.warmup, **timing,
                                            settle=args.settle, hold=args.hold,
                                            references=str(reference_folder), provenance=identity,
                                            path_sha256=digest(moving_folder / 'camera-path.json')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
