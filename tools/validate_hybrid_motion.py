"""Check every-frame geometry invalidation and independent native view histories.

The two-plane fixture moves its roof for 16 uninterrupted frames, both through
instance transforms and vertex uploads. Matching converged poses are rendered
afterward. Quality is gated after 32 static frames; moving frames must track the
current shadow and publish matching geometry with reset lighting history.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import compare, load
from validate_hybrid_gi import run


def analyze(folder):
    root = folder / 'motion_roof'
    report = {'motion': {}, 'views': {}}
    for stage in ('transform', 'deform'):
        # Frame 8 is the original, undisplaced roof, also used before each stage.
        _, previous_reference = load(Path(f'{root}-geometry-{stage}-frame8-reference'))
        previous_generation = None
        for frame in range(1, 17):
            prefix = Path(f'{root}-geometry-{stage}-frame{frame}')
            metadata, data = load(prefix)
            _, reference = load(Path(str(prefix) + '-reference'))
            valid = data[:, :, 8, 3] > 0
            identity = data[:, :, 12, 0].copy().view(np.uint32)
            expected_identity = reference[:, :, 12, 0].copy().view(np.uint32)
            floor = valid & (data[:, :, 8, 1] < data[:, :, 8, 1][valid].min() + .001)
            # The voxel capture reports depth-reconstructed positions, whereas motion is
            # rasterized from vertices. Intersect pixel rays with the fixture's exact
            # planes to keep depth quantization out of this independent motion check.
            matrix = np.array(metadata['projection_view_column_major']).reshape(4, 4).T
            height, width = valid.shape
            y, x = np.mgrid[:height, :width]
            ndc = np.stack(((x + .5) * 2 / width - 1, (y + .5) * 2 / height - 1,
                            np.zeros_like(x), np.ones_like(x)), axis=-1)
            inverse = np.linalg.inv(matrix)
            near = ndc @ inverse.T
            near /= near[:, :, 3:4]
            ndc[:, :, 2] = .5
            far = ndc @ inverse.T
            far /= far[:, :, 3:4]
            direction = far - near
            # Unit-cube normalization: x/z span four source units, centre y=.5.
            plane = np.where(floor, -.125, .125)
            position = near + direction * ((plane - near[:, :, 1]) / direction[:, :, 1])[:, :, None]
            previous = position.copy()
            previous[valid & ~floor, 0] -= -.175 if frame == 1 else .025
            clip, old_clip = position @ matrix.T, previous @ matrix.T
            expected_motion = old_clip[valid, :2] / old_clip[valid, 3:4] - clip[valid, :2] / clip[valid, 3:4]
            error = (data[:, :, 11, :2][valid] - expected_motion) * .5 * [metadata['width'], metadata['height']]
            generation = metadata['scene_geometry_generation']
            item = dict(history_max=float(data[:, :, 7, 0][valid].max()),
                        reset_reason=metadata['reset_reason'], geometry_generation=generation,
                        ray_scene_generation=metadata['ray_scene_generation'],
                        visibility_generation=metadata['ray_scene_generation'] if metadata['ray_provider'] == 'hardware'
                                              else metadata['voxel_geometry_generation'],
                        motion_max_pixels=float(np.linalg.norm(error, axis=1).max()),
                        identity_mismatches=int(np.count_nonzero(identity != expected_identity)),
                        quality=compare(prefix, Path(str(prefix) + '-reference')))
            previous_identity = previous_reference[:, :, 12, 0].copy().view(np.uint32)
            common_floor = floor & (previous_reference[:, :, 8, 3] > 0) & (identity == previous_identity)
            current = reference[:, :, 1, :3]
            change = current - previous_reference[:, :, 1, :3]
            error = data[:, :, 1, :3] - current
            denominator = float(np.square(change[common_floor]).sum())
            # -1 is the preceding shadow, 0 the current shadow; -0.5 is the
            # equal-distance boundary. Require each frame to track the current pose.
            item['shadow_pixels'] = int(common_floor.sum())
            item['old_shadow_projection'] = float((error[common_floor] * change[common_floor]).sum() / max(denominator, 1e-20))
            item['passed'] = (item['history_max'] == 1 and item['reset_reason'] == 'geometry_or_opacity'
                              and item['visibility_generation'] == generation
                              and denominator > 0 and item['old_shadow_projection'] > -.5
                              and item['identity_mismatches'] == 0
                              and item['motion_max_pixels'] < .01
                              and (previous_generation is None or generation > previous_generation))
            report['motion'][f'{stage}-{frame}'] = item
            previous_generation, previous_reference = generation, reference
        quality = compare(Path(f'{root}-geometry-{stage}-settled32'), Path(f'{root}-geometry-{stage}-frame16-reference'))
        report['motion'][f'{stage}-settled32'] = dict(quality=quality, passed=all(v['passed'] for v in quality.values()))
    for view in (1, 2):
        for frame in (1, 2, 32):
            prefix = Path(f'{root}-views-view{view}-frame{frame}')
            metadata, data = load(prefix)
            valid = data[:, :, 8, 3] > 0
            length = data[:, :, 7, 0][valid]
            # Length is stored in FP16 each frame; bilinear reprojection may round
            # downward by half an ULP at each write even for a stationary receiver.
            rounding = float(np.spacing(np.float16(frame))) * frame * .5
            quality = compare(prefix, Path(f'{root}-views-view{view}-reference'))
            report['views'][f'{view}-{frame}'] = dict(frame=metadata['frame'], history_min=float(length.min()),
                history_max=float(length.max()), quality=quality,
                passed=metadata['frame'] == frame and bool(np.all(np.abs(length - frame) <= rounding))
                and (frame < 32 or all(v['passed'] for v in quality.values())))
    report['passed'] = all(item['passed'] for group in report.values() for item in group.values())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provider', choices=('hardware', 'voxel'), default='hardware')
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    if args.capture:
        run(args.exe, args.output, ['motion_roof'], 64, args.provider, args.allow_present_baseline,
            args.provider == 'hardware', geometry_motion=True, independent_views=True)
    report = analyze(args.output)
    (args.output / 'motion-results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'failures': [name for group in ('motion', 'views')
          for name, item in report[group].items() if not item['passed']]}, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
