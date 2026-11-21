"""Freeze A/A bounds or compare the quality/frame-time portion of hardening gates.

Requires two completed repeats for the selected presets (all three by default). This does not certify the
GPU suites, pass budget, denoiser stage timing, or feature-removal byte identity.
Those checks retain their own evidence in Doc/HybridGI/Denoiser.md.

A phase that states its own frame-time budget (RenderPipelineHardeningPlan.md) replaces the frozen
whole-frame limit of one preset with --frame-budget PRESET=SUMMARY.json[,SUMMARY.json]: the worse
median of those captures. The report records the source of every replaced limit.
"""
import argparse
import json
import math
from pathlib import Path

PRESETS = ('hardware', 'compute', 'editor')
MODES = ('flicker', 'pan-slow', 'pan-fast', 'dolly', 'sparks', 'profile')


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def measurements(folder, presets=PRESETS):
    values = {}
    for preset in presets:
        for repeat in range(2):
            reports = {mode: read(folder / preset / f'run-{repeat}' / mode / 'report.json') for mode in MODES}
            flicker = reports['flicker']
            sample = {'static_rms': flicker['static_reference']['rms_8bit'],
                      'static_flicker': flicker['mean_std'],
                      'static_flicker_p999': flicker['p999_std'],
                      'frame_median_ms': reports['profile']['median_ms']}
            for mode in ('pan-slow', 'pan-fast', 'dolly'):
                rows = reports[mode]['poses']
                if [row['pose'] for row in rows] != [8, 32, 64]:
                    raise ValueError(f'Missing or reordered motion poses: {preset}/{repeat}/{mode}')
                sample.update({f'{mode}/{row["pose"]}': row['above_4_percent'] for row in rows})
            for camera in ('static', 'moving'):
                sparks = reports['sparks'][camera]
                sample[f'sparks/{camera}/pixels'] = sparks['spark_pixels_per_frame']
                for size in ('one', 'two_three', 'four_plus'):
                    sample[f'sparks/{camera}/{size}'] = sparks['per_frame_mean'][size]
            for name, value in sample.items():
                if not math.isfinite(value) or value < 0:
                    raise ValueError(f'Invalid measurement: {preset}/{repeat}/{name} = {value}')
                values.setdefault(f'{preset}/{name}', []).append(value)
    return values


def bounds(values, denoiser):
    result = {}
    for key, samples in values.items():
        if len(samples) != 2:
            raise ValueError(f'A/A needs two independent runs: {key}')
        low, high = min(samples), max(samples)
        band = high - low
        # Static RMS gets the observed repeat-to-repeat band beyond the worse run.
        # Other no-regression gates use the worse baseline repeat without extra slack.
        threshold = high + band if key.endswith('/static_rms') else high
        if denoiser and ('/pan-slow/' in key or '/pan-fast/' in key):
            threshold = .5 * low
        result[key] = dict(samples=samples, aa_band=band, maximum=threshold)
    return result


def frame_budget(preset, summaries):
    """The worse whole-frame median of accepted-baseline captures (tools/profile_hybrid_gi.py summaries)."""
    reports = [read(path) for path in summaries]
    medians = [report['median_ms'] for report in reports]
    if not summaries or not all(math.isfinite(value) and value > 0 for value in medians):
        raise ValueError(f'Frame budget for {preset} needs valid capture summaries')
    # An overlay hook may add its own work to the frame; a frozen budget needs clean provenance.
    hooked = [str(path) for path, report in zip(summaries, reports) if report.get('rtss_running', False)]
    if hooked:
        raise ValueError(f'Frame budget for {preset} cannot come from captures taken while RTSS ran: {hooked}')
    return dict(maximum=max(medians), samples=medians, sources=[str(Path(path).resolve()) for path in summaries])


def apply_frame_budgets(limits, budgets):
    """Replace frozen whole-frame limits with phase-stated budgets, keeping the frozen value on record."""
    for preset, budget in budgets.items():
        key = f'{preset}/frame_median_ms'
        if key not in limits:
            raise ValueError(f'No frame-time gate to replace for {preset}')
        limits[key] = dict(limits[key], frozen_maximum=limits[key]['maximum'], maximum=budget['maximum'],
                           budget_samples=budget['samples'], budget_sources=budget['sources'])
    return limits


def compare(limits, candidate):
    if limits.keys() != candidate.keys():
        raise ValueError('Candidate measurement set differs from baseline')
    rows = {}
    for key, limit in limits.items():
        samples = candidate[key]
        if len(samples) != 2 or not all(math.isfinite(v) and v >= 0 for v in samples):
            raise ValueError(f'Candidate requires two valid repeats: {key}')
        rows[key] = dict(**limit, candidate=samples, passed=max(samples) <= limit['maximum'])
    return dict(passed=all(row['passed'] for row in rows.values()), gates=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--presets', nargs='+', choices=PRESETS, default=list(PRESETS))
    parser.add_argument('--denoiser', action='store_true', help='Require 50%% lower pan error at every sampled pose')
    parser.add_argument('--frame-budget', action='append', default=[], metavar='PRESET=SUMMARY[,SUMMARY]',
                        help='Phase-stated whole-frame budget: the worse median of these capture summaries')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    limits  = bounds(measurements(args.baseline, args.presets), args.denoiser)
    budgets = {}
    for entry in args.frame_budget:
        preset, separator, paths = entry.partition('=')
        if not separator or preset not in args.presets:
            parser.error(f'--frame-budget expects PRESET=SUMMARY[,SUMMARY] for a selected preset: {entry}')
        budgets[preset] = frame_budget(preset, [Path(path) for path in paths.split(',')])
    limits = apply_frame_budgets(limits, budgets)
    report = dict(gates=limits)
    if args.candidate:
        original, candidate = read(args.baseline / 'identity.json'), read(args.candidate / 'identity.json')
        for key in ('setup_sha256', 'inputs', 'reference_sha256', 'reference_mask_sha256'):
            if original[key] != candidate[key]:
                raise ValueError(f'Comparison inputs differ: {key}')
        report = compare(limits, measurements(args.candidate, args.presets))
    report.update(scope='quality and whole-frame median only', denoiser_pan_gate=args.denoiser,
                  frame_budgets={preset: budget['sources'] for preset, budget in budgets.items()},
                  presets=args.presets,
                  baseline=str(args.baseline.resolve()),
                  candidate=str(args.candidate.resolve()) if args.candidate else None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, allow_nan=False) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in report.items() if key != 'gates'}))
    return 1 if report.get('passed') is False else 0


if __name__ == '__main__':
    raise SystemExit(main())
