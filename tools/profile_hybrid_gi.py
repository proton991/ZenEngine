"""Profile ZenApp Compute GI or HWRT with NVIDIA Nsight Graphics GPU Trace.

Supply a prepared .zenrender setup. Overrides are explicit ZenApp startup arguments;
Data/engine.cfg is never edited. Fullscreen uses the display's native mode, and a
fullscreen capture is rejected if its framebuffer is not the requested resolution.
Base clocks are the default. Pass intervals may overlap and must not be summed into
frame time. Source-scene import and editor UI are outside the measured process.
summary.json records the state ZenApp reports. RTSS hooks ZenApp.exe unless it is
excluded there, so a capture taken while RTSS runs may include its overlay work.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess

from check_common import rtss_running

ROOT = Path(__file__).resolve().parents[1]

NSIGHT = Path('C:/Program Files/NVIDIA Corporation/Nsight Graphics 2026.3.1/host/windows-desktop-nomad-x64/ngfx.exe')
QUERY_EXTENSIONS = ('VK_KHR_acceleration_structure', 'VK_KHR_ray_query')
RENDER_STATE = re.compile(r'ZenApp render: (\d+)x(\d+), async-compute=(.+?), RHI=(\w+), ray-provider=(\w+), '
                          r'ray-query=(\w+), diagnostics=(\w+)')
PRESENTATION_STATE = re.compile(r'ZenApp presentation: fullscreen=(\w+), framebuffer=(\d+)x(\d+)')
CULLING_STATE = re.compile(r'ZenApp culling: requested=(true|false), path=(\w+), reason=([^\r\n]*)')


def fingerprint(path):
    path = Path(path)
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return dict(path=str(path), bytes=path.stat().st_size, sha256=digest)


def actual_state(log):
    """The extents and renderer state ZenApp reports for its first frame."""
    render = RENDER_STATE.search(log)
    presentation = PRESENTATION_STATE.search(log)
    culling = list(CULLING_STATE.finditer(log))
    assert render and presentation, 'ZenApp did not report its render and presentation state'
    assert culling, 'ZenApp did not report its requested and actual GPU culling path'
    return dict(render_width=int(render[1]), render_height=int(render[2]), async_compute=render[3], rhi=render[4],
                ray_provider=render[5], ray_query=render[6], diagnostics=render[7] == 'true',
                fullscreen=presentation[1] == 'true', framebuffer_width=int(presentation[2]),
                framebuffer_height=int(presentation[3]), gpu_culling_requested=culling[0][1] == 'true',
                gpu_culling_paths=[dict(path=entry[2], reason=entry[3]) for entry in culling])


def table(path):
    return list(csv.reader(path.open(newline=''), delimiter='\t'))


def summarize(folder, frames):
    base = folder / 'BASE'
    rows = table(base / 'FRAME.xls')
    assert len(rows) == 1 and rows[0][0] == 'GPU frame time', rows[:1]
    samples = [float(v) for v in rows[0][1:]]
    assert len(samples) == frames and all(v > 0 for v in samples), (len(samples), frames)
    passes = {}
    for row in table(base / 'D3DPERF_EVENTS.xls'):
        if row and row[0] != 'event_text':
            values = [float(v) if v else 0.0 for v in row[1:]]
            assert len(values) == frames, (row[0], len(values), frames)
            # Repeated labels (faces, cascades) are separate ranges; add them per frame.
            entry = passes.setdefault(row[0], dict(occurrences=0, samples_ms=[0.0] * frames))
            entry['occurrences'] += 1
            entry['samples_ms'] = [a + b for a, b in zip(entry['samples_ms'], values)]
    assert passes, 'No GPU pass labels were exported'
    for entry in passes.values():
        # The export writes 0 for a frame whose range was not recorded; exclude those frames.
        recorded = [v for v in entry['samples_ms'] if v > 0]
        entry.update(missing_frames=frames - len(recorded),
                     median_ms=statistics.median(recorded) if recorded else 0.0,
                     mean_ms=statistics.mean(recorded) if recorded else 0.0)
    ordered = sorted(samples)
    repro = {row[0]: row[1] for row in table(base / 'REPRO_INFO.xls') if len(row) >= 2}
    return dict(frames=frames, median_ms=statistics.median(samples), mean_ms=statistics.mean(samples),
                p95_ms=ordered[int(.95 * (frames - 1))], max_ms=ordered[-1], samples_ms=samples,
                passes=dict(sorted(passes.items(), key=lambda item: -item[1]['median_ms'])), capture_info=repro)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=ROOT / 'build/x64-windows-msvc-release/bin/ZenApp.exe')
    parser.add_argument('--setup', type=Path, required=True, help='Prepared ZenApp .zenrender launch setup')
    parser.add_argument('--backend', choices=('compute', 'hardware'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nsight', type=Path, default=NSIGHT)
    parser.add_argument('--start-after-ms', type=int, default=60000)
    parser.add_argument('--frames', type=int, default=30)
    parser.add_argument('--max-duration-ms', type=int, default=9000)
    parser.add_argument('--async-compute', type=int, choices=(0, 1), default=1)
    parser.add_argument('--gpu-culling', type=int, choices=(0, 1), default=1,
                        help='Use compute culling/indirect draws, or force direct draws with 0')
    parser.add_argument('--fullscreen', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--width', type=int, default=3840)
    parser.add_argument('--height', type=int, default=2160)
    parser.add_argument('--clocks', choices=('base', 'boost', 'unaltered'), default='base')
    parser.add_argument('--hardware-events', action='store_true')
    parser.add_argument('--hardware-events-kb', type=int, default=10000)
    parser.add_argument('--event-buffer-kb', type=int, default=20000)
    parser.add_argument('--timestamps', type=int, default=100000)
    parser.add_argument('--multi-pass-metrics', action='store_true')
    parser.add_argument('--shader-profiler', metavar='ARCHITECTURE')
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    if executable.stem.casefold() != 'zenapp':
        parser.error('Profiling must launch ZenApp directly; other executables and aliases are not accepted')
    if not 1 <= min(args.width, args.height) <= max(args.width, args.height) <= 8192:
        parser.error('Width and height must be in 1..8192')
    if args.frames <= 0 or args.start_after_ms < 0 or not 0 < args.max_duration_ms < 10000:
        parser.error('Frames must be positive, delay nonnegative, and duration in 1..9999 ms')
    source = args.setup.resolve(strict=True)
    setup = json.loads(source.read_text(encoding='utf-8'))
    package = Path(setup['packagePath']).resolve(strict=True)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    launch_setup = out / 'setup.zenrender'
    launch_setup.write_bytes(source.read_bytes())
    target_frames = max(30000, args.start_after_ms * 2 + args.frames)
    application = [str(executable), f'--setup={launch_setup}', f'--backend={args.backend}',
                   f'--fullscreen={int(args.fullscreen)}', f'--width={args.width}', f'--height={args.height}',
                   '--rhi=threaded', f'--async-compute={args.async_compute}', f'--gpu-culling={args.gpu_culling}',
                   '--vsync=0', '--validation=0',
                   '--fixed-step', '--no-ui', '--gpu-markers', f'--frames={target_frames}']
    if args.backend == 'compute':
        application.append('--disable-rt')
    command = [str(args.nsight), '--activity', 'GPU Trace Profiler', '--exe', str(executable),
               '--dir', str(ROOT), '--args', subprocess.list2cmdline(application[1:]),
               '--env', 'DISABLE_RTSS_LAYER=1; VK_LOADER_LAYERS_DISABLE=VK_LAYER_RTSS;',
               '--start-after-ms', str(args.start_after_ms), '--limit-to-frames', str(args.frames),
               '--max-duration-ms', str(args.max_duration_ms), '--set-gpu-clocks', args.clocks,
               '--hes-enabled', str(int(args.hardware_events)),
               '--allocated-hes-buffer-memory-kb', str(args.hardware_events_kb),
               '--allocated-event-buffer-memory-kb', str(args.event_buffer_kb),
               '--allocated-timestamps', str(args.timestamps), '--output-dir', str(out), '--auto-export']
    if args.multi_pass_metrics:
        command.append('--multi-pass-metrics')
    if args.shader_profiler:
        command += ['--architecture', args.shader_profiler, '--real-time-shader-profiler']
    (out / 'application-command.json').write_text(json.dumps(application, indent=2) + '\n')
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    inputs = dict(executable=fingerprint(executable), nsight=fingerprint(args.nsight),
                  setup=fingerprint(source), package=fingerprint(package),
                  shaders=[fingerprint(p) for p in sorted((ROOT / 'Data/SpvShaders').rglob('*.spv'))])
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    overlay = rtss_running()
    if overlay:
        print('RTSS is running; unless ZenApp.exe is excluded there, this capture may include its overlay work', flush=True)
    with (out / 'launch.log').open('w') as log:
        process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    log = (out / 'launch.log').read_text(errors='replace')
    errors = [line for line in log.splitlines() if any(token in line for token in (
        '[error]', 'VUID-', 'SYNC-HAZARD', 'TARGET ERROR:', 'Using GI defaults after rejected configuration'))]
    assert process.returncode == 0 and not errors, (process.returncode, errors[:5])
    traces = list(out.glob('*.ngfx-gputrace'))
    assert len(traces) == 1, 'Nsight did not produce exactly one trace; inspect launch.log'
    actual = actual_state(log)
    assert (actual['render_width'], actual['render_height']) == (args.width, args.height), 'Requested render extent was not confirmed'
    assert actual['ray_provider'] == args.backend, ('Backend differs from the request', actual)
    assert actual['gpu_culling_requested'] == bool(args.gpu_culling), ('Culling request differs from the command', actual)
    if args.fullscreen:
        assert actual['fullscreen'] and (actual['framebuffer_width'], actual['framebuffer_height']) == (args.width, args.height), \
            'Fullscreen framebuffer differs from requested resolution; select the correct native display mode'
    queries = [extension for extension in QUERY_EXTENSIONS if f'Enabled Device Extension: {extension}' in log]
    assert queries == (list(QUERY_EXTENSIONS) if args.backend == 'hardware' else []), queries
    report = summarize(out, args.frames)
    report.update(application='ZenApp', backend=args.backend, clocks=args.clocks, setup=str(source),
                  start_after_ms=args.start_after_ms, async_compute=args.async_compute, gpu_culling=args.gpu_culling,
                  fullscreen=args.fullscreen,
                  width=args.width, height=args.height, actual=actual, rtss_running=overlay,
                  hardware_events=args.hardware_events,
                  multi_pass_metrics=args.multi_pass_metrics, shader_profiler=args.shader_profiler,
                  trace=traces[0].name, configuration_sha256=hashlib.sha256(launch_setup.read_bytes()).hexdigest())
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{out.name}: ZenApp median {report["median_ms"]:.3f} ms, p95 {report["p95_ms"]:.3f} ms over {args.frames} frames',
          flush=True)
    for name, entry in list(report['passes'].items())[:15]:
        missing = f'  ({entry["missing_frames"]} frames unrecorded)' if entry['missing_frames'] else ''
        print(f'  {entry["median_ms"]:8.3f} ms  {name}{missing}', flush=True)


if __name__ == '__main__':
    main()
