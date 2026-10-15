"""Compare pure-compute triangle lighting with HW RT or an archived Compute run.

The default reference requires an RT-capable GPU. --compute-baseline uses saved
Compute captures and tighter optimization tolerances without requiring RT. Every
compute capture disables RT. Checks linear RGB per signal, closed geometry, and
actual runtime metadata. The separately reproduced presentation hazard is opt-in,
never hidden.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

from capture_hybrid_gi import ROOT, fingerprint
from hybrid_capture_io import read_bytes
from validate_hybrid_gi import run

FIXTURES = ['open_plane', 'closed_box', 'single_sided_closed_box', 'narrow_slot',
            'thin_pole', 'alpha_mask', 'alpha_uv1', 'alpha_transform', 'alpha_vertex',
            'alpha_specgloss', 'alpha_repeat', 'alpha_clamp', 'alpha_mirror',
            'mirrored_two_sided', 'forward_open_plane', 'forward_closed_box',
            'blend_closed_box', 'transmission_closed_box', 'half_wall', 'outside_volume',
            'bright_environment_plane', 'rotated_environment_plane', 'black_environment_plane',
            'point_light_room', 'thin_wall_light', 'glossy_floor']


def load(prefix):
    meta = json.loads(Path(str(prefix)+'.lighting.json').read_text())
    shape = (meta['height'], meta['width'])
    hybrid = np.frombuffer(read_bytes(str(prefix)+'.hybrid.bin'), '<f4').reshape(*shape, 13, 4)
    lighting = np.frombuffer(read_bytes(str(prefix)+'.lighting.bin'), '<f4').reshape(*shape, 7, 4)
    assert np.isfinite(lighting).all() and np.isfinite(hybrid[:, :, :12]).all()
    return meta, hybrid, lighting


def compare(compute, reference, allow_present_baseline=False, compute_reference=False):
    a, ah, al = load(compute)
    b, bh, bl = load(reference)
    assert not a['ray_query_enabled'] and not a['acceleration_structure_enabled'], a
    assert a['compute_traversal'] == 'software_bvh' and a['hardware_feature_mask'] == 0, a
    if compute_reference:
        assert not b['ray_query_enabled'] and not b['acceleration_structure_enabled'], b
        assert b['compute_traversal'] == 'software_bvh' and b['hardware_feature_mask'] == 0, b
    else:
        assert b['ray_query_enabled'] and b['hardware_feature_mask'] == 63, b
    assert a['receiver_position'] == b['receiver_position'] == 'raster_fp32'
    assert a['sample_count_max'] == b['sample_count_max']
    for key in ('sample_count', 'history_limit', 'filter_iterations', 'sample_seed', 'indirect_intensity',
                'frame', 'bounce_source', 'reflections', 'rt_shadows_requested', 'voxel_resolution', 'angular_guide_resolution'):
        assert a[key] == b[key], (key, a[key], b[key])
    mask = bh[:, :, 8, 3] > 0
    assert np.array_equal(mask, ah[:, :, 8, 3] > 0) and mask.any()
    assert np.array_equal(ah[:, :, 8:11], bh[:, :, 8:11]), 'Different raster receivers'
    baseline = 0
    for prefix in (compute, reference):
        errors = [line for line in Path(str(prefix)+'.log').read_text(errors='replace').splitlines()
                  if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
        presentation = [line for line in errors if 'SYNC-HAZARD-PRESENT-AFTER-WRITE' in line]
        baseline += len(presentation)
        assert not [line for line in errors if not (allow_present_baseline and line in presentation)], errors[:3]
    # Near-black components need an absolute tolerance tied to the scene's light
    # level. Otherwise an invisible residual can have an arbitrarily large ratio.
    output_scale = float(abs(bl[:, :, 0, :3][mask]).mean())
    incident_scale = float(abs(bh[:, :, 1, :3][mask]+bh[:, :, 3, :3][mask]).mean())
    signals = [(name, al[:, :, i, :3], bl[:, :, i, :3], output_scale) for i, name in enumerate(b['components'])]
    signals += [('sky', ah[:, :, 1, :3], bh[:, :, 1, :3], incident_scale),
                ('bounce', ah[:, :, 3, :3], bh[:, :, 3, :3], incident_scale),
                ('reflection', ah[:, :, 5, :3], bh[:, :, 5, :3], incident_scale),
                ('specular_visibility', ah[:, :, 5, 3:4], bh[:, :, 5, 3:4], 1.)]
    metrics = {}
    for name, actual, expected, scene_scale in signals:
        actual, expected = actual[mask], expected[mask]
        scale = max(float(abs(expected).mean()), .1*scene_scale, 1e-6)
        error = abs(actual - expected)
        rms = float(np.sqrt(np.mean(error**2))/scale)
        p99 = float(np.quantile(error, .99)/scale)
        limits = (.002, .01) if compute_reference else (.01, .02)
        metrics[name] = dict(rms=rms, p99=p99, normalization=scale,
                             maximum_absolute_error=float(error.max()), identical=bool(np.array_equal(actual, expected)),
                             passed=rms <= limits[0] and p99 <= limits[1])
    unexpected_dark = mask & np.all(ah[:, :, 1, :3] == 0, axis=2) & (np.max(bh[:, :, 1, :3], axis=2) > 1e-5)
    pending = set(map(tuple, np.argwhere(unexpected_dark)))
    largest = 0
    while pending:
        queue = [pending.pop()]
        size = 0
        while queue:
            y, x = queue.pop()
            size += 1
            for neighbor in ((y-1, x), (y+1, x), (y, x-1), (y, x+1)):
                if neighbor in pending:
                    pending.remove(neighbor)
                    queue.append(neighbor)
        largest = max(largest, size)
    # Isolated stochastic misses are possible; contiguous floor blocks are not.
    return dict(signals=metrics, unexpected_zero_sky_pixels=int(unexpected_dark.sum()),
                largest_zero_sky_cluster=largest, preexisting_present_hazards=baseline,
                passed=all(value['passed'] for value in metrics.values()) and largest <= 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scene', type=Path, help='Optional Sponza/glTF/Sponza.gltf for hall/top captures')
    parser.add_argument('--fixtures', nargs='*', default=FIXTURES)
    parser.add_argument('--reuse', action='store_true')
    parser.add_argument('--compute-baseline', type=Path,
                        help='Compare against an archived Compute capture root, using stricter optimization tolerances')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    prefixes = []
    backends = ('compute',) if args.compute_baseline else ('compute', 'hardware')
    if args.fixtures:
        for backend in backends:
            if not args.reuse:
                run(args.exe, args.output/backend, args.fixtures, provider='voxel' if backend == 'compute' else 'hardware',
                    rt=backend == 'hardware', backend=backend, bounce_source='rays', indirect=1,
                    shadows=True, rt_shadows=True, allow_present_baseline=args.allow_present_baseline)
        baseline = args.compute_baseline/'compute' if args.compute_baseline else args.output/'hardware'
        prefixes += [(name, args.output/'compute'/name, baseline/name) for name in args.fixtures]
    if args.scene:
        for camera in ('hall', 'top'):
            pair = []
            for backend in backends:
                prefix = args.output/f'sponza-{camera}-{backend}'
                pair.append(prefix)
                if not args.reuse:
                    command = [sys.executable, str(ROOT/'tools/capture_hybrid_gi.py'), '--exe', str(args.exe),
                               '--scene', str(args.scene), '--output', str(prefix), '--camera', camera,
                               '--backend', backend, '--bounce', '1', '--rt' if backend == 'hardware' else '--no-rt']
                    subprocess.run(command, cwd=ROOT, check=True)
            if args.compute_baseline:
                pair.append(args.compute_baseline/f'sponza-{camera}-compute')
            prefixes.append((f'sponza-{camera}', *pair))
    cases = {name: compare(a, b, args.allow_present_baseline, args.compute_baseline is not None) for name, a, b in prefixes}
    for name, result in cases.items():
        print(name, 'PASS' if result['passed'] else 'FAIL', flush=True)
    result = dict(executable=fingerprint(args.exe), compute_baseline=str(args.compute_baseline) if args.compute_baseline else None,
                  cases=cases,
                  passed=bool(cases) and all(case['passed'] for case in cases.values()))
    (args.output/'parity.json').write_text(json.dumps(result, indent=2)+'\n')
    assert result['passed'], 'See parity.json for failed signal comparisons'


if __name__ == '__main__':
    main()
