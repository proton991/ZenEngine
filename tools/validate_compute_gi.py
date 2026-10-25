"""Measure G1 brick quality against archived triangle references and optionally profile the same views.

Runs serially because the existing capture/benchmark helpers own engine.cfg temporarily.
Quality uses 960x540 Papermill top/hall captures; optional timing uses native GPU timestamps
at 1920x1080, with adaptive sampling disabled and full diffuse resolution for both tracers.
The archived reference is a converged triangle render, not an independent Mitsuba reference.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from capture_hybrid_gi import ROOT, fingerprint
from compare_hybrid_gi import compare, load, validate_reference
from hybrid_capture_io import read_bytes


def run(command, log):
    with log.open('w') as stream:
        subprocess.run([str(part) for part in command], cwd=ROOT, stdout=stream,
                       stderr=subprocess.STDOUT, check=True)


def reference_inputs(prefix, scene):
    manifest = json.loads(Path(str(prefix) + '.inputs.json').read_text())
    scene_hash = fingerprint(scene)['sha256']
    if scene_hash not in {entry['sha256'] for entry in manifest['source_inputs']}:
        raise ValueError(f'Reference uses a different scene: {prefix}')
    for entry in manifest['source_inputs']:
        if fingerprint(Path(entry['path']))['sha256'] != entry['sha256']:
            raise ValueError(f'Reference dependency changed: {entry["path"]}')
    metadata, data = load(prefix)
    evidence = validate_reference(metadata, data)
    evidence['metadata'] = fingerprint(Path(str(prefix) + '.lighting.json'))
    evidence['hybrid_sha256'] = hashlib.sha256(read_bytes(str(prefix) + '.hybrid.bin')).hexdigest()
    return evidence


def quality(args, camera, traversal, resolution, output):
    prefix = output / f'{camera}-{traversal}-{resolution}'
    command = [sys.executable, ROOT / 'tools/capture_hybrid_gi.py', '--exe', args.exe,
               '--scene', args.scene, '--output', prefix, '--camera', camera, '--backend', 'compute',
               '--compute-traversal', traversal, '--brick-resolution', resolution,
               '--bounce', '1', '--bounce-source', 'auto', '--compact']
    run(command, Path(str(prefix) + '.capture.log'))
    metadata, _ = load(prefix)
    expected = 'bricks' if traversal == 'bricks' else 'software_bvh'
    if metadata['compute_traversal'] != expected or metadata['brick_resolution'] != resolution:
        raise ValueError(f'Requested tracer did not run: {prefix}')
    log = Path(str(prefix) + '.log').read_text(errors='replace')
    errors = [line for line in log.splitlines() if any(token in line for token in
              ('[error]', 'VUID-', 'SYNC-HAZARD', 'using defaults', 'rejected configuration'))]
    if errors:
        raise ValueError(f'Capture errors: {errors[:5]}')
    reference = args.reference_dir / f'papermill-{camera}-reference'
    report = {}
    for channel in ('sky', 'bounce', 'diffuse'):
        regions = compare(prefix, reference, channel=channel, required=('all_receivers', 'sponza_floor'))
        for region in regions.values():
            # The original triangle path has stricter thresholds; preserve measurements,
            # and explicitly report the relaxed +/-10% mean-bias screen for G1 separately.
            region.pop('passed', None)
            region['within_ten_percent_bias'] = abs(region['bias']) <= .1
        report[channel] = regions
    report['metadata'] = {key: value for key, value in metadata.items()
                          if key.startswith('brick_') or key in ('compute_traversal', 'visibility_geometry')}
    report['capture'] = str(prefix)
    return report


def profile(args, camera, traversal, resolution, output, frozen):
    folder = output / f'profile-{camera}-{traversal}-{resolution}'
    command = [sys.executable, ROOT / 'tools/benchmark_voxel_gi.py', '--executable', args.exe,
               '--output', folder, '--frames', args.frames, '--warmup', 64, '--profile',
               '--backend', 'compute', '--width', 1920, '--height', 1080]
    # Match quality lighting, then hold the sampling work constant for a tracer comparison.
    settings = json.loads(Path(str(frozen) + '.inputs.json').read_text())['settings']
    settings.update(default_model_path=str(frozen) + '.gltf', voxel_gi_compute_traversal=traversal,
                    voxel_gi_brick_resolution=resolution, voxel_gi_adaptive_sampling='false',
                    voxel_gi_diffuse_resolution='full')
    for key, value in settings.items():
        command += ['--set', f'{key}={value}']
    run(command, Path(str(folder) + '.log'))
    summary = json.loads((folder / 'summary.json').read_text())
    if summary['resolved_settings']['compute_traversal'] != ('bricks' if traversal == 'bricks' else 'software_bvh'):
        raise ValueError(f'Requested tracer did not run during profiling: {folder}')
    passes = [entry for entry in summary['pass_statistics']
              if entry['phase'] == 'measured' and entry['pass'] == 'HybridTrace']
    if len(passes) != 1 or passes[0]['gpu_us']['count'] != args.frames:
        raise ValueError(f'Missing GPU trace measurements: {folder}')
    return dict(summary=str(folder / 'summary.json'), trace_gpu_us=passes[0]['gpu_us'],
                gpu_frame_ms=summary['gpu_frame_ms'], settings=summary['resolved_settings'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--reference-dir', type=Path, required=True,
                        help='Folder containing papermill-{top,hall}-reference captures and manifests')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--resolutions', type=int, choices=(64, 128, 256), nargs='+', default=[128])
    parser.add_argument('--profile', action='store_true')
    parser.add_argument('--frames', type=int, default=120)
    args = parser.parse_args()
    if args.frames < 1:
        parser.error('--frames must be positive')
    args.exe, args.scene, args.reference_dir = args.exe.resolve(), args.scene.resolve(), args.reference_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(executable=fingerprint(args.exe), references={}, cases={})
    for camera in ('top', 'hall'):
        report['references'][camera] = reference_inputs(args.reference_dir / f'papermill-{camera}-reference', args.scene)
        for traversal, resolution in [('triangles', 128)] + [('bricks', r) for r in args.resolutions]:
            name = f'{camera}-{traversal}-{resolution}'
            case = quality(args, camera, traversal, resolution, output)
            if args.profile:
                case['profile'] = profile(args, camera, traversal, resolution, output, Path(case['capture']))
            report['cases'][name] = case
            (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
            print(f'{name}: floor sky bias {100 * case["sky"]["sponza_floor"]["bias"]:+.2f}%, '
                  f'diffuse bias {100 * case["diffuse"]["sponza_floor"]["bias"]:+.2f}%', flush=True)


if __name__ == '__main__':
    main()
