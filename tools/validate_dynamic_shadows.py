"""Validate animated shadow-map caches against a fresh map at the captured pose.

Uses the shadow capture harness and generated point/spot/sun, alpha-mask, thin-plane,
and forward-material fixtures. No Mitsuba/GPU oracle is started: hardware shadows
are captured separately as a quality comparator, not an exact PCF image oracle.
Native CSVs retain per-frame face/copy counts, GPU times, and total draw counts.
Run --prepare-only without launching the renderer or changing engine.cfg.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics

import numpy as np

from validate_hardware_shadows import Captures, ROOT, fixture


CASES = {
    'point-light': ('point', 'opaque', 'light', False),
    'point-occluder': ('point', 'opaque', 'occluder', False),
    'point-both': ('point', 'opaque', True, False),
    'spot-occluder': ('spot', 'opaque', 'occluder', False),
    'sun-occluder': ('directional', 'opaque', 'occluder', False),
    'masked-occluder': ('point', 'mask', 'occluder', False),
    'thin-occluder': ('point', 'thin', 'occluder', False),
    'forward-masked': ('point', 'mask', 'occluder', True),
}


def freeze_pose(scene, capture, output):
    """Read the rendered pose; the final diagnostic frame does not use fixed dt."""
    prefix, metadata = capture[:2]
    volume = json.loads(Path(str(prefix)+'-voxels.json').read_text())
    nodes = np.fromfile(str(prefix)+'-voxels.nodes.bin', '<f4').reshape(-1, volume['node_stride']//4)
    assert len(nodes) == 2, 'This fixture expects a static floor and one moving occluder'
    matrices = nodes[:, :16].reshape(-1, 4, 4).transpose(0, 2, 1)
    # The identity floor carries the renderer's scene normalization. Remove it
    # from the captured world matrix instead of guessing the animation clock.
    inverse = np.linalg.inv(matrices[0].astype(float))
    local = inverse @ matrices[1]
    assert np.allclose(local[:3, :3], np.eye(3), atol=1e-5), 'Only translation is authored'
    snapshot = json.loads(scene.read_text())
    snapshot.pop('animations', None)
    snapshot['nodes'][1]['translation'] = local[:3, 3].tolist()
    for node in snapshot['nodes']:
        light = node.get('extensions', {}).get('KHR_lights_punctual')
        if light is not None:
            index = light['light']
            position = np.array(metadata['lights'][index]['position_range'][:3]+[1.])
            node['translation'] = (inverse @ position)[:3].tolist()
    output.write_text(json.dumps(snapshot), encoding='utf-8')
    return dict(occluder_translation=local[:3, 3].tolist(),
                lights=[entry['position_range'] for entry in metadata['lights']],
                scene_geometry_generation=metadata['scene_geometry_generation'],
                lighting_generation=metadata['lighting_generation'])


def compare_cache(animated, fresh, rms_limit):
    actual, expected = animated[2][:, :, 1, :3], fresh[2][:, :, 1, :3]
    a_receivers, b_receivers = animated[3][:, :, 8], fresh[3][:, :, 8]
    mask = (a_receivers[:, :, 3] > 0) & (b_receivers[:, :, 3] > 0)
    assert np.any(mask), 'No comparable receivers'
    positions = np.linalg.norm(a_receivers[:, :, :3]-b_receivers[:, :, :3], axis=2)
    position_error = float(positions[mask].max())
    mask_disagreement = int(np.count_nonzero((a_receivers[:, :, 3] > 0) != (b_receivers[:, :, 3] > 0)))
    # Include every pixel in radiance error: forward materials are absent from
    # the opaque receiver mask, but their stale visibility must still fail.
    scale = max(float(np.mean(expected)), 1e-8)
    error = actual-expected
    rms = float(np.sqrt(np.mean(error**2))/scale)
    return dict(relative_rms=rms, maximum_relative_error=float(np.max(abs(error))/scale),
                receiver_position_error=position_error, receiver_mask_disagreement=mask_disagreement,
                passed=rms <= rms_limit and position_error <= 1e-5 and mask_disagreement == 0)


def compare_quality(maps, hardware, unshadowed, edge_pixels):
    actual, expected, unblocked = [capture[2][:, :, 1, :3] for capture in (maps, hardware, unshadowed)]
    scale = max(float(np.mean(unblocked)), 1e-8)
    strength = unblocked.max(axis=2)
    lit = strength > scale*.01
    visibility = expected.max(axis=2)/np.maximum(strength, 1e-8)
    visible = visibility > .5
    boundary = np.zeros(lit.shape, bool)
    boundary[1:, :] |= visible[1:, :] != visible[:-1, :]
    boundary[:-1, :] |= visible[1:, :] != visible[:-1, :]
    boundary[:, 1:] |= visible[:, 1:] != visible[:, :-1]
    boundary[:, :-1] |= visible[:, 1:] != visible[:, :-1]
    # Exclude the support around hard-reference edges for a second metric, while
    # retaining whole-image errors so broad leaks cannot hide in this mask.
    for _ in range(edge_pixels):
        padded = np.pad(boundary, 1)
        boundary = np.logical_or.reduce([padded[y:y+lit.shape[0], x:x+lit.shape[1]]
                                         for y in range(3) for x in range(3)])
    interior = lit & ~boundary
    error = actual-expected
    values = error[interior]
    return dict(relative_rms=float(np.sqrt(np.mean(error**2))/scale),
                maximum_relative_error=float(np.max(abs(error))/scale),
                interior_receivers=int(interior.sum()), excluded_edge_radius_pixels=edge_pixels,
                interior_relative_rms=float(np.sqrt(np.mean(values**2))/scale) if values.size else None,
                interior_visibility_mismatches=int(np.count_nonzero(
                    ((actual.max(axis=2)/np.maximum(strength, 1e-8)) > .5)[interior] != visible[interior])),
                note='Quality measurements only: PCF and hardware hard shadows need not be pixel-identical.')


def profile_counts(prefix):
    with Path(str(prefix)+'.frames.csv').open(newline='') as stream:
        frames = {row['frame_index']: dict(frame=int(row['frame_index']), phase=row['phase'],
                  shadow_faces=0, shadow_copies=0, shadow_gpu_us=0.,
                  total_native_draws=int(row['draws']), gpu_frame_ms=float(row['gpu_frame_ms'])
                  if row['gpu_status'] == 'available' else None) for row in csv.DictReader(stream)}
    with Path(str(prefix)+'.passes.csv').open(newline='') as stream:
        for row in csv.DictReader(stream):
            frame = frames.get(row['frame_index'])
            if frame is not None and row['pass'].startswith(('SceneShadowFace_', 'SceneShadowCopy_')):
                key = 'shadow_faces' if row['pass'].startswith('SceneShadowFace_') else 'shadow_copies'
                frame[key] += 1
                if row['gpu_status'] == 'available':
                    frame['shadow_gpu_us'] += float(row['gpu_us'])
    values = list(frames.values())
    warm = [row for row in values if row['frame'] > 0]
    return dict(frames=values,
                warmed_median={key: statistics.median(row[key] for row in warm)
                               for key in ('shadow_faces', 'shadow_copies', 'shadow_gpu_us', 'total_native_draws')}
                if warm else {},
                note='Draw counts include all native passes; face/copy counts name only shadow-map passes.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--allow-present-baseline', action='store_true')
    parser.add_argument('--cases', nargs='+', choices=CASES, default=list(CASES))
    parser.add_argument('--frames', nargs='+', type=int, default=[24, 80])
    parser.add_argument('--resolutions', nargs='+', type=int, choices=[128, 256, 512, 1024, 2048], default=[1024, 2048])
    parser.add_argument('--width', type=int, default=480)
    parser.add_argument('--height', type=int, default=270)
    parser.add_argument('--cache-rms-limit', type=float, default=.002)
    parser.add_argument('--edge-pixels', type=int, default=3)
    args = parser.parse_args()
    if not args.prepare_only and args.exe is None:
        parser.error('--exe is required unless --prepare-only is used')
    if min(args.frames) < 2 or min(args.width, args.height) < 1 or args.edge_pixels < 0:
        parser.error('Frames must be >=2, dimensions positive, and edge pixels nonnegative')
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    assets = args.output/'fixtures'
    assets.mkdir(exist_ok=True)
    scenes = {name: fixture(assets, name, kind, blocker, forward, moving=motion)
              for name in args.cases for kind, blocker, motion, forward in [CASES[name]]}
    report = dict(complete=False, fixtures={name: str(path) for name, path in scenes.items()}, cases={},
                  cache_rms_limit=args.cache_rms_limit,
                  limits=['Synthetic translation fixtures; material edits and rejected frames use CPU regression tests.',
                          'Hardware shadows are a quality comparator, not an independent correctness oracle.'])
    output = args.output/'results.json'
    if not args.prepare_only:
        args.exe = args.exe.resolve()
        report['executable_sha256'] = hashlib.sha256(args.exe.read_bytes()).hexdigest()
        runner = Captures(args)
        try:
            for name, scene in scenes.items():
                for resolution in args.resolutions:
                    for frames in args.frames:
                        tag = f'{name}-{resolution}-{frames}'
                        common = dict(width=args.width, height=args.height, resolution=resolution)
                        animated = runner.run(scene, tag, hardware=False, rt=False, frames=frames, voxels=True, **common)
                        frozen = assets/(tag+'-frozen.gltf')
                        pose = freeze_pose(scene, animated, frozen)
                        fresh = runner.run(frozen, tag+'-fresh', hardware=False, rt=False, **common)
                        hardware = runner.run(frozen, tag+'-hardware', **common)
                        unshadowed = runner.run(frozen, tag+'-unshadowed', shadows=False, **common)
                        result = dict(pose=pose, cache=compare_cache(animated, fresh, args.cache_rms_limit),
                                      hardware_quality=compare_quality(fresh, hardware, unshadowed, args.edge_pixels),
                                      profile=profile_counts(animated[0]))
                        report['cases'][tag] = result
                        output.write_text(json.dumps(report, indent=2)+'\n')
                        print(tag, result['cache'], result['profile']['warmed_median'], flush=True)
            report['complete'] = True
            report['passed'] = all(case['cache']['passed'] for case in report['cases'].values())
        finally:
            runner.restore()
            output.write_text(json.dumps(report, indent=2)+'\n')
    else:
        output.write_text(json.dumps(report, indent=2)+'\n')
    if report.get('passed') is False:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
