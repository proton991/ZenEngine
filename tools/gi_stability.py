"""Measure GI noise and motion stability of a native ZenApp setup.

Use build/venv/Scripts/python.exe. Both commands run the standalone ZenApp with explicit startup
arguments and derived .zenrender copies written into the output directory; Data/engine.cfg is
never edited. report.json records the executable, shader and setup hashes and the exact arguments.

noise   Static noise per lighting term. A one-frame batch dataset derived from the setup runs
        ZenApp --ground-truth once; after the GI settles, the lighting capture is compared with the
        capture 16 frames earlier. Batch mode needs windowed fixed-size rendering, no animation and
        normal maps off, so the derived setup applies those; the report lists them.
motion  Capture every frame of a deterministic orbit/dolly, then render each pose statically.
        Reports display-space error and the static reference's own 16-frame residual noise.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import os

import numpy as np
from PIL import Image
from ground_truth_workflow import input_files, digests

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_EXE = ROOT/'build/x64-windows-msvc-release/bin/ZenApp.exe'
# Lighting capture components (Graphics/Shared/LightingCapture.h).
TERMS = {'color': 0, 'direct': 1, 'diffuse_ibl': 2, 'specular_gi': 3, 'emission': 4, 'sky_materialed': 5,
         'bounce_materialed': 6, 'D_direct': 7, 'D_sky': 8, 'D_bounce': 9}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, allow_nan=False)+'\n', encoding='utf-8')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def derive(setup, args):
    """Gather and light selection applied to a copy of the setup."""
    result = copy.deepcopy(setup)
    if args.backend != 'setup':
        provider = 2 if args.backend == 'hardware' else 1
        result['rendering']['gi']['cone'].update(rayProvider=provider, featureProviders=[provider]*6)
    if args.gather != 'setup':
        result['rendering']['gi']['cone']['gather'] = 0 if args.gather == 'pixel' else 1
    if args.lights == 'local':
        result['rendering']['lights'] = [e for e in result['rendering']['lights'] if e['light']['type'] != 0]
    elif args.lights == 'directional':
        result['rendering']['lights'] = [e for e in result['rendering']['lights'] if e['light']['type'] == 0]
    return result


def zenapp(args, arguments, log):
    exe = args.executable.resolve()
    if exe.name.lower() != 'zenapp.exe':
        raise ValueError('Measurements require the standalone ZenApp executable')
    command = [str(exe), *arguments]
    write_json(str(log)+'.command.json', command)
    with Path(log).open('w', encoding='utf-8', errors='replace') as stream:
        environment = dict(os.environ, DISABLE_RTSS_LAYER='1', VK_LOADER_LAYERS_DISABLE='VK_LAYER_RTSS')
        result = subprocess.run(command, cwd=ROOT, env=environment, stdout=stream, stderr=subprocess.STDOUT,
                                check=False, timeout=1800)
    if result.returncode:
        raise RuntimeError(f'ZenApp failed ({result.returncode}); see {log}')
    return command


def provenance(args, setup_path, commands):
    shaders = sorted((ROOT/'Data/SpvShaders').rglob('*.spv'))
    combined = hashlib.sha256()
    for path in shaders:
        combined.update(str(path.relative_to(ROOT)).encode())
        combined.update(digest(path).encode())
    setup = read_json(setup_path)
    inputs = input_files(setup) | {args.setup.resolve()}
    inputs.update(Path(p) for p in (setup['defaultEnvironment'], setup['rendering']['environment']['texturePath']) if p)
    return dict(executable=str(args.executable.resolve()), executable_sha256=digest(args.executable),
                shaders_sha256=combined.hexdigest(), shader_count=len(shaders), setup=str(setup_path),
                setup_sha256=digest(setup_path), commands=commands,
                inputs={str(p): h for p, h in digests(inputs).items()}, tool_sha256=digest(__file__))


def publish_report(output, report, args):
    current = provenance(args, Path(report['setup']), report['commands'])
    for key in ('executable_sha256', 'shaders_sha256', 'setup_sha256', 'inputs', 'tool_sha256'):
        if current[key] != report[key]:
            raise ValueError(f'Measurement input changed during capture: {key}')
    write_json(output/'report.json', report)


def noise(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    setup = derive(read_json(args.setup), args)
    # Batch mode: one window, fixed render size, no animation, normal maps off.
    width = args.width or setup['run']['renderWidth']
    height = args.height or setup['run']['renderHeight']
    setup['animationEnabled'] = False
    setup['rendering']['normalMaps'] = False
    setup['run'].update(windowWidth=min(width, 1920), windowHeight=min(height, 1080), renderWidth=width,
                        renderHeight=height, fullscreen=False, followWindow=False)
    setup['camera']['aspect'] = width / height
    dataset = output/'dataset'
    (dataset/'frames'/'view').mkdir(parents=True)
    write_json(dataset/'frames'/'view'/'scene.zenrender', setup)
    # Generous noise bounds: the batch accepts the first settled pair, the comparison is below.
    write_json(dataset/'dataset.json', dict(version=1, frames=[dict(id='view', noise_rms=dict(D_direct=1.0, D_sky=1.0,
                                                                                              D_bounce=1.0))]))
    run = output/'run'
    identity = provenance(args, dataset/'frames'/'view'/'scene.zenrender', [])
    command = zenapp(args, ['--ground-truth='+str(dataset), '--output='+str(run), f'--frames={args.settle}'], output/'zenapp.log')
    lighting = np.memmap(run/'view.lighting.bin', '<f4', mode='r', shape=(height, width, 10, 4))
    previous = np.memmap(run/'view.previous.lighting.bin', '<f4', mode='r', shape=(height, width, 10, 4))
    valid = (lighting[:, :, 0, 3] > 0) & (previous[:, :, 0, 3] > 0)
    if not valid.any():
        raise ValueError('No valid receivers')
    terms = {}
    for name, index in TERMS.items():
        value = lighting[:, :, index, :3].mean(2)[valid]
        change = np.abs(lighting[:, :, index, :3] - previous[:, :, index, :3]).mean(2)[valid]
        if not np.isfinite(value).all() or not np.isfinite(change).all():
            raise ValueError(f'Non-finite capture: {name}')
        mean = float(value.mean())
        terms[name] = dict(mean=mean, change_mean=float(change.mean()),
                           relative_change=float(change.mean()/mean) if mean > 0 else 0.0,
                           change_p999=float(np.quantile(change, .999)), change_max=float(change.max()))
    report = dict(kind='noise', interval_frames=16, pixels=int(valid.sum()), render=[width, height],
                  settled=read_json(run/'view.convergence.json'), summary=read_json(run/'view.json'),
                  derived=dict(gather=args.gather, lights=args.lights, backend=args.backend, normal_maps=False,
                               windowed=True, animation=False),
                  terms=terms, **dict(identity, commands=[command]))
    publish_report(output, report, args)
    del lighting, previous
    if not args.keep_captures:
        for path in run.glob('*.bin'):
            path.unlink()
    for name in ('color', 'specular_gi', 'D_bounce', 'D_sky', 'D_direct'):
        t = terms[name]
        print(f"{name:12} mean {t['mean']:.3e}  16-frame change {100*t['relative_change']:6.2f}%  "
              f"p99.9 {t['change_p999']:.2e}  max {t['change_max']:.2e}")


def orbit(eye, target, degrees):
    """Rotates eye around the vertical axis through target."""
    angle = np.deg2rad(degrees)
    offset = np.asarray(eye, float) - np.asarray(target, float)
    c, s = np.cos(angle), np.sin(angle)
    rotated = np.array([c*offset[0] + s*offset[2], offset[1], -s*offset[0] + c*offset[2]])
    return (np.asarray(target, float) + rotated).tolist()


def motion(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    setup = derive(read_json(args.setup), args)
    setup['animationEnabled'] = False
    width, height = args.width or setup['run']['renderWidth'], args.height or setup['run']['renderHeight']
    setup['run'].update(windowWidth=min(width, 1920), windowHeight=min(height, 1080), renderWidth=width,
                        renderHeight=height, fullscreen=False, followWindow=False, vsync=False)
    setup['camera']['aspect'] = width / height
    eye, target = setup['camera']['eye'], setup['camera']['target']
    last = args.warmup + args.frames
    keys = [dict(frame=1, eye=eye, target=target), dict(frame=args.warmup, eye=eye, target=target)]
    for frame in range(1, args.frames + 1):
        t = frame / args.frames
        rotated = np.asarray(orbit(eye, target, args.orbit_degrees*t))
        pose = np.asarray(target) + (rotated - target)*(1 - args.dolly_fraction*t)
        keys.append(dict(frame=args.warmup+frame, eye=pose.tolist(), target=target))
    path = dict(keys=keys)
    write_json(output/'camera-path.json', path)
    moving_setup = output/'moving.zenrender'
    write_json(moving_setup, setup)
    identity = provenance(args, moving_setup, [])
    # The derived setup already resolves providers and gather. A global backend CLI override
    # would reset a requested probe gather back to per-pixel.
    overrides = []
    commands = [zenapp(args, ['--setup='+str(moving_setup), '--camera-path='+str(output/'camera-path.json'),
                              f'--frames={last}', f'--capture-start={args.warmup+1}', '--fixed-step', '--no-ui',
                              '--capture='+str(output/'moving'), *overrides], output/'moving.log')]
    rows = []
    for key in keys[2:]:
        frame = key['frame']
        still = copy.deepcopy(setup)
        still['camera'].update(eye=key['eye'], target=key['target'])
        still_setup = output/f'still-{frame}.zenrender'
        write_json(still_setup, still)
        commands.append(zenapp(args, ['--setup='+str(still_setup), f'--frames={args.settle}',
                                     f'--capture-start={args.settle-16}', '--fixed-step', '--no-ui',
                                     '--capture='+str(output/f'still-{frame}'), *overrides], output/f'still-{frame}.log'))
        moving_path = output/f'moving.frame-{frame}.ppm'
        reference_path = output/f'still-{frame}.frame-{args.settle}.ppm'
        moving = np.asarray(Image.open(moving_path)).astype(np.int16)
        reference = np.asarray(Image.open(reference_path)).astype(np.int16)
        previous = np.asarray(Image.open(output/f'still-{frame}.frame-{args.settle-16}.ppm')).astype(np.int16)
        difference = np.abs(moving-reference).max(axis=2)
        residual = np.abs(previous-reference).max(axis=2)
        Image.fromarray(np.clip(difference*16, 0, 255).astype(np.uint8)).save(output/f'difference-{frame}.png')
        rows.append(dict(frame=frame, mean_abs_8bit=float(difference.mean()), p99_8bit=float(np.quantile(difference, .99)),
                         above_4=float((difference > 4).mean()), above_16=float((difference > 16).mean()),
                         reference_residual_mean_8bit=float(residual.mean()),
                         reference_residual_p99_8bit=float(np.quantile(residual, .99)),
                         moving_state=read_json(str(moving_path)+'.json'),
                         reference_state=read_json(str(reference_path)+'.json'), setup_sha256=digest(still_setup)))
        if not args.keep_captures:
            moving_path.unlink()
            for capture in output.glob(f'still-{frame}.frame-*.ppm'):
                capture.unlink()
        write_json(output/'frames.json', rows)
        print(f"Frame {frame}: mean {rows[-1]['mean_abs_8bit']:.3f}/255; reference residual {residual.mean():.3f}/255", flush=True)
    report = dict(kind='motion', warmup=args.warmup, frames=args.frames, orbit_degrees=args.orbit_degrees,
                  settle=args.settle, render=list(moving.shape[1::-1]),
                  derived=dict(gather=args.gather, lights=args.lights, backend=args.backend),
                  dolly_fraction=args.dolly_fraction, per_frame=rows,
                  mean_abs_8bit=float(np.mean([r['mean_abs_8bit'] for r in rows])),
                  path_sha256=digest(output/'camera-path.json'), **dict(identity, commands=commands))
    publish_report(output, report, args)
    print(f"motion: {len(rows)} poses, mean {report['mean_abs_8bit']:.3f}/255")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('noise', 'motion'):
        command = commands.add_parser(name)
        command.add_argument('--setup', type=Path, required=True)
        command.add_argument('--output', type=Path, required=True, help='New directory')
        command.add_argument('--executable', type=Path, default=DEFAULT_EXE)
        command.add_argument('--backend', choices=('setup', 'hardware', 'compute'), default='setup')
        command.add_argument('--gather', choices=('setup', 'pixel', 'probes'), default='setup')
        command.add_argument('--lights', choices=('all', 'local', 'directional'), default='all')
        command.add_argument('--width', type=int, default=0, help='Render width; the setup value when zero')
        command.add_argument('--height', type=int, default=0)
        command.add_argument('--keep-captures', action='store_true')
        command.add_argument('--settle', type=int, default=256, help='Frames before the final static capture')
        if name == 'motion':
            command.add_argument('--warmup', type=int, default=160, help='Frames at the start pose')
            command.add_argument('--frames', type=int, default=30, help='Frames of camera motion')
            command.add_argument('--orbit-degrees', type=float, default=10.0)
            command.add_argument('--dolly-fraction', type=float, default=0.0, help='Fraction of eye-target distance to move inward')
    args = parser.parse_args()
    if bool(args.width) != bool(args.height):
        parser.error('--width and --height go together')
    if not (0 <= args.width <= 8192 and 0 <= args.height <= 8192 and 128 <= args.settle <= 4096):
        parser.error('Extents must be 0..8192 and --settle 128..4096')
    if args.command == 'motion' and (args.warmup < 2 or args.frames < 1):
        parser.error('--warmup must be at least 2 and --frames at least 1')
    if args.command == 'motion' and (not np.isfinite(args.orbit_degrees) or not 0 <= args.dolly_fraction < 1):
        parser.error('Orbit must be finite and dolly fraction in [0, 1)')
    globals()[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
