"""Generate matched one- and sixteen-vertex CUDA diffuse references for native GI.

Uses the current material diffuse reflectance and smooth hit normals. The fixture
must disable environment and emission, which this analytic-light estimator omits.
Stores independent sample halves so energy tolerances derive from reference noise.
"""
import argparse
from pathlib import Path
import time

import numpy as np
from gi_stability import ROOT, digest, read_json, write_json
from ground_truth_workflow import input_files, digests
from ground_truth_mitsuba import setup_mitsuba
from ground_truth_reference import Reference, Materials, bounce, direct
from profile_hybrid_gi import running_renderers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--receivers', type=int, default=4096)
    parser.add_argument('--samples', type=int, default=2048)
    parser.add_argument('--batch', type=int, default=32)
    parser.add_argument('--seed', type=int, default=81287)
    args = parser.parse_args()
    if running_renderers():
        parser.error('Close other Zen renderers before running CUDA')
    if args.receivers < 1 or args.samples < 2 or args.samples % 2 or args.batch < 1:
        parser.error('Need positive receivers/batch and a positive even sample count')
    args.setup, args.output = args.setup.resolve(), args.output.resolve()
    setup = read_json(args.setup)
    if setup['rendering']['environment']['lighting'] or setup['rendering']['gi']['cone']['emissiveLighting']:
        parser.error('Reference setup must disable environment and emissive lighting')
    args.output.mkdir(parents=True, exist_ok=False)
    inputs = {str(path): value for path, value in digests(input_files(setup) | {args.setup}).items()}
    mi = setup_mitsuba()
    import drjit as dr
    reference = Reference(mi, setup['sourcePath'])
    reference.materials = Materials(mi, dr, reference.source, reference.primitives)
    valid, points, geometric, normal, primitive = reference.primary(setup)
    available = np.flatnonzero(valid)
    indices = np.sort(np.random.default_rng(123791).choice(available, min(args.receivers, len(available)), replace=False))
    lights = [entry['light'] for entry in setup['rendering']['lights'] if entry['light']['enabled']]
    results = dict(indices=indices, position=points[indices], geometric=geometric[indices], normal=normal[indices])
    started = time.perf_counter()
    for depth in (1, 16):
        value, halves = bounce(mi, dr, reference.scene, reference.materials, None,
                               points[indices], geometric[indices], normal[indices], lights,
                               args.samples, args.batch, args.seed+depth, max_bounces=depth, sample_lights=False)
        results[f'bounce_{depth}'], results[f'halves_{depth}'] = value, halves
        print(depth, float(value.mean()), time.perf_counter()-started, flush=True)
    results['direct'] = np.array(direct(mi, dr, reference.scene, mi.Point3f(points[indices].T),
                                       mi.Normal3f(geometric[indices].T), mi.Normal3f(normal[indices].T),
                                       lights, mi.Bool(True))).T
    for path, expected in inputs.items():
        if digest(path) != expected:
            raise ValueError(f'Reference input changed: {path}')
    np.savez_compressed(args.output/'reference.npz', **results)
    write_json(args.output/'reference.json', dict(setup=str(args.setup), inputs=inputs, receivers=len(indices),
        samples=args.samples, max_bounces=[1, 16], seed=args.seed, receiver_seed=123791, variant=mi.variant(),
        mitsuba=mi.__version__, seconds=time.perf_counter()-started, tool_sha256=digest(__file__),
        estimator_sha256=digest(ROOT/'tools/ground_truth_reference.py'),
        contract='Smooth shading normals, material diffuse reflectance including the 0.96 dielectric factor; no emission or environment.'))


if __name__ == '__main__':
    main()
