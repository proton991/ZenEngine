"""Fast Hybrid GI A/B gate screen through standalone ZenApp: one quality launch per candidate.

Run with build/venv/Scripts/python.exe. Measures the gi_noise_probe gates in minutes, not hours:
- Fixed-step rendering is deterministic (A/A repeats are bit-identical), so each candidate runs once.
- Only what a change needs is rendered: A/A runs (--aa-offsets) measure the scene's noise band once, not per
  change; --timing-only candidates get Nsight cost without a quality launch (a baseline already measured);
  the pan capture (--pan-timing) is only for changes to motion cost.
- Converged references depend on the pose, not on temporal options; they are rendered once per preset
  with the first candidate's settings, reused by every candidate, and cached by build and setup identity.
- One quality launch per candidate and preset plays one camera and light path: a static window (flicker,
  static RMS against the matched reference, static sparks), slow pan, fast pan and dolly (moving frame
  against the converged reference at eight poses each), moving sparks and a light move with 16 recovery
  frames. ZenApp writes only the measured frames (--capture-frames).
- Cost comes from one Nsight GPU Trace per candidate (profile_hybrid_gi.py, base clocks, shorter start
  delay because the quality launch has warmed the shader cache), plus an optional capture during a
  1 degree/frame triangle pan. In-engine --profile pass times are not used: timestamps of neighbouring
  passes overlap and the per-frame pass list is truncated.

Validation layers are off by default for speed; the variant matrix covers descriptor validation.
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

from gi_matched_quality import image, metrics
from gi_noise_probe import MOTIONS, check_state, preset
from gi_spark_metric import find_sparks, rotate_target, summarize
from gi_stability import DEFAULT_EXE, ROOT, digest, provenance, read_json, write_json, zenapp
from profile_hybrid_gi import running_renderers

POSES = (8, 16, 24, 32, 40, 48, 56, 64)
GATE_POSES = (8, 32, 64)
STATIC_END = 256
FLICKER = range(STATIC_END - 31, STATIC_END + 1)
SPARK_FRAMES = 16
SETTLE = 160
LIGHT_FRAMES = 4
RECOVERY = (1, 2, 4, 8, 16)
PAN_TIMING_FRAMES = 12000
DENOISE = ('HybridReconstruct', 'GIDenoiseAtrous0', 'GIDenoiseAtrous1', 'GIDenoiseAtrous2', 'GIDenoiseStabilize',
           'GIDenoisePreblur', 'GIDenoiseTemporal', 'GIDenoiseHistoryFix')
OPTION_STATE = ('bicubicHistory', 'dataDrivenAntiLag', 'youngReceiverSamples', 'antiFirefly', 'footprintReceivers')
COMMON = ['--fixed-step', '--no-ui', '--rhi=threaded', '--gpu-culling=1', '--fullscreen', '--width=3840', '--height=2160']


def base_pose(setup):
    return list(setup['camera']['eye']), list(setup['camera']['target'])


def motion_pose(setup, kind, step):
    eye, target = base_pose(setup)
    if kind == 'dolly':
        direction = np.subtract(target, eye) / np.linalg.norm(np.subtract(target, eye))
        shift = direction * setup['camera']['sceneScale'] * .005 * step / 64
        return (np.add(eye, shift).tolist(), np.add(target, shift).tolist())
    speed = .25 if kind == 'pan-slow' else 1.
    return eye, rotate_target(eye, target, speed * step)


def key(frame, pose):
    return dict(frame=frame, eye=list(pose[0]), target=list(pose[1]))


def light_positions(setup, light_id, distance):
    entry = next((e for e in setup['rendering']['lights'] if e['id'] == light_id), None)
    if entry is None or entry['light']['type'] == 0:
        raise ValueError('--light-id must identify a point or spot light in the setup')
    initial = np.asarray(entry['light']['position'], float)
    return [(initial + [distance * i / LIGHT_FRAMES, 0, 0]).tolist() for i in range(LIGHT_FRAMES + 1)]


def quality_timeline(setup, lights, offset=0, static_end=STATIC_END, recovery=RECOVERY, poses=POSES):
    """Camera and light paths of the quality launch, the frames to capture and where each measurement is.

    offset delays everything by that many static frames: the same measurement with other per-frame noise.
    """
    base = base_pose(setup)
    keys = [key(1, base)]
    flicker_frames = list(range(static_end - 31 + offset, static_end + 1 + offset))
    layout = dict(flicker=flicker_frames, static_sparks=flicker_frames[-SPARK_FRAMES:], motion={})
    frame = static_end + offset
    for kind in MOTIONS:
        keys.append(key(frame, base))
        keys += [key(frame + step, motion_pose(setup, kind, step)) for step in range(1, 65)]
        keys.append(key(frame + 65, base))
        layout['motion'][kind] = {step: frame + step for step in poses}
        frame += 65 + SETTLE
    frame += frame % 2  # Odd frames take the second pose, as in gi_spark_metric.camera_path.
    moved = (base[0], rotate_target(base[0], base[1], .5))
    keys.append(key(frame, base))
    keys += [key(frame + i, moved if (frame + i) % 2 else base) for i in range(1, SPARK_FRAMES + 1)]
    keys.append(key(frame + SPARK_FRAMES + 1, base))
    layout['moving_sparks'] = list(range(frame + 1, frame + SPARK_FRAMES + 1))
    frame += SPARK_FRAMES + 1 + SETTLE
    light_keys = [dict(frame=1, position=lights[0])]
    light_keys += [dict(frame=frame + i, position=lights[i]) for i in range(1, LIGHT_FRAMES + 1)]
    layout['light'] = {r: frame + LIGHT_FRAMES + r for r in recovery}
    last = frame + LIGHT_FRAMES + max(recovery)
    captures = sorted(set(layout['flicker'] + layout['moving_sparks'] + list(layout['light'].values())
                          + [f for poses in layout['motion'].values() for f in poses.values()]))
    return dict(keys=keys), light_keys, captures, layout, last


def reference_timeline(setup, lights, hold, poses=POSES):
    """One hold per motion pose and one for the moved light; the first hold also covers scene warm-up."""
    targets = [(kind, step, motion_pose(setup, kind, step)) for kind in MOTIONS for step in poses]
    keys, captures, layout, frame = [], [], {kind: {} for kind in MOTIONS}, 1
    for index, (kind, step, pose) in enumerate(targets):
        length = hold + (64 if index == 0 else 0)
        keys += [key(frame, pose), key(frame + length - 1, pose)]
        layout[kind][step] = frame + length - 1
        captures.append(frame + length - 1)
        frame += length
    keys.append(key(frame, base_pose(setup)))
    light_keys = [dict(frame=1, position=lights[0]), dict(frame=frame, position=lights[-1])]
    layout['light'] = frame + hold - 1
    captures.append(layout['light'])
    return dict(keys=keys), light_keys, captures, layout, layout['light']


def launch(args, folder, setup, camera, light_keys, frames, last, extra=()):
    """One ZenApp run; returns the command. Captures land at folder/capture.frame-N.ppm."""
    folder.mkdir(parents=True, exist_ok=True)
    write_json(folder / 'setup.zenrender', setup)
    write_json(folder / 'camera-path.json', camera)
    command = [f'--setup={folder / "setup.zenrender"}', f'--frames={last}', f'--camera-path={folder / "camera-path.json"}',
               f'--validation={int(args.validation)}', *COMMON, *extra]
    if light_keys is not None:
        write_json(folder / 'light-path.json', dict(lightId=args.light_id, keys=light_keys))
        command.append(f'--light-path={folder / "light-path.json"}')
    if frames:
        command += [f'--capture={folder / "capture"}', '--capture-frames=' + ','.join(map(str, frames))]
    started = time.perf_counter()
    command = zenapp(args, command, folder / 'zenapp.log')
    log = (folder / 'zenapp.log').read_text(errors='replace')
    for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
        if marker in log:
            raise ValueError(f'ZenApp log marker {marker!r}: {folder}')
    print(f'  {folder.relative_to(args.output)}: {last} frames, {len(frames)} captures, '
          f'{time.perf_counter() - started:.0f} s', flush=True)
    return command


def frame_path(folder, frame):
    return folder / f'capture.frame-{frame}.ppm'


def load(folder, setup, frame):
    state = read_json(str(frame_path(folder, frame)) + '.json')
    check_state(state, setup, frame)
    cone = setup['rendering']['gi']['cone']
    for name in OPTION_STATE:
        expected = False if cone.get('screenProbeGI', False) else bool(cone.get(name, True))
        if state.get(name) != expected:  # Engine defaults since 2026-11-14.
            raise ValueError(f'Captured {name}={state.get(name)!r} differs from the setup: {folder}')
    return image(frame_path(folder, frame))


def references(args, preset_name, setup, lights, name=None):
    """Converged images per motion pose and for the moved light, rendered once per build and setup.

    name renders the candidate's own references (--own-references), for candidates whose settled images differ.
    """
    folder = args.output / preset_name / ('reference' if name is None else f'{name}/reference')
    camera, light_keys, captures, layout, last = reference_timeline(setup, lights, args.hold)
    # Rendering inputs only: editing the tool's measurements does not invalidate converged images.
    build = {name: value for name, value in args.identity.items() if name != 'tool_sha256'}
    identity = dict(build=build, setup=setup, camera=camera, light_keys=light_keys, hold=args.hold)
    stored = folder / 'reference.json'
    if stored.exists() and read_json(stored)['identity'] == identity:
        return read_json(stored)['layout'], folder
    if folder.exists():
        for old in folder.glob('capture.frame-*'):
            old.unlink()
    command = launch(args, folder, setup, camera, light_keys, captures, last)
    for frame in captures:
        load(folder, setup, frame)
    write_json(stored, dict(identity=identity, layout=layout, command=command))
    return read_json(stored)['layout'], folder


def flicker(folder, setup, frames, args):
    mean = m2 = None
    for count, frame in enumerate(frames, 1):
        peak = load(folder, setup, frame).max(axis=2).astype(np.float64)
        if mean is None:
            mean, m2 = np.zeros_like(peak), np.zeros_like(peak)
        delta = peak - mean
        mean += delta / count
        m2 += delta * (peak - mean)
    std = np.sqrt(m2 / len(frames))
    Image.fromarray(np.clip(std * 32, 0, 255).astype(np.uint8)).save(folder / 'std-x32.png')
    reference_setup = read_json(args.reference / 'static/center/half/setup.zenrender')
    for name in ('camera', 'packagePath', 'sourcePath'):
        if setup[name] != reference_setup[name]:
            raise ValueError(f'Static reference {name} differs from the candidate')
    for name in ('lights', 'environment', 'normalMaps', 'cameraLight'):
        if setup['rendering'][name] != reference_setup['rendering'][name]:
            raise ValueError(f'Static reference rendering.{name} differs from the candidate')
    reference = np.load(args.reference / 'static/center/reference-mean.npy', allow_pickle=False)
    mask = np.asarray(Image.open(args.reference_mask)) > 0
    last = load(folder, setup, frames[-1])
    return dict(mean_std=float(std.mean()), p999_std=float(np.percentile(std, 99.9)),
                static_reference=metrics(last, reference, mask))


def sparks(folder, setup, frames):
    values = np.stack([np.asarray(Image.open(frame_path(folder, f)), dtype=np.int16) for f in frames])
    for frame in frames:
        check_state(read_json(str(frame_path(folder, frame)) + '.json'), setup, frame)
    rows, pixels = find_sparks(values, 48)
    return summarize(rows, pixels, len(frames) - 4)


def quality(args, preset_name, name, setup, lights, reference_layout, reference_folder, offset=0):
    folder = args.output / preset_name / name / 'quality'
    report_path = folder / 'report.json'
    camera, light_keys, captures, layout, last = quality_timeline(setup, lights, offset)
    identity = dict(build=args.identity, setup=setup, camera=camera, light_keys=light_keys)
    if report_path.exists() and read_json(report_path)['identity'] == identity:
        return read_json(report_path)['result']
    command = launch(args, folder, setup, camera, light_keys, captures, last)
    result = dict(flicker=flicker(folder, setup, layout['flicker'], args),
                  sparks=dict(static=sparks(folder, setup, layout['static_sparks']),
                              moving=sparks(folder, setup, layout['moving_sparks'])))
    for kind in MOTIONS:
        rows = []
        for step, frame in layout['motion'][kind].items():
            moving = load(folder, setup, frame)
            settled = image(frame_path(reference_folder, reference_layout[kind][str(step)]))
            rows.append(dict(pose=step, frame=frame, **metrics(moving, settled)))
        result[kind] = rows
    settled = image(frame_path(reference_folder, reference_layout['light']))
    result['light'] = []
    for recovery, frame in layout['light'].items():
        difference = np.abs(load(folder, setup, frame) - settled).max(axis=2)
        result['light'].append(dict(recovery_frame=recovery, frame=frame, mean_abs_8bit=float(difference.mean()),
                                    above_4=float((difference > 4).mean()), p99_8bit=float(np.quantile(difference, .99))))
    state = read_json(str(frame_path(folder, layout['flicker'][-1])) + '.json')
    if not args.keep_captures:
        for old in folder.glob('capture.frame-*.ppm'):
            old.unlink()
    write_json(report_path, dict(identity=identity, result=result, command=command, state=state, layout=layout))
    return result


def triangle_pan(setup):
    """1 degree/frame between -20 and +20 degrees for the whole Nsight run (the last key holds)."""
    base = base_pose(setup)
    keys = []
    for frame in range(1, PAN_TIMING_FRAMES + 1):
        phase = frame % 80
        degrees = phase if phase <= 20 else 40 - phase if phase <= 60 else phase - 80
        keys.append(key(frame, (base[0], rotate_target(base[0], base[1], degrees))))
    return dict(keys=keys)


def warm_up(args, preset_name, name, setup):
    """A 64-frame launch without captures, so a cost-only candidate's Nsight run does not time cold compiles."""
    folder = args.output / preset_name / name / 'warm-up'
    eye, target = base_pose(setup)
    launch(args, folder, setup, dict(keys=[key(1, (eye, target)), key(64, (eye, target))]), None, [], 64)


