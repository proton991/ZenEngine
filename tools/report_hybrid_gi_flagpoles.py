"""Measure and display the three flagpole-shadow crops in the frozen 960x540 top view.

Rectangles cover the visible poles at y=166, 320 and 477; receiver metrics include
only the frozen floor region. Each environment uses a shared display scale for
truth, reference and shipping. Metrics always use unscaled scene-linear RGB.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from compare_hybrid_gi import compare as compare_shipping, load
from ground_truth_mitsuba import compare as compare_truth
from ground_truth_sweep import ENVIRONMENTS, load_dataset


RECTANGLES = ((416, 130, 552, 207), (416, 285, 552, 362), (416, 437, 552, 514))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--environments', nargs='+', choices=list(ENVIRONMENTS), default=list(ENVIRONMENTS))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    dataset = load_dataset(args.dataset)
    report = {'rectangles_xyxy': RECTANGLES, 'views': {}}
    for environment in args.environments:
        view = environment + '-top'
        reference = args.captures / 'gt' / (view + '-reference')
        metadata, data = load(reference)
        assert (metadata['width'], metadata['height']) == (960, 540)
        floor = (data[:, :, 10, 1] > .99) & (data[:, :, 8, 1] < -.12)
        masks = {}
        for index, (x0, y0, x1, y1) in enumerate(RECTANGLES):
            mask = np.zeros(floor.shape, bool)
            mask[y0:y1, x0:x1] = True
            masks[f'pole_{index}'] = mask & floor
        truth_path = args.dataset / dataset['views'][view]['truth']
        truth = np.load(truth_path)
        converged = compare_truth(truth_path, reference, additional_regions=masks)
        shipping = compare_shipping(args.captures / 'ship' / view,
                                    args.captures / 'ship' / (view + '-reference'), masks)
        report['views'][view] = {name: {'converged': converged[name], 'shipping': shipping[name]}
                                 for name in masks}
        # Show all three pole locations and the same normal-map-free sky signal.
        _, candidate = load(args.captures / 'gt' / view)
        images = (truth['sky'], data[:, :, 1, :3], candidate[:, :, 1, :3])
        scale = max(float(np.quantile(truth['sky'][floor], .99)), 1e-20)
        canvas = Image.new('RGB', (3 * 272, 3 * 154 + 52), '#181818')
        draw = ImageDraw.Draw(canvas)
        draw.text((8, 4), environment + ' / top / vertex normals / shared scale', fill='white')
        for column, label in enumerate(('Mitsuba', '1024 x 64 reference', '4 rays / 64 frames')):
            draw.text((column * 272 + 8, 25), label, fill='white')
            for row, (x0, y0, x1, y1) in enumerate(RECTANGLES):
                rgb = np.clip(images[column][y0:y1, x0:x1] / scale, 0, 1) ** (1 / 2.2)
                crop = Image.fromarray(np.uint8(rgb * 255)).resize((272, 154), Image.Resampling.NEAREST)
                canvas.paste(crop, (column * 272, 52 + row * 154))
        canvas.save(args.output / (environment + '.png'))
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
