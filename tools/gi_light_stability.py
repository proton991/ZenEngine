"""Move a point/spot light deterministically in ZenApp, then measure recovery against static poses.

Uses the authored light ID in a .zenrender setup. Reports display-space error, the static reference's
own residual noise, exact commands and input hashes. No engine.cfg edits or executable substitution.
"""
import argparse
import copy
from pathlib import Path

import numpy as np
from PIL import Image
from gi_stability import DEFAULT_EXE, derive, digest, provenance, publish_report, read_json, write_json, zenapp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--light-id', required=True, type=int)
    parser.add_argument('--distance', default=0.04, type=float, help='World-space X displacement')
    parser.add_argument('--frames', default=8, type=int)
    parser.add_argument('--warmup', default=160, type=int)
    parser.add_argument('--recovery', default=8, type=int)
    parser.add_argument('--settle', default=256, type=int)
    parser.add_argument('--width', default=960, type=int)
    parser.add_argument('--height', default=540, type=int)
    parser.add_argument('--executable', default=DEFAULT_EXE, type=Path)
    parser.add_argument('--keep-captures', action='store_true')
    parser.add_argument('--backend', choices=('setup', 'hardware', 'compute'), default='setup')
    parser.add_argument('--gather', choices=('setup', 'pixel', 'probes'), default='setup')
    args = parser.parse_args()
    if not (args.frames > 0 and args.warmup > 0 and args.recovery > 0 and args.settle >= 128
            and 1 <= args.width <= 8192 and 1 <= args.height <= 8192 and np.isfinite(args.distance)):
        parser.error('Invalid frame count, extent or distance')
    args.lights = 'all'
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    setup = derive(read_json(args.setup), args)
    setup['animationEnabled'] = False
    setup['run'].update(windowWidth=min(args.width, 1920), windowHeight=min(args.height, 1080),
                        renderWidth=args.width, renderHeight=args.height, fullscreen=False, followWindow=False, vsync=False)
    setup['camera']['aspect'] = args.width / args.height
    light = next((entry for entry in setup['rendering']['lights'] if entry['id'] == args.light_id), None)
    if light is None or light['light']['type'] == 0:
        parser.error('--light-id must identify a point or spot light in the setup')
    initial = np.asarray(light['light']['position'], dtype=float)
    keys = [dict(frame=1, position=initial.tolist())]
    for i in range(1, args.frames + 1):
        position = initial + np.array([args.distance * i / args.frames, 0, 0])
        keys.append(dict(frame=args.warmup + i, position=position.tolist()))
    path = output/'light-path.json'
    write_json(path, dict(lightId=args.light_id, keys=keys))
    setup_path = output/'moving.zenrender'
    write_json(setup_path, setup)
    identity = provenance(args, setup_path, [])
    path_hash, tool_hash = digest(path), digest(__file__)
    last = args.warmup + args.frames + args.recovery
    commands = [zenapp(args, ['--setup='+str(setup_path), '--light-path='+str(path), f'--frames={last}',
                              '--fixed-step', '--no-ui', f'--capture-start={args.warmup+1}',
                              '--capture='+str(output/'moving')], output/'moving.log')]
    rows = []
    first_reference = None
    static_pose_change = None
    for i, key in enumerate(keys[1:], start=1):
        still = copy.deepcopy(setup)
        next(e for e in still['rendering']['lights'] if e['id'] == args.light_id)['light']['position'] = key['position']
        still_path = output/f'still-{i}.zenrender'
        write_json(still_path, still)
        commands.append(zenapp(args, ['--setup='+str(still_path), f'--frames={args.settle}', '--fixed-step', '--no-ui',
                                     f'--capture-start={args.settle-16}', '--capture='+str(output/f'still-{i}')],
                              output/f'still-{i}.log'))
        reference = np.asarray(Image.open(output/f'still-{i}.frame-{args.settle}.ppm')).astype(np.int16)
        if first_reference is None:
            first_reference = reference
        previous = np.asarray(Image.open(output/f'still-{i}.frame-{args.settle-16}.ppm')).astype(np.int16)
        reference_state = read_json(output/f'still-{i}.frame-{args.settle}.ppm.json')
        residual = np.abs(previous-reference).max(axis=2)
        changed_region = None
        if i == args.frames:
            # A light outside the camera's useful footprint can mostly measure reset noise.
            # Retain the actual endpoint change and recovery error in the affected pixels.
            change = np.abs(reference-first_reference).max(axis=2)
            changed_region = change > 4
            static_pose_change = dict(first_frame=keys[1]['frame'], last_frame=key['frame'],
                                      mean_max_channel_8bit=float(change.mean()),
                                      p99_8bit=float(np.quantile(change, .99)), max_8bit=int(change.max()),
                                      changed_pixels=int((change > 0).sum()), above_4_pixels=int(changed_region.sum()))
            Image.fromarray(changed_region.astype(np.uint8)*255).save(output/'changed-region.png')
        frames = range(key['frame'], last + 1) if i == args.frames else [key['frame']]
        for frame in frames:
            capture = output/f'moving.frame-{frame}.ppm'
            moving = np.asarray(Image.open(capture)).astype(np.int16)
            difference = np.abs(moving-reference).max(axis=2)
            moving_state = read_json(str(capture)+'.json')
            if (moving_state['sceneFrames'] != frame or reference_state['sceneFrames'] != args.settle
                    or any(state['width'] != args.width or state['height'] != args.height
                           for state in (moving_state, reference_state))):
                raise ValueError('Capture frame or render extent differs from the requested sequence')
            row = dict(frame=frame, recovery_frame=max(0, frame-args.warmup-args.frames),
                       mean_abs_8bit=float(difference.mean()), p99_8bit=float(np.quantile(difference, .99)),
                       above_4=float((difference > 4).mean()),
                       reference_residual_mean_8bit=float(residual.mean()), setup_sha256=digest(still_path),
                       expected_light_position=key['position'], moving_state=moving_state,
                       reference_state=reference_state)
            if changed_region is not None and changed_region.any():
                row.update(changed_region_mean_abs_8bit=float(difference[changed_region].mean()),
                           changed_region_p99_8bit=float(np.quantile(difference[changed_region], .99)),
                           changed_region_reference_residual_mean_8bit=float(residual[changed_region].mean()))
            rows.append(row)
            print(f"frame {frame}: {difference.mean():.3f}/255, reference residual {residual.mean():.3f}/255", flush=True)
            if not args.keep_captures:
                capture.unlink()
        if not args.keep_captures:
            for capture in output.glob(f'still-{i}.frame-*.ppm'):
                capture.unlink()
        write_json(output/'frames.json', rows)
    if digest(path) != path_hash or digest(__file__) != tool_hash:
        raise ValueError('Light path or measurement tool changed during capture')
    report = dict(kind='light_motion', light_id=args.light_id, distance=args.distance, warmup=args.warmup,
                  motion_frames=args.frames, recovery_frames=args.recovery, settle=args.settle, per_frame=rows,
                  static_pose_change=static_pose_change,
                  path_sha256=path_hash, light_tool_sha256=tool_hash, **dict(identity, commands=commands))
    publish_report(output, report, args)


if __name__ == '__main__':
    main()