def nsight(args, preset_name, name, setup, moving):
    """Nsight GPU Trace (profile_hybrid_gi.py, base clocks) of the candidate, static or panning."""
    folder = args.output / preset_name / name / ('nsight-pan' if moving else 'nsight')
    identity_path = folder.parent / f'{folder.name}.identity.json'
    identity = dict(build=args.identity, setup=setup, start_after_ms=args.nsight_start_ms, moving=moving)
    if (folder / 'summary.json').exists() and identity_path.exists() and read_json(identity_path) == identity:
        return read_json(folder / 'summary.json')
    if folder.exists():
        shutil.rmtree(folder)  # An interrupted capture; profile_hybrid_gi.py requires a new folder.
    inputs = folder.parent / f'{folder.name}-input'
    inputs.mkdir(parents=True, exist_ok=True)
    write_json(inputs / 'setup.zenrender', setup)
    backend = 'compute' if setup['rendering']['gi']['cone']['rayProvider'] == 1 else 'hardware'
    command = [sys.executable, str(ROOT / 'tools/profile_hybrid_gi.py'), f'--exe={args.executable}',
               f'--setup={inputs / "setup.zenrender"}', f'--backend={backend}', f'--output={folder}',
               f'--start-after-ms={args.nsight_start_ms}']
    if moving:
        write_json(inputs / 'camera-path.json', triangle_pan(setup))
        command.append(f'--camera-path={inputs / "camera-path.json"}')
    started = time.perf_counter()
    with (folder.parent / f'{folder.name}.log').open('w', encoding='utf-8', errors='replace') as log:
        subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
    if not args.keep_traces:
        for trace in folder.glob('*.ngfx-gputrace'):
            trace.unlink()
    write_json(identity_path, identity)
    print(f'  {folder.relative_to(args.output)}: {time.perf_counter() - started:.0f} s', flush=True)
    return read_json(folder / 'summary.json')


