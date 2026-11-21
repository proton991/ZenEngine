"""Compare native GI or cache-backed pixels with matched CUDA diffuse references.

The setup must have the reference camera, scene and lights. Receiver positions and
surface identities are checked before comparing linear irradiance/pi. A 3-sigma
energy tolerance comes from independent reference halves, not the candidate error.
"""
import argparse
from pathlib import Path

import numpy as np
from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json, zenapp
from profile_hybrid_gi import running_renderers


def receiver_taps(indices, width, height, reference_width, reference_height):
    px = (indices % reference_width + .5) * width / reference_width - .5
    py = (indices // reference_width + .5) * height / reference_height - .5
    x, y = np.floor(px).astype(int), np.floor(py).astype(int)
    fx, fy = px - x, py - y
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1)):
        pixel = np.clip(y + dy, 0, height - 1) * width + np.clip(x + dx, 0, width - 1)
        yield pixel, (fx if dx else 1 - fx) * (fy if dy else 1 - fy)


def compare(capture, reference, width, height, reference_width, reference_height, cache=False, pixels=None):
    indices = reference['indices']
    if pixels is None:
        data = np.memmap(capture, '<f4', mode='r', shape=(width * height, 18, 4))
    else:
        if Path(capture).stat().st_size != 24 + len(pixels) * 18 * 16:
            raise ValueError('Invalid selected hybrid capture size')
        counts = np.fromfile(capture, '<u8', count=3)
        data = np.memmap(capture, '<f4', mode='r', offset=24, shape=(len(pixels), 18, 4))
    values = np.zeros((len(indices), 18, 4), np.float64)
    matched = np.ones(len(indices), bool)
    identity = None
    for pixel, weight in receiver_taps(indices, width, height, reference_width, reference_height):
        tap = data[pixel if pixels is None else np.searchsorted(pixels, pixel)]
        values += tap * weight[:, None, None]
        current = tap[:, 12, 0].view('<u4')
        if identity is None:
            identity = current
        matched &= (weight == 0) | ((tap[:, 8, 3] > 0) & (current == identity))
    matched &= np.linalg.norm(values[:, 8, :3] - reference['position'], axis=1) < 1e-5
    if matched.sum() < 2:
        raise ValueError('Need at least two matching receiver positions to estimate energy uncertainty')
    rows = {}
    for depth in ((0, 1, 16) if cache else (1, 16)):
        if depth == 0:
            truth = reference['direct'][matched]
            halves = np.stack((truth, truth))
        else:
            truth = reference[f'bounce_{depth}'][matched]
            halves = reference[f'halves_{depth}'][:, matched]
            if cache:
                truth = truth + reference['direct'][matched]
                halves = halves + reference['direct'][None, matched]
        candidate = values[matched, 15 if cache else 3, :3]
        # Each half has twice the variance of the full reference. Half their
        # difference estimates the full-reference uncertainty per receiver.
        half_difference = (halves[0] - halves[1]).mean(axis=1) * .5
        tolerance = 3 * half_difference.std(ddof=1) / np.sqrt(len(half_difference))
        bias = float(candidate.mean() - truth.mean())
        rows[str(depth)] = dict(reference_energy=float(truth.mean()), candidate_energy=float(candidate.mean()),
                               bias=bias, relative_bias=bias / float(truth.mean()),
                               rms=float(np.sqrt(np.square(candidate-truth).mean())),
                               reference_half_rms=float(np.sqrt(np.square(halves[0]-halves[1]).mean())),
                               energy_tolerance=float(tolerance), energy_passed=bool(abs(bias) <= tolerance))
    if pixels is None:
        valid_count, supported_count = 0, 0
        for row in data.reshape(height, width, 18, 4):
            valid = row[:, 8, 3] > 0
            valid_count += int(valid.sum())
            supported_count += int((valid & (row[:, 15 if cache else 1, 3] > 0)).sum())
    else:
        valid_count, supported_count = int(counts[0]), int(counts[2 if cache else 1])
    report = dict(matched=int(matched.sum()), reference_receivers=len(indices), comparisons=rows,
                  valid_receivers=valid_count, supported_receivers=supported_count,
                  coverage=supported_count / max(valid_count, 1), cache_diagnostics=data[0, 13].tolist(),
                  cache_update_diagnostics=data[0, 14].tolist())
    return report, values, matched


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--mode', choices=('probes', 'pixel', 'cache'), default='probes')
    parser.add_argument('--depth', type=int, choices=(0, 1, 16), default=16)
    parser.add_argument('--width', type=int, default=3840)
    parser.add_argument('--height', type=int, default=2160)
    parser.add_argument('--reference-width', type=int, default=480)
    parser.add_argument('--reference-height', type=int, default=270)
    parser.add_argument('--frames', type=int, default=1024)
    parser.add_argument('--world-cache', type=int, choices=(0, 1), default=1,
                        help='pixel mode: 0 traces exact one-bounce hits, as the per-pixel methods without the cache')
    parser.add_argument('--pixel-divisor', type=int, choices=(1, 2, 4), default=None,
                        help='pixel mode: GI resolution divisor (default 1); the capture reads the full-resolution output')
    args = parser.parse_args()
    if (args.mode == 'cache' and args.depth == 1) or (args.mode != 'cache' and args.depth == 0):
        parser.error('Cache depth is 0 (direct only) or 16; gathered GI depth is 1 or 16')
    if args.mode != 'pixel' and (args.world_cache == 0 or args.pixel_divisor is not None):
        parser.error('--world-cache and --pixel-divisor apply to pixel mode')
    if running_renderers():
        parser.error('Close other Zen renderers before measuring')
    if min(args.width, args.height, args.reference_width, args.reference_height, args.frames) < 1:
        parser.error('Extents and frame count must be positive')
    args.setup, args.reference, args.output = args.setup.resolve(), args.reference.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    setup = read_json(args.setup)
    setup['run'].update(windowWidth=min(args.width, 1280), windowHeight=min(args.height, 720),
                        renderWidth=args.width, renderHeight=args.height, fullscreen=False, followWindow=False)
    setup['rendering']['gi']['asyncCompute'] = 0
    # The explicit fields below would be ignored by a setup saved in the optimal configuration.
    setup['rendering']['gi']['optimal'] = False
    # Phase 1 reads the full-resolution cache diagnostic while the retained renderer
    # runs at half resolution. Screen-probe filtering does not enter this comparison.
    divisor = 2 if args.mode == 'cache' else args.pixel_divisor or 1
    setup['rendering']['gi']['cone'].update(screenProbeGI=args.mode == 'probes', worldRadianceCache=args.world_cache == 1,
                                            worldCacheFeedback=args.depth == 16, nativeProbeBenchmark=False,
                                            referenceSamples=0, referenceTileStride=1, pixelResolutionDivisor=divisor)
    dataset = args.output / 'dataset'
    path = dataset / 'frames/view/scene.zenrender'
    path.parent.mkdir(parents=True)
    write_json(path, setup)
    reference_hash = digest(args.reference)
    with np.load(args.reference) as reference:
        taps = receiver_taps(reference['indices'], args.width, args.height, args.reference_width, args.reference_height)
        # Pixel zero contains the existing frame-wide cache diagnostics.
        pixels = np.unique(np.concatenate(([0], *(pixel for pixel, _ in taps))))
    manifest = dataset / 'dataset.json'
    write_json(manifest, dict(version=1, frames=[dict(id='view', noise_rms=dict(D_direct=1, D_sky=1, D_bounce=1),
                                                       hybrid_pixels=pixels.tolist())]))
    manifest_hash = digest(manifest)
    identity = provenance(args, path, [])
    command = zenapp(args, [f'--ground-truth={dataset}', f'--output={args.output/"run"}', f'--frames={args.frames}'],
                     args.output/'console.log')
    log = (args.output/'console.log').read_text(errors='replace')
    for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
        if marker in log:
            raise ValueError(f'{marker}: see console.log')
    final = provenance(args, path, [command])
    for key in ('executable_sha256', 'shaders_sha256', 'inputs', 'setup_sha256'):
        if identity[key] != final[key]:
            raise ValueError(f'Measurement input changed: {key}')
    if digest(args.reference) != reference_hash or digest(manifest) != manifest_hash:
        raise ValueError('Reference or sampled receiver list changed during measurement')
    state = read_json(args.output/'run/view.json')
    for key, expected in dict(width=args.width, height=args.height,
                              giWidth=(args.width+divisor-1)//divisor, giHeight=(args.height+divisor-1)//divisor,
                              gather='screen_probes' if args.mode == 'probes' else 'pixel', referenceSamples=0).items():
        if state.get(key) != expected:
            raise ValueError(f'Unexpected {key}={state.get(key)!r}')
    with np.load(args.reference) as reference:
        capture = args.output / 'run/view.hybrid-pixels.bin'
        report, values, matched = compare(capture, reference, args.width, args.height,
                                          args.reference_width, args.reference_height, args.mode == 'cache', pixels)
    report.update(provenance=final, actual=state, reference=str(args.reference), reference_sha256=reference_hash,
                  depth=args.depth, mode=args.mode, world_cache=args.world_cache, divisor=divisor, tool_sha256=digest(__file__), manifest_sha256=manifest_hash,
                  capture_bytes=capture.stat().st_size, capture_sha256=digest(capture))
    write_json(args.output/'report.json', report)
    np.savez_compressed(args.output/'matched.npz', values=values, matched=matched)
    print(report['comparisons'], flush=True)


if __name__ == '__main__':
    main()
