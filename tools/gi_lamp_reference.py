"""Publish a versioned imported-lamps + sun diagnostic dataset for ReSTIR Phase 0.

Uses the existing independent CUDA path estimator on a deterministic uniform subset of receivers.
Run with build/venv/Scripts/python.exe; capture/compare the result with ground_truth_workflow.py.
The sparse truth is diagnostic: it is not a full-image accuracy measurement.
"""
import argparse
import copy
from pathlib import Path
import stat
import tempfile
import time

import numpy as np

from ground_truth_workflow import (ROOT, TOOLS, digest, digests, input_files, read_json, write_json,
                                   verify, validate_camera, noise_rms)


def build(args):
    import mitsuba as mi
    mi.set_variant('cuda_ad_rgb')
    import drjit as dr
    from ground_truth_reference import Reference, Materials, bounce, direct

    target = args.dataset.resolve()
    if target.exists() or target.is_relative_to(ROOT/'build'):
        raise ValueError('Choose a new immutable dataset version outside build/')
    setup = copy.deepcopy(read_json(args.template))
    setup['rendering']['lights'] = read_json(args.lights)['rendering']['lights']
    setup['rendering']['environment']['lighting'] = False
    setup['rendering']['normalMaps'] = False
    setup['animationEnabled'] = False
    setup['rendering']['gi']['cone'].update(rayProvider=2, featureProviders=[2]*6, gather=0,
                                         emissiveLighting=False, previousFrameRadiance=False)
    target.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=target.name+'.building-', dir=target.parent))
    frame = staging/'frames'/'imported_lamps_sun'
    frame.mkdir(parents=True)
    started = time.perf_counter()
    reference = Reference(mi, setup['sourcePath'])
    validation = validate_camera(reference, setup, indoor=True, floor=True)
    if not validation['passed']:
        raise ValueError(f'Invalid camera: {validation}')
    reference.materials = Materials(mi, dr, reference.source, reference.primitives)
    reference.configure(setup)
    valid, points, geometric, normal, primitive, interaction = reference.primary(setup, return_interaction=True)
    indices = np.sort(np.random.default_rng(args.seed).choice(np.flatnonzero(valid), args.receivers, replace=False))
    p, g, n = (value[indices] for value in (points, geometric, normal))
    value, halves = bounce(mi, dr, reference.scene, reference.materials, reference.radiance, p, g, n,
                           reference.lights, args.samples, args.batch, args.seed, None)
    d = np.array(direct(mi, dr, reference.scene, mi.Point3f(*p.T), mi.Normal3f(*g.T), mi.Normal3f(*n.T),
                       reference.lights, mi.Bool(True))).T
    albedo = np.array(dr.select(interaction.is_valid(), reference.materials.eval(
        interaction, interaction.is_valid(), albedo_only=True), 0)).T
    selected = np.zeros_like(valid)
    selected[indices] = True
    truth = dict(valid=selected, position=np.where(valid[:, None], points, 0), geometric=geometric,
                 normal=normal, primitive=primitive, albedo=albedo)
    for term in ('D_direct', 'D_sky', 'D_bounce'):
        truth[term] = np.zeros_like(points, dtype=np.float32)
    truth['D_direct'][indices] = d
    truth['D_bounce'][indices] = value
    truth['D_sky_halves'] = np.zeros((2, *points.shape), np.float32)
    truth['D_bounce_halves'] = np.zeros((2, *points.shape), np.float32)
    truth['D_bounce_halves'][:, indices] = halves
    h, w = setup['run']['renderHeight'], setup['run']['renderWidth']
    truth = {key: data.reshape((2, h, w, 3) if key.endswith('_halves') else
                              (h, w, 3) if data.ndim == 2 else (h, w)) for key, data in truth.items()}
    if any(not np.isfinite(data).all() for data in truth.values()):
        raise ValueError('Non-finite reference')
    write_json(frame/'scene.zenrender', setup)
    np.savez_compressed(frame/'truth.npz', **truth)
    # Batch convergence is a full-image check; sparse reference RMS is not a suitable threshold.
    noise = dict(D_direct=1.0, D_sky=1.0, D_bounce=1.0)
    write_json(frame/'frame.json', dict(setup_sha256=digest(frame/'scene.zenrender'), validation=validation,
                                       samples=args.samples, receivers=args.receivers, seed=args.seed,
                                       reference_noise_rms=noise_rms(truth), noise_rms=noise))
    inputs = input_files(setup) | {args.template.resolve(), args.lights.resolve(),
                                  Path(setup['rendering']['environment']['texturePath'])}
    manifest = dict(version=1, frames=[dict(id=frame.name, noise_rms=noise)],
                    inputs={str(p): sha for p, sha in digests(inputs).items()},
                    tools={name: digest(ROOT/'tools'/name) for name in (*TOOLS, Path(__file__).name)},
                    mitsuba=mi.__version__, variant=mi.variant(), resolution=[w, h],
                    seconds=time.perf_counter()-started, diagnostic='uniform sparse imported lamp receivers')
    manifest['files'] = {p.relative_to(staging).as_posix(): digest(p) for p in staging.rglob('*') if p.is_file()}
    write_json(staging/'dataset.json', manifest)
    verify(staging)
    staging.rename(target)
    for path in target.rglob('*'):
        if path.is_file():
            path.chmod(stat.S_IREAD)
    print(f'Published {target}: {args.receivers} receivers x {args.samples} samples', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--template', type=Path, required=True, help='Validated windowed ground-truth camera setup')
    parser.add_argument('--lights', type=Path, required=True, help='Setup containing the imported lights')
    parser.add_argument('--dataset', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=262144)
    parser.add_argument('--receivers', type=int, default=512)
    parser.add_argument('--batch', type=int, default=1024)
    parser.add_argument('--seed', type=int, default=2519)
    args = parser.parse_args()
    if args.batch <= 0 or args.samples < 2*args.batch or args.samples % (2*args.batch) or args.receivers < 1:
        parser.error('Positive receiver/batch counts and samples divisible by twice batch required')
    build(args)