def cost(summary):
    passes = summary['passes']
    return dict(frame_ms=summary['median_ms'], trace_ms=passes.get('HybridTrace', {}).get('median_ms'),
                denoise_ms=sum(passes[name]['median_ms'] for name in DENOISE if name in passes))


def flatten(result):
    values = {}
    if result['quality']:
        values = flatten_quality(result['quality'])
    for window, summary in result['cost'].items():
        values.update({f'{window}/{metric}': value for metric, value in summary.items()})
    return values


def flatten_quality(quality_result):
    flicker_report = quality_result['flicker']
    values = {'static_flicker': flicker_report['mean_std'], 'static_flicker_p999': flicker_report['p999_std'],
              'static_rms': flicker_report['static_reference']['rms_8bit']}
    for kind in MOTIONS:
        rows = quality_result[kind]
        values[f'{kind}/mean'] = float(np.mean([row['above_4_percent'] for row in rows]))
        values.update({f'{kind}/{row["pose"]}': row['above_4_percent'] for row in rows if row['pose'] in GATE_POSES})
    for camera in ('static', 'moving'):
        values[f'sparks/{camera}/pixels'] = quality_result['sparks'][camera]['spark_pixels_per_frame']
    for row in quality_result['light']:
        if row['recovery_frame'] == max(RECOVERY):
            values.update({'light/above_4_percent': 100 * row['above_4'], 'light/mean_abs_8bit': row['mean_abs_8bit']})
    return values


