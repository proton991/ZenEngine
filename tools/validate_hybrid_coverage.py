"""Verify P2 fixture coverage, analytic raw estimates and isolated lighting channels.

Run sequentially with other native capture tools. This complements the frozen
Sponza image/convergence gates and the full configuration/component matrix.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import load
from validate_hybrid_gi import environment_integral, run, sky_cache_check


FUNCTIONAL = ('open_plane', 'closed_box', 'single_sided_closed_box', 'narrow_slot', 'thin_pole',
              'alpha_mask', 'mirrored_two_sided', 'forward_open_plane', 'forward_closed_box',
              'bright_environment_plane', 'rotated_environment_plane', 'black_environment_plane',
              'normal_map_plane', 'bright_edge_near', 'bright_edge_middle', 'bright_edge_far',
              'moving_occluder_light', 'blend_closed_box', 'transmission_closed_box')
ANALYTIC = ('open_plane', 'closed_box', 'single_sided_closed_box', 'forward_open_plane',
            'forward_closed_box', 'bright_environment_plane', 'rotated_environment_plane',
            'normal_map_plane', 'blend_closed_box', 'transmission_closed_box')


def raw_analytic(prefix):
    metadata, data = load(prefix)
    valid = data[:, :, 8, 3] > 0
    raw = data[:, :, 0, :3][valid]
    name = prefix.name
    if 'closed_box' in name:
        error = float(raw.max())
        passed = error == 0
    elif name == 'normal_map_plane':
        expected = float(((1 + (data[:, :, 9, :3][valid] * data[:, :, 10, :3][valid]).sum(1)) / 2).mean())
        error = float(abs(raw.mean() - expected) / expected)
        passed = error <= .005
    else:
        expected = environment_integral(prefix, metadata, data[:, :, 9, :3][valid][0])
        error = float(np.max(abs(raw.mean(0) - expected)) / expected.mean())
        passed = error <= .005
    return dict(raw_relative_error=error, passed=bool(passed))


def emission_channels(prefix, bounce):
    metadata, data = load(prefix)
    components = np.fromfile(str(prefix) + '.lighting.bin', '<f4').reshape(metadata['height'], metadata['width'], 7, 4)
    checks = dict(sky_zero=bool(np.max(abs(data[:, :, 1, :3])) == 0),
                  direct_zero=bool(np.max(abs(components[:, :, 1, :3])) == 0),
                  emission_present=bool(components[:, :, 4, :3].max() > .01),
                  bounce=bool(data[:, :, 3, :3].max() == 0 if bounce == 0 else data[:, :, 3, :3].max() > 1e-5))
    return dict(checks=checks, passed=all(checks.values()))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--providers', nargs='+', choices=('hardware', 'voxel'), default=['hardware', 'voxel'])
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {}
    for provider in args.providers:
        entry = {}
        report[provider] = entry
        options = dict(provider=provider, rt=provider == 'hardware',
                       allow_present_baseline=args.allow_present_baseline, reflections=provider == 'hardware')
        folder = args.output / provider
        entry['functional'] = run(args.exe, folder / 'functional', FUNCTIONAL, **options)
        entry['analytic'] = run(args.exe, folder / 'analytic', ANALYTIC, reference_samples=1024, **options)
        for name in ANALYTIC:
            entry['analytic'][name].update(raw_analytic(folder / 'analytic' / name))
        # Both voxelization paths must include the newly covered material classes.
        entry['geometry_voxelizer'] = run(args.exe, folder / 'geometry-voxelizer',
            ('blend_closed_box', 'transmission_closed_box'), voxelizer='geom', **options)
        entry['emission'] = {}
        for bounce in (0, 1):
            output = folder / f'emission-bounce{bounce}'
            run(args.exe, output, ['glossy_emissive'], provider=provider, rt=provider == 'hardware',
                allow_present_baseline=args.allow_present_baseline, indirect=bounce, bounce_source='auto',
                environment_lighting=False, analytic_lighting=False, reflections=False)
            entry['emission'][str(bounce)] = emission_channels(output / 'glossy_emissive', bounce)
        entry['passed'] = (all(item['passed'] for item in entry['analytic'].values())
                           and all(item['passed'] for item in entry['emission'].values()))
        (args.output / 'coverage-results.json').write_text(json.dumps(report, indent=2) + '\n')
    report['sky_cache'] = sky_cache_check(args.exe, args.output / 'sky-cache',
                                        allow_present_baseline=args.allow_present_baseline)
    report['passed'] = all(report[provider]['passed'] for provider in args.providers)
    (args.output / 'coverage-results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(passed=report['passed']), indent=2))
    raise SystemExit(not report['passed'])


if __name__ == '__main__':
    main()
