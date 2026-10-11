"""Check the approved frozen flagpole regions against Mitsuba and the shipping tier reference.

This analyzes archived captures without launching the renderer. Independent captures use
vertex normals; shipping captures retain normal maps. The frozen rectangles are never
selected from the error image. The original source identity and truth hash are checked.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from capture_hybrid_gi import fingerprint
from compare_hybrid_gi import compare as compare_shipping, load
from ground_truth_mitsuba import compare as compare_truth
from ground_truth_sweep import check_identity, TIER_BIAS, TIER_EXCESS
from validate_hybrid_acceptance import SHIPPING_ENVIRONMENTS

RECTANGLES = ((416, 130, 552, 207), (416, 285, 552, 362), (416, 437, 552, 514))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--independent-root', type=Path, required=True, help='Ground-truth sweep directory containing gt/')
    parser.add_argument('--shipping-root', type=Path, required=True, help='Acceptance directory containing static/')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    dataset = json.loads((args.dataset / 'dataset.json').read_text())
    report = dict(rectangles_xyxy=RECTANGLES, shipping_channel='diffuse', views={})
    for environment in SHIPPING_ENVIRONMENTS:
        view = f'{environment}-top'
        entry = dataset['views'][view]
        truth = args.dataset / entry['truth']
        if hashlib.sha256(truth.read_bytes()).hexdigest() != entry['truth_sha256']:
            raise ValueError(f'Changed truth: {truth}')
        independent = args.independent_root / 'gt' / f'{view}-reference'
        check_identity(view, entry, independent)
        candidate = args.shipping_root / 'static' / view
        reference = Path(str(candidate) + '-reference')
        metadata, _ = load(candidate)
        if (metadata['width'], metadata['height']) != (960, 540):
            raise ValueError(f'Wrong frozen extent: {candidate}')
        masks = {}
        for index, (x0, y0, x1, y1) in enumerate(RECTANGLES):
            mask = np.zeros((540, 960), dtype=bool)
            mask[y0:y1, x0:x1] = True
            masks[f'pole_{index}'] = mask
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
    report['passed'] = all(item['passed'] for item in report['views'].values())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(passed=report['passed'], regions=15), indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
