"""Target the largest distant sky disagreements with an independent Embree MIS tracer.

This diagnostic deliberately selects difficult pixels, not an unbiased image sample.
It reports separate estimates at the ground truth's primary hits and the engine's
receivers. The latter uses the hardware shader's depth error and float-ULP offset.
Two independently seeded estimates expose remaining sampling uncertainty.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import load
from environment_reference import Environment, load_gltf, sky_reference
from ground_truth_mitsuba import depth_quantization_error, engine_origins


def run(args):
    metadata, data = load(args.capture)
    truth = np.load(args.truth)
    position = data[:, :, 8, :3]
    tolerance = 4*depth_quantization_error(metadata, position) + 1e-6*(1+np.abs(position).max(-1))
    valid = ((data[:, :, 8, 3] > 0) & (truth['position'][..., 3] > 0)
             & (np.linalg.norm(truth['position'][..., :3]-position, axis=-1) <= tolerance)
             & (np.linalg.norm(position-np.array(metadata['camera_position']), axis=-1) >= args.far))
    error = np.mean((data[:, :, 1, :3]-truth['sky'])**2, axis=-1)
    candidates = np.flatnonzero(valid)
    picks = candidates[np.argsort(error.ravel()[candidates])[-args.count:][::-1]]
    pixels = np.array(np.unravel_index(picks, valid.shape)).T
    size = metadata['environment_cube_size']
    cube = np.fromfile(str(args.capture)+'.environment.bin', '<f4').reshape(6, size, size, 4)
    e = metadata['environment_intensity_rotation_enabled_visible']
    environment = Environment(cube=cube, rotation=e[1], orientation=metadata['environment_orientation'], intensity=e[0]*e[2])
    oracle, _ = load_gltf(args.scene or Path(str(args.capture)+'.gltf'))
    origins = engine_origins(metadata, data, policy=args.origin_policy)
    estimates = np.zeros((2, 2, len(pixels), 3))
    for index, (y, x) in enumerate(pixels):
        p, ns, ng = truth['position'][y, x, :3], truth['normal'][y, x], truth['geometric'][y, x]
        truth_origin = p+ng*4e-6*(1+np.abs(p).max())
        for half in range(2):
            for receiver, (point, normal, geometric, origin) in enumerate((
                    (p, ns, ng, truth_origin),
                    (position[y, x], data[y, x, 9, :3], data[y, x, 10, :3], origins[y, x]))):
                estimates[receiver, half, index], _ = sky_reference(
                    oracle, point, normal, geometric, environment, args.samples, args.seed+half*104729+index, origin)
        if index % 16 == 0:
            print(f'{index+1}/{len(pixels)} receivers', flush=True)
    y, x = pixels.T
    engine, mitsuba = data[y, x, 1, :3], truth['sky'][y, x]
    scale = max(float(truth['sky'][valid].mean()), 1e-20)
    report = dict(capture=str(args.capture), truth=str(args.truth), origin_policy=args.origin_policy,
                  samples_per_proposal_per_half=args.samples,
                  pixels=len(pixels), selection='largest RGB squared disagreement beyond distance threshold',
                  far=args.far, normalization='mean RGB over eligible far receivers', mean=scale,
                  engine_mitsuba_rms=float(np.sqrt(np.mean((engine-mitsuba)**2))/scale))
    for receiver, name in enumerate(('truth_receivers', 'engine_receivers')):
        mean = estimates[receiver].mean(0)
        noise = np.mean(((estimates[receiver, 0]-estimates[receiver, 1])/2)**2)
        report[name] = dict(noise_rms=float(np.sqrt(noise)/scale))
        for label, target in (('engine', engine), ('mitsuba', mitsuba)):
            delta = mean-target
            report[name][label] = dict(bias=float(delta.mean()/scale), rms=float(np.sqrt(np.mean(delta**2))/scale))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(str(args.output)+'.npz', pixels=pixels[:, ::-1], estimates=estimates, engine=engine, mitsuba=mitsuba,
                        origins=origins[y, x], truth_positions=truth['position'][y, x], engine_positions=position[y, x])
    Path(str(args.output)+'.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', type=Path, required=True)
    parser.add_argument('--truth', type=Path, required=True)
    parser.add_argument('--scene', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--count', type=int, default=256)
    parser.add_argument('--samples', type=int, default=32768)
    parser.add_argument('--seed', type=int, default=71)
    parser.add_argument('--far', type=float, default=.3)
    parser.add_argument('--origin-policy', choices=('auto', 'depth', 'plane', 'truth-offset'), default='auto',
                        help='Explicit diagnostic origin contract; plane and truth-offset are receiver probes')
    arguments = parser.parse_args()
    if arguments.count <= 0 or arguments.samples <= 0:
        parser.error('count and samples must be positive')
    run(arguments)
