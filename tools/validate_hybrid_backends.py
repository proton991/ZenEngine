"""P2/P3/P4 image checks for each explicit feature selector, in both directions.

At both frozen Sponza cameras, test all Compute, all HW RT, each feature alone
on HW RT, and each feature alone on Compute. Sky must meet the existing shipping
limits against its selected provider's converged reference. Specular visibility
must agree with that provider's endpoint (2% absolute RMS and P99); changing other
providers must not substitute their visibility. Check linear composition too.
This supplements the archived motion/reset gates; it is not a P8 qualification.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import compare, load
from hybrid_capture_io import read_bytes, compact
from validate_hybrid_acceptance import capture, validate_capture
from validate_hybrid_gi import run as fixture_capture

FEATURES = ('sky', 'specular', 'sky_cache', 'bounce', 'reflections', 'shadows')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compute-reference-root', type=Path, required=True)
    parser.add_argument('--hardware-reference-root', type=Path, required=True)
    parser.add_argument('--reference-shaders', type=Path, help='Verified archived shader binaries for the canonical references')
    parser.add_argument('--reference-exe', type=Path, help='Matching archived executable for the canonical references')
    parser.add_argument('--allow-reference-present-baseline', action='store_true',
                        help='Allow the recorded legacy presentation hazard only in reused archived references')
    parser.add_argument('--cameras', nargs='+', choices=('top', 'hall'), default=['top', 'hall'])
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--phases', nargs='+', choices=('static', 'switching', 'cache'), default=['static', 'switching', 'cache'])
    parser.add_argument('--reuse-captures', action='store_true')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    args.exe, args.scene = args.exe.resolve(), args.scene.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    configurations = [('compute', (), 0), ('hardware', (), 63)]
    for base, other, mask in (('compute', 'hardware', 0), ('hardware', 'compute', 63)):
        configurations += [(base, (f'{feature}={other}',), mask ^ (1 << bit))
                           for bit, feature in enumerate(FEATURES)]
    expected_cases = len(args.cameras) * ((14 if 'static' in args.phases else 0) + (15 if 'switching' in args.phases else 0)) + ('cache' in args.phases)
    report = dict(scope=__doc__, expected_cases=expected_cases, cases={}, complete=False, passed=False)
    cameras = args.cameras if 'static' in args.phases or 'switching' in args.phases else ()
    for camera in cameras:
        endpoints = {}
        hardware_reference = args.hardware_reference_root / f'papermill-{camera}-reference'
        compute_reference = args.compute_reference_root / f'papermill-{camera}-reference'
        for provider, reference in (('hardware', hardware_reference), ('voxel', compute_reference)):
            archived_reference = Path(str(reference) + '.inputs.json').exists()
            if args.capture and not archived_reference:
                capture(args, reference, 'papermill', camera, provider, reference=True, compact=True)
            reference_args = argparse.Namespace(**vars(args))
            reference_args.allow_present_baseline = (args.allow_present_baseline
                or (archived_reference and args.allow_reference_present_baseline))
            if archived_reference and args.reference_exe:
                reference_args.exe = args.reference_exe.resolve()
            validate_capture(reference_args, reference, 'papermill', camera, provider, True,
                             shader_root=args.reference_shaders if archived_reference else None)
        for backend, overrides, mask in configurations if 'static' in args.phases else ():
            name = f'{camera}-{mask:02d}'
            prefix = args.output / name
            provider = 'hardware' if backend == 'hardware' else 'voxel'
            if args.capture and not (args.reuse_captures and Path(str(prefix) + '.inputs.json').exists()):
                capture(args, prefix, 'papermill', camera, provider, compact=True,
                        backend=backend, feature_backends=overrides)
            evidence = validate_capture(args, prefix, 'papermill', camera, provider, feature_mask=mask)
            metadata, data = load(prefix)
            valid = data[:, :, 8, 3] > 0
            visibility = data[:, :, 5, 3][valid]
            if mask in (0, 63):
                endpoints[mask] = visibility.copy()
            expected_visibility = endpoints[63 if mask & 2 else 0]
            error = visibility - expected_visibility
            visibility_check = dict(rms=float(np.sqrt(np.mean(error ** 2))),
                                    p99=float(np.quantile(abs(error), .99)),
                                    maximum=float(abs(error).max()))
            visibility_check['passed'] = visibility_check['rms'] <= .02 and visibility_check['p99'] <= .02
            reference = hardware_reference if mask & 1 else compute_reference
            sky = compare(prefix, reference)
            lighting = np.frombuffer(read_bytes(str(prefix) + '.lighting.bin'), '<f4').reshape(540, 960, 7, 4)
            composition_error = float(abs(lighting[:, :, 0, :3] - lighting[:, :, 1:5, :3].sum(2)).max())
            diffuse_split_error = float(abs(lighting[:, :, 2, :3] - lighting[:, :, 5:7, :3].sum(2)).max())
            composition_passed = bool(np.isfinite(lighting).all() and composition_error < 1e-5
                                      and diffuse_split_error < 1e-5)
            passed = all(r['passed'] for r in sky.values()) and visibility_check['passed'] and composition_passed
            report['cases'][name] = dict(mask=mask, backend=backend, overrides=overrides, sky=sky,
                specular_visibility=visibility_check, composition_error=composition_error,
                diffuse_split_error=diffuse_split_error, evidence=evidence, passed=passed)
            report['complete'] = len(report['cases']) == report['expected_cases']
            report['passed'] = report['complete'] and all(v['passed'] for v in report['cases'].values())
            (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            print(name, 'PASS' if passed else 'FAIL', flush=True)
            if not passed:
                return 1
        if 'switching' in args.phases:
            prefix = args.output / f'{camera}-switching'
            if args.capture and not (args.reuse_captures and Path(str(prefix) + '.inputs.json').exists()):
                capture(args, prefix, 'papermill', camera, 'hardware', compact=True,
                        backend='hardware', feature_switching=True)
            evidence = validate_capture(args, prefix, 'papermill', camera, 'hardware', feature_mask=63)
            masks = (0, 1, 2, 4, 8, 16, 32, 63, 62, 61, 59, 55, 47, 31, 0)
            for stage, mask in enumerate(masks):
                first_metadata, first = load(Path(f'{prefix}-switch-{stage}-frame1'))
                settled = Path(f'{prefix}-switch-{stage}-frame32')
                metadata, data = load(settled)
                valid = first[:, :, 8, 3] > 0
                reset = (first_metadata['reset_reason'] == 'provider_changed'
                         and bool(np.all(first[:, :, 7, 0][valid] == 1)))
                routing = (first_metadata['hardware_feature_mask'] == mask
                           and metadata['hardware_feature_mask'] == mask)
                finite = bool(np.isfinite(first).all() and np.isfinite(data).all())
                reference = hardware_reference if mask & 1 else compute_reference
                sky = compare(settled, reference)
                passed = reset and routing and finite and all(r['passed'] for r in sky.values())
                name = f'{camera}-switch-{stage:02d}'
                report['cases'][name] = dict(mask=mask, reset=reset, routing=routing, finite=finite,
                                             sky=sky, evidence=evidence, passed=passed)
                report['complete'] = len(report['cases']) == expected_cases
                report['passed'] = report['complete'] and all(v['passed'] for v in report['cases'].values())
                (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
                print(name, 'PASS' if passed else 'FAIL', flush=True)
                if not passed:
                    return 1
    if 'cache' in args.phases:
        values = {}
        for provider, mask in (('voxel', 0), ('hardware', 4)):
            folder = args.output / 'cache' / provider
            prefix = folder / 'half_wall'
            if args.capture and not (args.reuse_captures and Path(str(prefix) + '.inputs.json').exists()):
                # Keep cone bounce: the global backend selector also selects ray-hit bounce.
                # Inherit Compute and override only the cache whose contribution is measured.
                fixture_capture(args.exe, folder, ['half_wall'], provider='voxel', rt=provider == 'hardware',
                    indirect=1, feature_backends={'sky_cache': 'hardware' if mask else 'compute'},
                    allow_present_baseline=args.allow_present_baseline)
                compact(prefix)
            metadata, data = load(prefix)
            assert metadata['hardware_feature_mask'] == mask and metadata['bounce_source'] == 'cone'
            near = (data[:, :, 8, 3] > 0) & (data[:, :, 10, 1] > .99) & (abs(data[:, :, 8, 0]) < .15)
            assert near.sum() > 1000
            values[provider] = float(data[:, :, 3, :3][near].mean())
        ratio = values['hardware'] / max(values['voxel'], 1e-20)
        passed = values['voxel'] > 0 and .97 <= ratio <= 1.10
        report['cases']['sky-cache-isolation'] = dict(means=values, ratio=ratio, passed=passed)
        report['complete'] = len(report['cases']) == expected_cases
        report['passed'] = report['complete'] and all(v['passed'] for v in report['cases'].values())
        (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print('sky-cache-isolation', 'PASS' if passed else 'FAIL', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
