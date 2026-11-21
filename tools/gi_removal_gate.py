"""Check Phase 1 removals against the frozen ZenApp HW/Compute capture pixels.

This gate only proves image identity for the sampled static and moving frames.
Quality, timing and GPU-suite gates are recorded separately.
"""
import argparse
from pathlib import Path

import numpy as np
from PIL import Image

from gi_noise_probe import MOTIONS, POSES, WARMUP, motion_path, preset, record
from gi_matched_quality import image, metrics
from gi_stability import DEFAULT_EXE, ROOT, digest, provenance, read_json, write_json
from profile_hybrid_gi import running_renderers


def compare_frame(candidate, baseline, frame, error_prefix='error-x8'):
    filename = f'frame-{frame}.png'
    original = read_json(baseline / 'capture.json')
    baseline_sha = digest(baseline / filename)
    candidate_sha = digest(candidate / filename)
    if baseline_sha != original['files'][filename]:
        raise ValueError(f'Baseline capture changed: {baseline / filename}')
    changed = 0
    statistics = dict(rms_8bit=0., relative_rms_percent=0., psnr_db=None, mean_max_channel_8bit=0.,
                      p99_max_channel_8bit=0., max_channel_8bit=0., above_4_percent=0., above_16_percent=0., mean_bias_8bit=0.)
    # Identical files prove identical RGB without decoding and scanning two 4K images.
    # Different encodings still go through the pixel comparison.
    if candidate_sha != baseline_sha:
        actual, expected = image(candidate / filename), image(baseline / filename)
        if actual.shape != expected.shape:
            raise ValueError(f'Capture extent differs: {candidate}')
        error = np.abs(actual - expected).max(axis=2)
        changed = int(np.count_nonzero(error))
        if changed:
            Image.fromarray(np.clip(error * 8, 0, 255).astype(np.uint8)).save(candidate / f'{error_prefix}-{frame}.png')
        statistics = metrics(actual, expected)
    return dict(frame=frame, byte_identical=changed == 0, changed_pixels=changed,
                candidate_sha256=candidate_sha, baseline_sha256=baseline_sha, **statistics)


def main(args):
    if running_renderers():
        raise ValueError('Close other Zen renderers before measuring')
    args.output, args.baseline = args.output.resolve(), args.baseline.resolve()
    args.setup = args.setup.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    identity = provenance(args, args.setup, [])
    references = [args.baseline]
    if args.previous:
        args.previous = args.previous.resolve()
        references.append(args.previous)
        identity.update(previous=str(args.previous), previous_sha256=digest(args.previous / 'identity.json'))
    for reference in references:
        baseline = read_json(reference / 'identity.json')
        for key in ('setup_sha256', 'inputs'):
            if identity[key] != baseline[key]:
                raise ValueError(f'Comparison inputs differ: {reference}: {key}')
    identity.update(baseline_sha256=digest(args.baseline / 'identity.json'),
                    removal_tool_sha256=digest(__file__),
                    probe_sha256=digest(ROOT / 'tools/gi_noise_probe.py'),
                    capture_tool_sha256=digest(ROOT / 'tools/gi_matched_quality.py'))
    identity_path = args.output / 'identity.json'
    if identity_path.exists() and read_json(identity_path) != identity:
        raise ValueError('Measurement inputs changed; use a new output directory')
    write_json(identity_path, identity)
    source, rows, previous_rows = read_json(args.setup), {}, {}
    for name in args.presets:
        setup = preset(source, name)
        previous_preset = args.previous / name if args.previous else None
        if previous_preset and (previous_preset / 'run-0').is_dir():
            previous_preset /= 'run-0'
        # Keep the original capture intervals: readback must not change frame timing/history.
        destination = args.output / name / 'flicker'
        original = args.baseline / name / 'run-0' / 'flicker'
        record(args, destination, setup, 289, 320)
        rows[f'{name}/static'] = [compare_frame(destination, original, frame) for frame in range(289, 321)]
        if args.previous:
            previous_rows[f'{name}/static'] = [
                compare_frame(destination, previous_preset / 'flicker', frame, 'previous-error-x8')
                for frame in range(289, 321)]
        for kind in MOTIONS:
            destination = args.output / name / kind / 'moving'
            original = args.baseline / name / 'run-0' / kind / 'moving'
            frames = [WARMUP + pose for pose in POSES]
            record(args, destination, setup, min(frames), WARMUP + 65,
                   keep=frames, path=motion_path(setup, kind))
            rows[f'{name}/{kind}'] = [compare_frame(destination, original, frame) for frame in frames]
            if args.previous:
                previous_rows[f'{name}/{kind}'] = [
                    compare_frame(destination, previous_preset / kind / 'moving', frame, 'previous-error-x8')
                    for frame in frames]
        print(name, 'identical:', all(row['byte_identical'] for key, values in rows.items()
                                      if key.startswith(name + '/') for row in values), flush=True)
    current = provenance(args, args.setup, [])
    if any(identity[key] != value for key, value in current.items()):
        raise ValueError('Measurement inputs changed during the run')
    passed = all(row['byte_identical'] for values in rows.values() for row in values)
    previous_passed = all(row['byte_identical'] for values in previous_rows.values() for row in values)
    write_json(args.output / 'report.json', dict(passed=passed and previous_passed,
               scope='captured RGB byte identity', frames=rows, previous_frames=previous_rows,
               previous_passed=previous_passed if args.previous else None))
    return 0 if passed and previous_passed else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--previous', type=Path, help='Also compare the preceding removal or noise-probe capture (run 0)')
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--presets', nargs='+', choices=('hardware', 'compute'), default=['hardware', 'compute'])
    raise SystemExit(main(parser.parse_args()))
