"""Measure warmed mode-3 throughput and NVIDIA GPU activity with optional serial engine.cfg ownership and exact restoration."""
import argparse
import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=1200)
    parser.add_argument('--warmup', type=int, default=120)
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--gbuffer', type=int, default=2048)
    parser.add_argument('--thread', type=int, choices=(0, 1), default=1)
    parser.add_argument('--async-compute', type=int, choices=(0, 1), default=1)
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--motion-fixture', action='store_true')
    parser.add_argument('--config', type=Path)
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    args = parser.parse_args()
    if min(args.frames, args.warmup, args.width, args.height, args.gbuffer) <= 0:
        parser.error('Frame counts and dimensions must be positive')
    assert all('=' in item and '\n' not in item for item in args.set)
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    settings = args.config.read_bytes() if args.config else original
    if args.set:
        settings += b'\n' + ('\n'.join(args.set)+'\n').encode()
    try:
        if settings != original:
            config.write_bytes(settings)
        measure(args)
    finally:
        assert config.read_bytes() == settings, 'Configuration changed externally; preserving it'
        if settings != original:
            config.write_bytes(original)


def measure(args):
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    executable = args.executable.resolve()
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    (out/'engine.cfg').write_bytes(original)
    command = [str(executable), '--mode=3', '--fixed-step', '--disable-rt', '--gpu-memory-stats', f'--warmup={args.warmup}',
               f'--frames={args.frames}', f'--width={args.width}', f'--height={args.height}',
               f'--gbuffer-size={args.gbuffer}', f'--rhi-thread={args.thread}',
               f'--async-compute={args.async_compute}', f'--frame-times={out / "frames.csv"}']
    if not args.validation:
        command.append('--disable-validation')
    if args.motion_fixture:
        command.append('--gi-motion-fixture')
    env = os.environ.copy()
    env.update(DISABLE_RTSS_LAYER='1', VK_LOADER_LAYERS_DISABLE='~implicit~')
    (out/'command.json').write_text(json.dumps(command, indent=2), encoding='utf-8')
    hardware = subprocess.run(['nvidia-smi', '--query-gpu=name,driver_version', '--format=csv'],
                              capture_output=True, text=True, check=True).stdout
    (out/'gpu-info.csv').write_text(hardware, encoding='utf-8')
    (out/'inputs.sha256.json').write_text(json.dumps({str(p):hashlib.sha256(p.read_bytes()).hexdigest()
        for p in [executable, *sorted((ROOT/'Data/SpvShaders').rglob('*.spv'))]}, indent=2), encoding='utf-8')
    with (out/'gpu.csv').open('w') as gpu, (out/'run.log').open('w') as log:
        monitor = subprocess.Popen(['nvidia-smi', '--query-gpu=timestamp,utilization.gpu,utilization.memory,clocks.gr,power.draw',
            '--format=csv,noheader,nounits', '-lms', '100'], stdout=gpu, stderr=subprocess.STDOUT)
        try:
            run = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, timeout=300)
        finally:
            monitor.terminate()
            monitor.wait()
    assert config.read_bytes() == original, 'Configuration changed during the benchmark'
    text = (out/'run.log').read_text(errors='replace')
    errors = [line for line in text.splitlines() if any(x in line for x in ('[error]', 'VUID-', 'SYNC-HAZARD'))]
    assert run.returncode == 0 and not errors, (run.returncode, errors[:5])
    frames = [float(row['cpu_frame_ms']) for row in csv.DictReader((out/'frames.csv').open())]
    assert len(frames) == args.frames and all(math.isfinite(value) and value > 0 for value in frames)
    times = re.findall(r'\[([^]]+)\] \[info\] Render threads:', text)
    assert len(times) == 2, times
    # NVML activity covers a preceding sample interval. Discard the first second
    # after warmup so startup/measurement boundaries cannot inflate utilization.
    start = datetime.datetime.fromisoformat(times[0]) + datetime.timedelta(seconds=1)
    end = datetime.datetime.fromisoformat(times[1])
    samples = []
    for row in csv.reader((out/'gpu.csv').open()):
        if len(row) == 5:
            timestamp = datetime.datetime.strptime(row[0].strip(), '%Y/%m/%d %H:%M:%S.%f')
            if start <= timestamp <= end:
                samples.append([float(value.strip()) for value in row[1:]])
    assert samples, 'No warmed NVIDIA activity samples; increase --frames'
    methods = re.findall(r'Selected voxel GI method: (\w+); query backend: (\w+)', text)
    ordered = sorted(frames)
    report = dict(frames=len(frames), warmup=args.warmup, fps=1000/statistics.mean(frames),
        mean_frame_ms=statistics.mean(frames), median_frame_ms=statistics.median(frames),
        p95_frame_ms=ordered[int(.95*(len(ordered)-1))],
        gpu_activity_percent_mean=statistics.mean(row[0] for row in samples),
        gpu_activity_percent_median=statistics.median(row[0] for row in samples),
        memory_activity_percent_mean=statistics.mean(row[1] for row in samples),
        graphics_clock_mhz_mean=statistics.mean(row[2] for row in samples),
        power_watts_mean=statistics.mean(row[3] for row in samples), gpu_samples=len(samples),
        resolved_method=methods[-1][0], query_backend=methods[-1][1],
        configuration_sha256=hashlib.sha256(original).hexdigest(),
        metric_note='CPU frame intervals include frame-slot backpressure; NVML GPU activity is not SM occupancy. '
                    'Animation advances 1/60 second per frame; image quality settings are unchanged.')
    memory = re.findall(r'GPU memory VMA: peak_committed_bytes=(\d+) peak_device_local_bytes=(\d+) remaining_bytes=(\d+)', text)
    assert len(memory) == 1 and int(memory[0][2]) == 0, memory
    report['vma_peak_committed_bytes'], report['vma_peak_device_local_bytes'], report['vma_remaining_bytes'] = map(int, memory[0])
    (out/'summary.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(out.name, json.dumps(report), flush=True)


if __name__ == '__main__':
    main()
