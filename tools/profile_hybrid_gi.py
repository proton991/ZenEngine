"""Capture a warmed Nsight Graphics GPU Trace of the hybrid GI P8 baseline; owns engine.cfg serially.

Uses benchmark_hybrid_gi.py's baseline workload on one frozen top/hall view with the all-feature
Compute or HW RT selector. RDG pass names reach the trace as Vulkan debug labels (--gpu-markers).
Nsight locks clocks (base by default) and disables VSync, so these GPU times are not comparable
with the unprofiled benchmark's boost-clock frame intervals. Pass ranges overlap across queues and
must not be summed into a frame time.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

from benchmark_hybrid_gi import BASELINE, check_frozen_view
from capture_hybrid_gi import CAMERAS, ROOT, capture_configuration, fingerprint

NSIGHT = Path('C:/Program Files/NVIDIA Corporation/Nsight Graphics 2026.3.1/host/windows-desktop-nomad-x64/ngfx.exe')
QUERY_EXTENSIONS = ('VK_KHR_acceleration_structure', 'VK_KHR_ray_query')


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
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True, help='Frozen view glTF written by capture_hybrid_gi.py')
    parser.add_argument('--camera', choices=CAMERAS, required=True)
    parser.add_argument('--backend', choices=('compute', 'hardware'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--nsight', type=Path, default=NSIGHT)
    parser.add_argument('--warmup', type=int, default=100, help='Presented frames before tracing starts')
    parser.add_argument('--frames', type=int, default=30, help='Traced frames')
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=540)
    parser.add_argument('--clocks', choices=('base', 'boost', 'unaltered'), default='base')
    # Hardware events overflow beyond about ten Compute frames, and their ranges sometimes bill
    # HybridTrace's dispatch to HybridTemporal; API timestamps attribute every frame correctly.
    parser.add_argument('--hardware-events', action='store_true',
                        help='Time compute ranges with the hardware event system instead of API timestamps')
    parser.add_argument('--hardware-events-kb', type=int, default=10000, help='Hardware-event storage, 500 to 10000 KiB')
    parser.add_argument('--multi-pass-metrics', action='store_true',
                        help='Collect additional counters over repeated frames for the Trace Analysis advice')
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    args = parser.parse_args()
    assert args.frames > 0 and args.warmup > 0 and min(args.width, args.height) > 0
    assert all('=' in item and '\n' not in item for item in args.set)
    scene = args.scene.resolve()
    check_frozen_view(scene, args.camera)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = ROOT / 'Data/engine.cfg'
    original = config.read_bytes()
    workload = BASELINE | {'default_model_path': scene.as_posix()}
    workload.update(item.split('=', 1) for item in args.set)
    workload['voxel_gi_backend'] = args.backend
    settings = capture_configuration(original, workload)
    (out / 'engine.cfg').write_bytes(settings)
    demo = ['--mode=3', '--fixed-step', '--no-ui', '--vsync=0', '--disable-validation', '--gpu-markers',
            '--gpu-memory-stats', '--rhi-thread=1', '--async-compute=1', f'--frames={args.warmup + args.frames + 60}',
            f'--width={args.width}', f'--height={args.height}', f'--frame-times={out / "cpu-frames.csv"}']
    if args.backend == 'compute':
        demo.append('--disable-rt')
    command = [str(args.nsight), '--activity', 'GPU Trace Profiler', '--exe', str(args.exe.resolve()),
               '--dir', str(ROOT), '--args', subprocess.list2cmdline(demo), '--env', 'DISABLE_RTSS_LAYER=1;',
               '--start-after-frames', str(args.warmup), '--limit-to-frames', str(args.frames),
               '--max-duration-ms', '9000', '--set-gpu-clocks', args.clocks,
               '--hes-enabled', '1' if args.hardware_events else '0',
               '--allocated-hes-buffer-memory-kb', str(args.hardware_events_kb), '--auto-export',
               '--output-dir', str(out), '--verbose']
    if args.multi_pass_metrics:
        command.append('--multi-pass-metrics')
    (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    inputs = dict(executable=fingerprint(args.exe.resolve()), nsight=fingerprint(args.nsight), scene=fingerprint(scene),
                  shaders=[fingerprint(p) for p in sorted((ROOT / 'Data/SpvShaders').rglob('*.spv'))])
    (out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    try:
        config.write_bytes(settings)
        with (out / 'launch.log').open('w') as log:
            process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=600)
    finally:
        if config.read_bytes() != settings:
            raise RuntimeError('External engine.cfg edit; leaving it untouched')
        config.write_bytes(original)
    log = (out / 'launch.log').read_text(errors='replace')
    errors = [line for line in log.splitlines() if any(token in line for token in (
        '[error]', 'VUID-', 'SYNC-HAZARD', 'TARGET ERROR:', 'Using GI defaults after rejected configuration'))]
    assert process.returncode == 0 and not errors, (process.returncode, errors[:5])
    traces = list(out.glob('*.ngfx-gputrace'))
    assert traces, 'Nsight produced no trace; inspect launch.log'
    queries = [extension for extension in QUERY_EXTENSIONS if f'Enabled Device Extension: {extension}' in log]
    assert queries == (list(QUERY_EXTENSIONS) if args.backend == 'hardware' else []), queries
    report = summarize(out, args.frames)
    report.update(backend=args.backend, camera=args.camera, clocks=args.clocks, warmup=args.warmup,
                  width=args.width, height=args.height, hardware_events=args.hardware_events,
                  multi_pass_metrics=args.multi_pass_metrics, trace=traces[0].name, settings=workload,
                  configuration_sha256=hashlib.sha256(settings).hexdigest())
    (out / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{out.name}: median {report["median_ms"]:.3f} ms, p95 {report["p95_ms"]:.3f} ms over {args.frames} frames',
          flush=True)
    for name, entry in list(report['passes'].items())[:15]:
        missing = f'  ({entry["missing_frames"]} frames unrecorded)' if entry['missing_frames'] else ''
        print(f'  {entry["median_ms"]:8.3f} ms  {name}{missing}', flush=True)


if __name__ == '__main__':
    main()
