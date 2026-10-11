"""Check all frozen compute environments against their 1024-ray voxel reference.

The reference accumulates 1024 rays over 64 frames (65,536 rays/pixel).
The candidate defaults to 4096 rays over 32 frames (131,072 rays/pixel).
Neither uses spatial filtering or reprojection. Shipping/error budgets are unchanged.
Run after the voxel static sweep, serially with other native capture tools.
"""
import argparse
import json
from pathlib import Path
import time

from compare_hybrid_gi import compare
from ground_truth_sweep import CAMERAS, ENVIRONMENTS, TIER_BIAS, TIER_EXCESS
from validate_hybrid_acceptance import capture, validate_capture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reference-root', type=Path, required=True,
                        help='Directory containing <environment>-<camera>-reference from the voxel static sweep')
    parser.add_argument('--environments', nargs='+', choices=ENVIRONMENTS, default=list(ENVIRONMENTS))
    parser.add_argument('--cameras', nargs='+', choices=CAMERAS, default=list(CAMERAS))
    parser.add_argument('--frames', type=int, choices=(32, 64), default=32,
                        help='Higher-sample accumulation; the frozen 1024-ray reference always uses 64 frames')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    args.exe, args.scene = args.exe.resolve(), args.scene.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(samples_per_frame=[4096, 1024], frames=[args.frames, 64],
                  total_rays_per_pixel=[4096 * args.frames, 65536], views={})
    for environment in args.environments:
        for camera in args.cameras:
            name = f'{environment}-{camera}'
            reference = args.reference_root / f'{name}-reference'
            evidence = [validate_capture(args, reference, environment, camera, 'voxel', True)]
            prefix = args.output / name
            if args.capture:
                capture(args, prefix, environment, camera, 'voxel', reference=True, reference_samples=4096, frames=args.frames)
            evidence.append(validate_capture(args, prefix, environment, camera, 'voxel', True,
                                             reference_samples=4096, frames=args.frames))
            regions = compare(prefix, reference)
            for region in regions.values():
                # The converged tier gate is 1% mean bias / 3% RMS, not the shipping noise gate.
                region['passed'] = abs(region['bias']) <= TIER_BIAS and region['rms'] <= TIER_EXCESS
            item = dict(regions=regions, evidence=evidence, passed=all(r['passed'] for r in regions.values()))
            report['views'][name] = item
            report['complete'] = len(report['views']) == len(args.environments) * len(args.cameras)
            report['passed'] = report['complete'] and all(v['passed'] for v in report['views'].values())
            (args.output / 'convergence-results.json').write_text(json.dumps(report, indent=2) + '\n')
            print(time.strftime('%H:%M:%S'), name, 'PASS' if item['passed'] else 'FAIL', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