def parse_candidate(text):
    name, _, assignments = text.partition(':')
    options = {}
    for assignment in filter(None, assignments.split(',')):
        option, _, value = assignment.partition('=')
        options[option] = json.loads(value)
    return name, options


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--candidate', action='append', required=True,
                        help='NAME[:option=json,...] applied to rendering.gi.cone; the first is the comparison base')
    parser.add_argument('--presets', nargs='+', choices=('hardware', 'compute', 'editor'), default=['hardware'])
    parser.add_argument('--light-id', type=int, default=3)
    parser.add_argument('--light-distance', type=float, default=0.04)
    parser.add_argument('--hold', type=int, default=160, help='Frames per converged reference pose')
    parser.add_argument('--own-references', action='store_true',
                        help='Compare each candidate with its own converged references instead of the first '
                             "candidate's: for different methods or GI resolutions, which settle to different images")
    parser.add_argument('--validation', action='store_true', help='Run with Vulkan validation layers (slower)')
    parser.add_argument('--keep-captures', action='store_true')
    parser.add_argument('--aa-offsets', nargs='*', type=int, default=[], metavar='FRAMES',
                        help='A/A runs of the first candidate delayed by these frames, e.g. 1 2. Off by default: the '
                             'noise band belongs to the scene and harness, so measure it once, not per change')
    parser.add_argument('--timing-only', nargs='*', default=[], metavar='NAME',
                        help='Candidates measured for cost only (no quality launch), e.g. a baseline whose images an '
                             'earlier run already measured')
    parser.add_argument('--no-timing', action='store_true', help='Skip the Nsight cost captures')
    parser.add_argument('--pan-timing', nargs='*', default=[], metavar='NAME',
                        help='Candidates that also get an Nsight capture during a 1 degree/frame pan (plus the first)')
    parser.add_argument('--nsight-start-ms', type=int, default=20000,
                        help='Nsight start delay; the quality launch before it has warmed the shader cache')
    parser.add_argument('--keep-traces', action='store_true', help='Keep the ~1 GB .ngfx-gputrace files')
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--reference', type=Path, default=Path('E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2'))
    parser.add_argument('--reference-mask', type=Path,
                        default=Path('E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-reference-module/mask.png'))
    args = parser.parse_args()
    if running_renderers():
        parser.error('Close other Zen renderers before measuring')
    args.output, args.setup = args.output.resolve(), args.setup.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    identity = provenance(args, args.setup, [])
    identity.update(tool_sha256=digest(__file__), reference_sha256=digest(args.reference / 'static/center/reference-mean.npy'))
    args.identity = {k: identity[k] for k in ('executable_sha256', 'shaders_sha256', 'tool_sha256', 'reference_sha256')}
    source = read_json(args.setup)
    candidates = [parse_candidate(text) for text in args.candidate]
    summary = dict(identity=identity, candidates=dict(candidates), presets={})
    for preset_name in args.presets:
        print(preset_name, flush=True)
        setups = {}
        for name, options in candidates:
            setup = preset(source, preset_name)
            setup['rendering']['gi']['cone'].update(options)
            setups[name] = setup
        lights = light_positions(setups[candidates[0][0]], args.light_id, args.light_distance)
        # The converged reference only serves quality launches; a cost-only screen skips it.
        quality_needed = bool(args.aa_offsets) or any(name not in args.timing_only for name, _ in candidates)
        layout, reference_folder = (references(args, preset_name, setups[candidates[0][0]], lights)
                                    if quality_needed and not args.own_references else (None, None))
        results = {}
        first = candidates[0][0]
        if args.aa_offsets and args.own_references:
            layout, reference_folder = references(args, preset_name, setups[first], lights, first)
        for offset in args.aa_offsets:
            # A/A: the first candidate again on a delayed timeline; quality only, so no cost entry.
            results[f'{first}+{offset}'] = dict(quality=quality(args, preset_name, f'{first}+{offset}', setups[first],
                                                                lights, layout, reference_folder, offset), cost={})
        pan_timing = set(args.pan_timing) | ({first} if args.pan_timing else set())
        for name, _ in candidates:
            if name in args.timing_only:
                # Cost only: a short launch warms the driver's shader cache in place of the quality launch.
                warm_up(args, preset_name, name, setups[name])
                results[name] = dict(quality={}, cost={})
            else:
                if args.own_references:
                    layout, reference_folder = references(args, preset_name, setups[name], lights, name)
                results[name] = dict(quality=quality(args, preset_name, name, setups[name], lights, layout,
                                                     reference_folder), cost={})
            if not args.no_timing:
                results[name]['cost']['static'] = cost(nsight(args, preset_name, name, setups[name], False))
            if not args.no_timing and name in pan_timing:
                results[name]['cost']['pan'] = cost(nsight(args, preset_name, name, setups[name], True))
        summary['presets'][preset_name] = {name: flatten(result) for name, result in results.items()}
    if digest(args.executable) != identity['executable_sha256']:
        raise ValueError('ZenApp changed during the run')
    summary['seconds'] = time.perf_counter() - started
    write_json(args.output / 'summary.json', summary)
    for table in summary['presets'].values():
        print_table(table, candidates[0][0])
    print(f'\n{summary["seconds"] / 60:.1f} min', flush=True)


def print_table(table, first):
    """Values with the change against the first candidate; '~' marks a change inside the A/A spread."""
    names = list(table)
    aa = [name for name in names if name.startswith(first + '+')]
    print(f'\n(change vs {first}; ~ = within the A/A spread of {", ".join(aa) or "no A/A run"})')
    print('  '.join([f'{"metric":26}'] + [f'{name:>21}' for name in names]))
    # Every metric any candidate has, so candidates still show quality when the first is --timing-only.
    metrics = list(dict.fromkeys(metric for name in names for metric in table[name]))
    for metric in metrics:
        base   = table[first].get(metric)
        spread = None
        if base is not None:
            spread = max([abs(table[name][metric] - base) for name in aa if metric in table[name]], default=None)
        cells = []
        for name in names:
            value = table[name].get(metric)
            if value is None:
                cells.append('-'.rjust(21))
                continue
            change = ''
            if name != first and base:  # No change column without a base value.
                noise = '~' if spread is not None and abs(value - base) <= spread else ''
                change = f' ({100 * (value - base) / base:+.0f}%{noise})'
            cells.append(f'{value:.4g}{change}'.rjust(21))
        print('  '.join([f'{metric:26}'] + cells))


if __name__ == '__main__':
    main()
