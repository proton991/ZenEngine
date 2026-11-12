"""Repeat the spark protocol at the exact fullscreen quarter/half quality presets."""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from gi_matched_quality import capture, preset
from gi_spark_metric import camera_path, find_sparks, overlay, summarize
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json


def main(args):
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    source = read_json(args.setup)
    identity = provenance(args, args.setup.resolve(), [])
    identity['measurement_tool_sha256'] = digest(__file__)
    identity['metric_tool_sha256'] = digest(Path(__file__).with_name('gi_spark_metric.py'))
    write_json(args.output / 'identity.json', identity)
    reports = {}
    for resolution in ('quarter', 'half'):
        setup = preset(source, resolution)
        reports[resolution] = {}
        for camera in ('moving', 'static'):
            folder = args.output / resolution / camera
            path = camera_path(setup, camera, 128, 16, .5)
            capture(args, folder, setup, 129, 144, path=path)
            values = np.stack([np.asarray(Image.open(folder / f'frame-{frame}.png'), dtype=np.int16)
                               for frame in range(129, 145)])
            rows, pixels = find_sparks(values, 48)
            result = summarize(rows, pixels, 12)
            result.update(threshold_8bit=48, warmup=128, captured_frames=16, angle_degrees=.5)
            write_json(folder / 'report.json', result)
            np.savez_compressed(folder / 'spark-components.npz', components=rows)
            overlay(values, rows, folder / 'sparks.png')
            reports[resolution][camera] = result
            print(resolution, camera, result['per_frame_mean'], flush=True)
            del values
    write_json(args.output / 'report.json', reports)
    current = provenance(args, args.setup.resolve(), [])
    if any(identity[key] != value for key, value in current.items()):
        raise ValueError('Measurement inputs changed during the spark captures')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    main(parser.parse_args())
