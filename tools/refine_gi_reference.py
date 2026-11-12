"""Extend only the matched views whose reference checkpoint drift needs refinement.

Keeps the initial reports and writes report-refined.json with the replacement metrics.
This is an accumulation extension with the same sampled pixels, materials and candidates.
"""
import argparse
import copy
from pathlib import Path

import numpy as np
from PIL import Image

from gi_matched_quality import average, capture, image, metrics
from gi_stability import digest, read_json, write_json


def main(args):
    args.output = args.dataset.resolve()
    identity = read_json(args.output / 'identity.json')
    args.executable = Path(identity['executable'])
    if digest(args.executable) != identity['executable_sha256']:
        raise ValueError('Reference extension requires the same executable')
    original = read_json(args.output / 'report.json')
    result = copy.deepcopy(original)
    selected = args.views or sorted({row['view'] for row in original['static']
                                     if not row['reference_drift_below_quarter_error']})
    for view in selected:
        folder = args.output / 'static' / view
        old_capture = read_json(folder / 'reference/capture.json')
        module = Path(next(value.split('=', 1)[1] for value in old_capture['arguments']
                           if value.startswith('--hybrid-trace-buffered-module=')))
        if digest(module) != identity['reference_module_sha256']:
            raise ValueError('Reference module changed')
        mask_path = module.parent / 'mask.png'
        if digest(mask_path) != identity['reference_mask_sha256']:
            raise ValueError('Reference sampling mask changed')
        mask = np.asarray(Image.open(mask_path)) > 0
        reference_folder = folder / f'reference-{args.frames}'
        early = list(range(args.frames - 128, args.frames - 111))
        late = list(range(args.frames - 16, args.frames + 1))
        setup = read_json(folder / 'reference/setup.zenrender')
        capture(args, reference_folder, setup, early[0], late[-1], keep=early + late, module=module)
        reference = average(reference_folder, late)
        drift = metrics(average(reference_folder, early), reference, mask)
        np.save(reference_folder / 'mean.npy', reference, allow_pickle=False)
        for row in result['static']:
            if row['view'] != view:
                continue
            resolution = row['preset']
            row['frame_256'] = metrics(image(folder / resolution / 'frame-256.png'), reference, mask)
            row['frames_240_256_mean'] = metrics(average(folder / resolution, range(240, 257)), reference, mask)
            row['reference_drift'] = drift
            row['reference_drift_below_quarter_error'] = drift['rms_8bit'] < row['frame_256']['rms_8bit'] / 4
            row['reference_extension'] = dict(folder=str(reference_folder), frames=args.frames,
                                               early_frames=early, final_frames=late)
            print(view, resolution, row['frame_256'], 'reference drift', drift['rms_8bit'],
                  'qualified', row['reference_drift_below_quarter_error'], flush=True)
        write_json(args.output / 'report-refined.json', result)
    result['refinement'] = dict(views=selected, frames=args.frames, tool_sha256=digest(__file__),
                                original_report_sha256=digest(args.output / 'report.json'))
    write_json(args.output / 'report-refined.json', result)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=512)
    parser.add_argument('--views', nargs='+')
    main(parser.parse_args())
