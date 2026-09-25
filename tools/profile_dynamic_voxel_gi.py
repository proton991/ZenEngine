"""Capture release-build GPU timings with Nsight Graphics; owns engine.cfg serially."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
NSIGHT = Path('C:/Program Files/NVIDIA Corporation/Nsight Graphics 2026.3.1/host/windows-desktop-nomad-x64')


def summarize(folder):
    base = folder / 'BASE'
    rows = list(csv.reader((base / 'FRAME.xls').open(), delimiter='\t'))
    assert len(rows) == 1 and rows[0][0] == 'GPU frame time'
    frames = [float(v) for v in rows[0][1:]]
    assert frames and all(v > 0 for v in frames)
    ordered = sorted(frames)
    passes = {}
    for row in csv.reader((base / 'D3DPERF_EVENTS.xls').open(), delimiter='\t'):
        if row[0] != 'event_text':
            values = [float(v) if v else 0 for v in row[1:]]
            assert len(values) == len(frames), (row[0], len(values), len(frames))
            entry = passes.setdefault(row[0], dict(occurrences=0, samples_ms=[0.0] * len(frames)))
            entry['occurrences'] += 1
            entry['samples_ms'] = [a + b for a, b in zip(entry['samples_ms'], values)]
    for entry in passes.values():
        entry.update(mean_ms=statistics.mean(entry['samples_ms']), median_ms=statistics.median(entry['samples_ms']))
    repro = dict(row[:2] for row in csv.reader((base / 'REPRO_INFO.xls').open(), delimiter='\t') if len(row) >= 2)
    return dict(frames=len(frames), median_ms=statistics.median(frames), mean_ms=statistics.mean(frames),
        p95_ms=ordered[int(.95 * (len(ordered) - 1))], max_ms=max(frames), samples_ms=frames,
        passes=passes, capture_info=repro)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=ROOT/'build/x64-windows-msvc-release/bin/scene_renderer_demo.exe')
    parser.add_argument('--nsight', type=Path, default=NSIGHT/'ngfx.exe')
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    parser.add_argument('--warmup', type=int, default=40)
    parser.add_argument('--frames', type=int, default=60)
    parser.add_argument('--cold', action='store_true',
        help='Request tracing from the first submit; inspect initial passes before accepting cold-start timings')
    parser.add_argument('--gi-start-frame', type=int, default=0,
        help='Run PBR first, then switch to GI inside the capture to retain initialization work')
    parser.add_argument('--motion-fixture', action='store_true')
    parser.add_argument('--hardware-events-kb', type=int, default=2000,
        help='Nsight hardware event storage, 500 to 10000 KiB')
    parser.add_argument('--disable-hardware-events', action='store_true',
        help='Use API timestamps for cold captures that exceed hardware-event storage')
    parser.add_argument('--thread', type=int, choices=(0, 1), default=0)
    parser.add_argument('--async-compute', type=int, choices=(0, 1), default=0)
    parser.add_argument('--gbuffer', type=int, default=2048)
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    args = parser.parse_args()
    assert args.frames > 0 and args.warmup >= 0 and args.gi_start_frame >= 0
    assert args.gi_start_frame == 0 or (not args.cold and args.warmup <= args.gi_start_frame < args.warmup+args.frames)
    assert all('=' in item and '\n' not in item for item in args.set)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    settings = args.config.read_bytes() + b'\n' + ('\n'.join(args.set) + '\n').encode()
    (out/'engine.cfg').write_bytes(settings)
    (out/'config.original.sha256').write_text(hashlib.sha256(original).hexdigest())
    demo_arguments = ['--mode=3', f'--frames={max(240, args.warmup+args.frames+80)}',
        '--disable-rt', '--disable-validation', '--gpu-markers', '--gpu-memory-stats', '--fixed-step',
        f'--gi-start-frame={args.gi_start_frame}', f'--rhi-thread={args.thread}',
        f'--async-compute={args.async_compute}', f'--gbuffer-size={args.gbuffer}',
        f'--width={args.width}', f'--height={args.height}', f'--frame-times={out / "cpu-frames.csv"}']
    if args.motion_fixture:
        demo_arguments.append('--gi-motion-fixture')
    arguments = subprocess.list2cmdline(demo_arguments)
    command = [str(args.nsight), '--activity', 'GPU Trace Profiler', '--exe', str(args.executable.resolve()),
        '--dir', str(ROOT), '--args', arguments, '--env', 'DISABLE_RTSS_LAYER=1;',
        '--start-after-submits' if args.cold else '--start-after-frames', '0' if args.cold else str(args.warmup),
        '--limit-to-frames', str(args.frames), '--max-duration-ms', '9000', '--set-gpu-clocks', 'base',
        '--allocated-hes-buffer-memory-kb', str(args.hardware_events_kb),
        '--hes-enabled', '0' if args.disable_hardware_events else '1',
        '--auto-export', '--output-dir', str(out), '--verbose']
    (out/'command.json').write_text(json.dumps(command, indent=2))
    (out/'executable.sha256').write_text(hashlib.sha256(args.executable.read_bytes()).hexdigest())
    inputs = [args.nsight, Path(__file__), *sorted((ROOT/'Data/SpvShaders').rglob('*.spv'))]
    (out/'inputs.sha256.json').write_text(json.dumps({str(path.resolve()):
        hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}, indent=2))
    try:
        config.write_bytes(settings)
        with (out/'launch.log').open('w') as log:
            process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        log = (out/'launch.log').read_text(errors='replace')
        errors = [line for line in log.splitlines() if any(token in line for token in ('[error]', 'VUID-', 'SYNC-HAZARD', 'TARGET ERROR:'))]
        assert process.returncode == 0 and not errors, (process.returncode, errors[:5])
        assert 'Selected voxel GI method:' in log and list(out.glob('*.ngfx-gputrace'))
        assert all('Enabled Device Extension: '+extension not in log for extension in (
            'VK_KHR_acceleration_structure', 'VK_KHR_ray_query', 'VK_KHR_ray_tracing_pipeline'))
        report = summarize(out)
        assert report['frames'] == args.frames, (report['frames'], args.frames)
        selections = re.findall(r'Selected voxel GI method: (\w+); query backend: (\w+)', log)
        report['resolved_method'], report['query_backend'] = selections[-1]
        assert report['passes'], 'No GPU pass labels were exported'
        report['cold'] = args.cold
        report['gi_start_frame'] = args.gi_start_frame
        memory = re.findall(r'GPU memory VMA: peak_committed_bytes=(\d+) peak_device_local_bytes=(\d+) remaining_bytes=(\d+)', log)
        assert len(memory) == 1 and int(memory[0][2]) == 0, memory
        report['vma_peak_committed_bytes'], report['vma_peak_device_local_bytes'], report['vma_remaining_bytes'] = map(int, memory[0])
        if args.cold or args.gi_start_frame:
            report['initial_cache_clear_visible'] = 'GIStaticCacheClear' in report['passes']
            report['startup_note'] = ('Do not aggregate changing pass ranges without inspecting the raw trace; '
                'the export can repeat samples for absent ranges, and initial work may precede complete frames.')
        if args.gi_start_frame:
            assert report['initial_cache_clear_visible'], 'Delayed capture missed GI initialization'
            cpu = [float(row['cpu_frame_ms']) for row in csv.DictReader((out/'cpu-frames.csv').open())]
            report['cold_cpu_trigger_frame_ms'] = cpu[args.gi_start_frame]
        report['config_sha256'] = hashlib.sha256(settings).hexdigest()
        (out/'summary.json').write_text(json.dumps(report, indent=2))
        print(out.name, 'frames=', report['frames'], 'median_ms=', report['median_ms'],
              'p95_ms=', report['p95_ms'], 'pass_rows=', len(report['passes']), flush=True)
    finally:
        assert config.read_bytes() == settings, 'Config changed externally; preserving it'
        config.write_bytes(original)


if __name__ == '__main__':
    main()
