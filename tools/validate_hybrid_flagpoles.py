"""Check the approved frozen flagpole regions against Mitsuba and the shipping tier reference.

This analyzes archived captures without launching the renderer. Independent captures use
vertex normals; shipping captures retain normal maps. The frozen rectangles are never
selected from the error image. The original source identity and truth hash are checked.
"""
import argparse
import hashlib
import json
from pathlib import Path

from capture_hybrid_gi import fingerprint
from compare_hybrid_gi import compare as compare_shipping, load
from ground_truth_mitsuba import compare as compare_truth
from ground_truth_sweep import check_identity, TIER_BIAS, TIER_EXCESS
from validate_hybrid_acceptance import SHIPPING_ENVIRONMENTS
from validate_hybrid_acceptance import capture, validate_capture
from report_hybrid_gi_flagpoles import RECTANGLES, flagpole_masks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--independent-root', type=Path, required=True, help='Ground-truth sweep directory containing gt/')
    parser.add_argument('--shipping-root', type=Path, required=True, help='Acceptance directory containing static/')
    parser.add_argument('--shipping-reference-root', type=Path, help='Reuse converged references from another acceptance directory')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--scene', type=Path)
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--reuse-captures', action='store_true')
    parser.add_argument('--reference-shaders', type=Path, help='Verify archived canonical captures after a mixed-only shader fix')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    dataset = json.loads((args.dataset / 'dataset.json').read_text())
    report = dict(rectangles_xyxy=RECTANGLES, receiver_mask='frozen floor: geometric_normal.y > .99 and position.y < -.12',
                  shipping_channel='diffuse', views={})
    for environment in SHIPPING_ENVIRONMENTS:
        view = f'{environment}-top'
        entry = dataset['views'][view]
        truth = args.dataset / entry['truth']
        if hashlib.sha256(truth.read_bytes()).hexdigest() != entry['truth_sha256']:
            raise ValueError(f'Changed truth: {truth}')
        independent = args.independent_root / 'gt' / f'{view}-reference'
        candidate = args.shipping_root / 'static' / view
        reference = (args.shipping_reference_root or args.shipping_root) / 'static' / f'{view}-reference'
        if args.capture:
            if not args.exe or not args.scene:
                parser.error('--capture requires --exe and --scene')
            args.exe, args.scene = args.exe.resolve(), args.scene.resolve()
            for prefix, ref, stripped in ((independent, True, True), (reference, True, False), (candidate, False, False)):
                if not (args.reuse_captures and Path(str(prefix) + '.inputs.json').exists()):
                    capture(args, prefix, environment, 'top', 'hardware', reference=ref,
                            sky_only=stripped, compact=True, strip_normal_maps=stripped)
        check_identity(view, entry, independent)
        if args.exe and args.scene:
            validate_capture(args, independent, environment, 'top', 'hardware', True,
                             sky_only=True, strip_normal_maps=True, shader_root=args.reference_shaders)
            validate_capture(args, reference, environment, 'top', 'hardware', True, shader_root=args.reference_shaders)
            validate_capture(args, candidate, environment, 'top', 'hardware', shader_root=args.reference_shaders)
        metadata, _ = load(candidate)
        if (metadata['width'], metadata['height']) != (960, 540):
            raise ValueError(f'Wrong frozen extent: {candidate}')
        _, independent_data = load(independent)
        masks = flagpole_masks(independent_data)
        converged = compare_truth(truth, independent, additional_regions=masks)
        shipping = compare_shipping(candidate, reference, masks, channel='diffuse')
        regions = {}
        for name in masks:
            exact = converged[name]
            exact['passed'] = (abs(exact['bias']) <= TIER_BIAS and exact['excess_rms'] <= TIER_EXCESS
                               and exact['blocks']['excess_rms'] <= TIER_EXCESS)
            regions[name] = dict(converged=exact, shipping=shipping[name],
                                 passed=exact['passed'] and shipping[name]['passed'])
        evidence = [fingerprint(truth)]
        for prefix in (independent, candidate, reference):
            evidence += [fingerprint(Path(str(prefix) + suffix)) for suffix in ('.inputs.json', '.hybrid.bin', '.lighting.json')]
        report['views'][view] = dict(regions=regions, evidence=evidence,
                                    passed=all(item['passed'] for item in regions.values()))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(view, 'PASS' if report['views'][view]['passed'] else 'FAIL', flush=True)
        if not report['views'][view]['passed']:
            return 1
    report['passed'] = all(item['passed'] for item in report['views'].values())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(passed=report['passed'], regions=15), indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
