"""Reproduce the frozen Sponza static and reset-settling acceptance gates.

Static hardware captures gate composed diffuse with ray-hit bounce (P5 decision 2).
Compute captures gate sky with the shipping cone default. Sky and bounce remain separately
reported. Reset-settling gates remain on sky; the static P5 amendment does not
silently change those limits. Independent sky correctness is checked separately
by ground_truth_sweep.py. Run config-mutating capture tools sequentially.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

import numpy as np

from capture_hybrid_gi import ROOT, CAMERAS as CAMERA_MATRICES, fingerprint
from compare_hybrid_gi import compare, load
from ground_truth_sweep import CAMERAS, ENVIRONMENTS

SHIPPING_ENVIRONMENTS = ('papermill', 'kloppenheim', 'qwantani', 'hotel', 'carpentry')
EVENTS = ('motion', 'cut', 'deform', 'alpha', 'rotation', 'resize')
FRAMES = (1, 4, 8, 32)


def capture(args, prefix, environment, camera, provider, reference=False, settling=False, motion=False,
            reference_samples=1024, frames=64):
    command = [sys.executable, str(ROOT / 'tools/capture_hybrid_gi.py'),
               '--exe', str(args.exe), '--scene', str(args.scene), '--output', str(prefix),
               '--camera', camera, '--provider', provider, '--environment-texture', ENVIRONMENTS[environment], '--frames', str(frames),
               '--bounce', '1', '--bounce-source', 'auto', '--reference-samples', str(reference_samples) if reference else '0']
    if provider == 'hardware':
        command.append('--rt')
    if settling:
        command += ['--stability', '--provider-switching']
    if motion:
        command.append('--camera-motion')
    subprocess.run(command, cwd=ROOT, check=True)


def validate_capture(args, prefix, environment, camera, provider, reference=False, reference_samples=1024, frames=64):
    """Reject incomplete, stale or differently configured inputs before comparing."""
    inputs = json.loads(Path(str(prefix) + '.inputs.json').read_text())
    # The caller may supply the archived matching executable after a later build
    # replaces the original bin/ path. Its content hash, checked below, is authoritative.
    for entry in inputs['source_inputs'] + inputs['shaders']:
        path = Path(entry['path'])
        if not path.exists() or fingerprint(path)['sha256'] != entry['sha256']:
            raise ValueError(f'Stale input for {prefix}: {path}')
    if inputs['executable']['sha256'] != fingerprint(args.exe)['sha256']:
        raise ValueError(f'Wrong executable for {prefix}')
    if fingerprint(args.scene)['sha256'] not in {entry['sha256'] for entry in inputs['source_inputs']}:
        raise ValueError(f'Wrong scene for {prefix}')
    scene = json.loads(Path(str(prefix) + '.gltf').read_text())
    source = json.loads(args.scene.read_text(encoding='utf-8'))
    if (scene['nodes'][-1].get('matrix') != CAMERA_MATRICES[camera]
            or any('normalTexture' in m for m in scene.get('materials', []))
            != any('normalTexture' in m for m in source.get('materials', []))):
        raise ValueError(f'Wrong camera or normal-map configuration for {prefix}')
    settings = inputs['settings']
    expected = dict(environment_texture=ENVIRONMENTS[environment], environment_lighting='true',
                    environment_intensity=1, environment_rotation_degrees=0, scene_lighting_override='true', light_count=0,
                    voxel_gi_shadow_enabled='false', voxel_gi_specular_occlusion='true', voxel_gi_ray_provider=provider,
                    voxel_gi_indirect_intensity=1, voxel_gi_bounce_source='auto', voxel_gi_history_frames=32,
                    voxel_gi_samples=4, voxel_gi_reference_samples=reference_samples if reference else 0,
                    voxel_gi_temporal='true', voxel_gi_filter='true', voxel_resolution=64)
    if any(settings.get(key) != value for key, value in expected.items()):
        raise ValueError(f'Wrong settings for {prefix}')
    metadata, data = load(prefix)
    expected = dict(width=960, height=540, frame=frames, ray_provider=provider, sample_seed=0,
                    sample_count=reference_samples if reference else 4, indirect_intensity=1,
                    history_limit=256 if reference else 32, filter_iterations=0 if reference else 5,
                    temporal=True, reflections=provider == 'hardware',
                    bounce_source='rays' if provider == 'hardware' else 'cone')
    if any(metadata.get(key) != value for key, value in expected.items()):
        raise ValueError(f'Wrong capture metadata for {prefix}')
    if not np.isfinite(data).all() or not (data[:, :, 8, 3] > 0).any():
        raise ValueError(f'Invalid receiver data for {prefix}')
    lines = Path(str(prefix) + '.log').read_text(errors='replace').splitlines()
    errors = [line for line in lines if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
    known = [line for line in errors if 'SYNC-HAZARD-PRESENT-AFTER-WRITE' in line]
    unexpected = [line for line in errors if line not in known or not args.allow_present_baseline]
    if unexpected:
        raise ValueError(f'Validation errors for {prefix}: {unexpected[:3]}')
    return dict(manifest=str(prefix) + '.inputs.json', preexisting_present_hazards=len(known),
                sky_cache_evaluated=metadata['sky_cache_evaluated'],
                sky_cache_center_fallback=metadata['sky_cache_center_fallback'])


def metrics(candidate, reference):
    return {channel: compare(candidate, reference, channel=channel) for channel in ('sky', 'bounce', 'diffuse')}


def passed(regions):
    return all(value['passed'] for value in regions.values())


def static_view(args, environment, camera):
    prefix = args.output / 'static' / f'{environment}-{camera}'
    reference = Path(str(prefix) + '-reference')
    if args.capture:
        capture(args, reference, environment, camera, args.provider, reference=True)
        capture(args, prefix, environment, camera, args.provider)
    evidence = [validate_capture(args, path, environment, camera, args.provider, ref)
                for path, ref in ((prefix, False), (reference, True))]
    quality = metrics(prefix, reference)
    _, data = load(reference)
    mask = data[:, :, 8, 3] > 0
    sky = float(data[:, :, 1, :3][mask].mean())
    bounce = float(data[:, :, 3, :3][mask].mean())
    if sky <= 1e-12:
        raise ValueError(f'Unlit reference cannot establish static sky acceptance: {reference}')
    channel = 'diffuse' if args.provider == 'hardware' else 'sky'
    return dict(quality=quality, bounce_share=bounce / max(sky + bounce, 1e-20), evidence=evidence,
                gated_channel=channel, gated=environment in SHIPPING_ENVIRONMENTS, passed=passed(quality[channel]))


def settling_view(args, environment, camera):
    prefix = args.output / 'settling' / f'{environment}-{camera}'
    voxel_reference = Path(str(prefix) + '-voxel-reference')
    if args.capture:
        capture(args, prefix, environment, camera, 'hardware', settling=True)
        capture(args, voxel_reference, environment, camera, 'voxel', reference=True)
    evidence = [validate_capture(args, prefix, environment, camera, 'hardware'),
                validate_capture(args, voxel_reference, environment, camera, 'voxel', True)]
    report = {}
    base_metadata, _ = load(prefix)
    for event in EVENTS:
        reference = Path(f'{prefix}-stability-{event}-reference')
        for frame in FRAMES:
            candidate = Path(f'{prefix}-stability-{event}-frame{frame}')
            metadata, data = load(candidate)
            if (metadata['width'], metadata['height']) != (960, 540):
                raise ValueError(f'Wrong restored extent: {candidate}')
            if event == 'rotation':
                delta = (metadata['environment_intensity_rotation_enabled_visible'][1]
                         - base_metadata['environment_intensity_rotation_enabled_visible'][1])
                if abs(delta - np.pi / 2) > 1e-6:
                    raise ValueError(f'Environment was not rotated: {candidate}')
            if frame == 1 and event in ('rotation', 'resize'):
                expected_reset = 'environment' if event == 'rotation' else 'resize'
                if metadata['reset_reason'] != expected_reset:
                    raise ValueError(f'Wrong reset reason for {candidate}: {metadata["reset_reason"]}')
            quality = metrics(candidate, reference)
            if quality['sky']['all_receivers']['reference_mean'] <= 1e-12:
                raise ValueError(f'Unlit reference cannot establish sky settling: {reference}')
            report[f'{event}-{frame}'] = dict(quality=quality, gated=frame == 32,
                reset_reason=metadata['reset_reason'], geometry_generation=metadata['scene_geometry_generation'],
                history_mean=float(data[:, :, 7, 0][data[:, :, 8, 3] > 0].mean()),
                passed=passed(quality['sky']))
    # Stage 0 is already hardware, so only actual transitions 1 and 2 are gates.
    hardware_reference = args.output / 'static' / f'{environment}-{camera}-reference'
    evidence.append(validate_capture(args, hardware_reference, environment, camera, 'hardware', True))
    for stage, reference in ((1, voxel_reference), (2, hardware_reference)):
        for frame in (1, 32):
            candidate = Path(f'{prefix}-switch-{stage}-frame{frame}')
            metadata, _ = load(candidate)
            expected_provider = 'voxel' if stage == 1 else 'hardware'
            if metadata['ray_provider'] != expected_provider:
                raise ValueError(f'Wrong switched provider: {candidate}')
            quality = metrics(candidate, reference)
            report[f'switch-{expected_provider}-{frame}'] = dict(quality=quality, gated=frame == 32,
                reset_reason=metadata['reset_reason'], passed=passed(quality['sky']))
    return dict(events=report, evidence=evidence, gated_channel='sky',
                passed=all(item['passed'] for item in report.values() if item['gated']))


def motion_view(args, environment, camera):
    prefix = args.output / 'motion' / f'{environment}-{camera}'
    if args.capture:
        capture(args, prefix, environment, camera, 'hardware', motion=True)
    evidence = validate_capture(args, prefix, environment, camera, 'hardware')
    report = {}
    # CaptureReflectionMotion moves the camera 0.003 normalized world units right each frame.
    right = np.array(CAMERA_MATRICES[camera][:3], dtype=np.float64)
    right /= np.linalg.norm(right)
    for frame in (8, 16, 24, 32):
        candidate = Path(f'{prefix}-motion-frame{frame}')
        reference = Path(str(candidate) + '-reference')
        metadata, data = load(candidate)
        _, reference_data = load(reference)
        identity_mismatches = int(np.count_nonzero(
            data[:, :, 12, 0].copy().view(np.uint32) != reference_data[:, :, 12, 0].copy().view(np.uint32)))
        valid = data[:, :, 8, 3] > 0
        position = data[:, :, 8, :][valid].astype(np.float64)
        matrix = np.array(metadata['projection_view_column_major']).reshape(4, 4).T
        current = position @ matrix.T
        position[:, :3] += .003 * right
        previous = position @ matrix.T
        expected = previous[:, :2] / previous[:, 3:4] - current[:, :2] / current[:, 3:4]
        extent = np.array([metadata['width'], metadata['height']])
        motion = data[:, :, 11, :2][valid] * .5 * extent
        delta_error = motion - expected * .5 * extent
        y, x = np.nonzero(valid)
        reprojected = np.column_stack((x, y)) + .5 + motion
        expected_pixel = (previous[:, :2] / previous[:, 3:4] * .5 + .5) * extent
        maximum = float(np.linalg.norm(reprojected - expected_pixel, axis=1).max())
        quality = metrics(candidate, reference)
        # The 0.01-pixel P1 limit applies during motion. The frozen shipping image
        # limits apply after static settling, so moving-image errors are diagnostics.
        report[f'moving-{frame}'] = dict(quality=quality, gated=True, reprojection_max_pixels=maximum,
            motion_delta_max_pixels=float(np.linalg.norm(delta_error, axis=1).max()),
            identity_mismatches=identity_mismatches, quality_gated=False, reprojection_passed=maximum < .01,
            passed=maximum < .01 and identity_mismatches == 0)
    return dict(events=report, evidence=evidence, gated_channel='reprojection',
                reprojection_passed=all(item['reprojection_passed'] for item in report.values()),
                passed=all(item['passed'] for item in report.values()))


def summary(report):
    lines = ['# Hybrid GI acceptance', '',
             f'Commit: `{report["commit"]}`. Provider: `{report["provider"]}`. Complete: {report["complete"]}.', '',
             'Frozen preset: four diffuse rays, 32-frame history, five filter iterations, 64 static frames. '
             'Bias / RMS / P99 / unexpected zeros; limits 2% / 8% / 20% / 0.', '',
             '| Check | Channel | All receivers | Floor | Pass |', '| --- | --- | --- | --- | --- |']
    for phase, views in report['results'].items():
        for view, item in views.items():
            cases = item.get('events', {'static': item})
            for event, case in cases.items():
                if event != 'static' and not case.get('gated', True):
                    continue
                channel = item['gated_channel']
                if channel == 'reprojection':
                    lines.append(f'| {view} {event} | motion (limit 0.01 px) | '
                                 f'{case["reprojection_max_pixels"]:.6f} px; {case["identity_mismatches"]} identity mismatches '
                                 f'| - | {case["passed"]} |')
                    continue
                regions = case['quality'][channel]
                cells = []
                for name in ('all_receivers', 'sponza_floor'):
                    m = regions[name]
                    cells.append(f'{m["bias"] * 100:+.2f}% / {m["rms"] * 100:.2f}% / '
                                 f'{m["p99"] * 100:.2f}% / {m["unexpected_zeros"]}')
                status = str(case['passed']) if case.get('gated', True) else f'{case["passed"]} (stress, reported)'
                lines.append(f'| {view} {event} | {channel} | {cells[0]} | {cells[1]} | {status} |')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--provider', choices=('hardware', 'voxel'), default='hardware')
    parser.add_argument('--environments', nargs='+', choices=ENVIRONMENTS, default=list(SHIPPING_ENVIRONMENTS))
    parser.add_argument('--cameras', nargs='+', choices=CAMERAS, default=list(CAMERAS))
    parser.add_argument('--phases', nargs='+', choices=('static', 'settling', 'motion'), default=['static'])
    parser.add_argument('--capture', action='store_true', help='Generate fresh captures before analysis')
    parser.add_argument('--allow-present-baseline', action='store_true',
                        help='Report only the already reproduced PRESENT_AFTER_WRITE hazard separately')
    args = parser.parse_args()
    if any(phase != 'static' for phase in args.phases) and args.provider != 'hardware':
        parser.error('The native sequences currently require hardware; voxel switches are included in settling')
    args.exe, args.scene, args.output = args.exe.resolve(), args.scene.resolve(), args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    report = dict(commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                  executable=fingerprint(args.exe), provider=args.provider,
                  requested=dict(phases=args.phases, environments=args.environments, cameras=args.cameras), results={})
    for phase in args.phases:
        report['results'][phase] = {}
        for environment in args.environments:
            for camera in args.cameras:
                view = f'{environment}-{camera}'
                result = {'static': static_view, 'settling': settling_view, 'motion': motion_view}[phase](args, environment, camera)
                report['results'][phase][view] = result
                phase_complete = len(report['results'][phase]) == len(args.environments) * len(args.cameras)
                report['complete'] = phase == args.phases[-1] and phase_complete
                report['passed'] = report['complete'] and all(
                    item['passed'] for views in report['results'].values() for item in views.values() if item.get('gated', True))
                (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
                (args.output / 'summary.md').write_text(summary(report), encoding='utf-8')
                phase_report = dict(report, results={phase: report['results'][phase]}, complete=phase_complete,
                                    passed=phase_complete and all(item['passed'] for item in report['results'][phase].values()
                                                                 if item.get('gated', True)))
                (args.output / f'{phase}-results.json').write_text(json.dumps(phase_report, indent=2) + '\n')
                (args.output / f'{phase}-summary.md').write_text(summary(phase_report), encoding='utf-8')
                print(time.strftime('%H:%M:%S'), phase, view, 'PASS' if result['passed'] else 'FAIL', flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
