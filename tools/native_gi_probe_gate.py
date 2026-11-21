"""Measure NativeGIPlan Phase 0 through canonical ZenApp and the Nsight profiling workflow.

The extra dispatch does not replace GI. Its range measures cache-only probe hit shading;
whole-frame time still includes the old GI path and must not be reported as native cached GI.
The retained per-pixel path runs at half resolution to leave room for both RT pipelines.
Probe placement always reads the full 3840x2160 G-buffer, independently of that path.
"""
import argparse
import copy
import json
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace

from gi_stability import DEFAULT_EXE, ROOT, provenance, read_json, write_json, zenapp
from profile_hybrid_gi import actual_state, running_renderers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=2)
    parser.add_argument('--start-after-ms', type=int, default=60000)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error('--repeats must be positive')
    if running_renderers():
        parser.error('Close other Zen renderers before measurement')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    setup = copy.deepcopy(read_json(args.setup))
    setup['animationEnabled'] = False
    setup['rendering']['gi']['cone'].update(nativeProbeBenchmark=True, radianceCache=False,
                                           rayProvider=2, rayTracingPipeline=True,
                                           referenceSamples=0, referenceTileStride=1,
                                           pixelResolutionDivisor=2)
    setup_path = args.output / 'setup.zenrender'
    write_json(setup_path, setup)
    launch = SimpleNamespace(executable=DEFAULT_EXE, setup=args.setup)
    capture = args.output / 'validation.ppm'
    command = zenapp(launch, [f'--setup={setup_path}', '--backend=hardware', '--frames=64',
                              '--width=3840', '--height=2160', '--fullscreen=1', '--vsync=0',
                              '--rhi=threaded', '--async-compute=0', '--gpu-culling=1',
                              '--validation=1', '--fixed-step', '--no-ui', f'--capture={capture}'],
                     args.output / 'validation.log')
    write_json(args.output / 'validation-identity.json', provenance(launch, setup_path, [command]))
    log = (args.output / 'validation.log').read_text(errors='replace')
    for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
        if marker in log:
            raise RuntimeError(f'{marker}: see validation.log')
    state = read_json(str(capture) + '.json')
    if not state.get('nativeProbeBenchmark') or state.get('nativeProbeRaySlots') != 2592000:
        raise RuntimeError(f'Probe benchmark did not dispatch the requested ray slots: {state}')
    if not state.get('rayTracingPipeline') or not state.get('radianceCacheCapacity'):
        raise RuntimeError('Benchmark requires an active RT pipeline and allocated cache')
    summaries = []
    for repeat in range(args.repeats):
        folder = args.output / f'timing-{repeat}'
        subprocess.run([sys.executable, str(ROOT / 'tools/profile_hybrid_gi.py'),
                        '--setup', str(setup_path), '--backend', 'hardware', '--output', str(folder),
                        '--clocks', 'base', '--async-compute', '0', '--start-after-ms', str(args.start_after_ms)],
                       cwd=ROOT, check=True)
        summary = read_json(folder / 'summary.json')
        measured = summary['passes'].get('NativeProbeBenchmark')
        if not measured or measured['missing_frames']:
            raise RuntimeError('Benchmark range missing in the GPU trace')
        cache_sizes = re.findall(r'GI radiance cache: (\d+) entries',
                                (folder / 'launch.log').read_text(errors='replace'))
        if not cache_sizes or int(cache_sizes[-1]) == 0:
            raise RuntimeError('No active cache at the end of the timing launch')
        summaries.append(dict(path=str(folder), probe_ms=measured['median_ms'],
                              final_cache_entries=int(cache_sizes[-1]),
                              frame_ms=summary['median_ms'], actual=summary['actual']))
    report = dict(ray_slots=2592000, probe_slots=40500, rays_per_probe=64,
                  note='Sky receivers skip tracing; these are dispatch slots, not a measured traversal count.',
                  validation_actual=actual_state(log), runs=summaries,
                  cost_gate_ms=3.0, cost_gate_passed=all(run['probe_ms'] <= 3.0 for run in summaries),
                  quality_qualified=False)
    write_json(args.output / 'report.json', report)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
