"""Archive and verify P0 lighting/resolution/viewport combinations on an open half-room.

The thin-wall fixture has both visible sky and an authored point light. Its component
checks complement Sponza's frozen image gates; they do not replace those gates or
the independent bounce/sky references. Config-mutating captures run sequentially.
"""
import argparse
import itertools
import json
from pathlib import Path

import numpy as np

from compare_hybrid_gi import load
from capture_hybrid_gi import fingerprint
from validate_hybrid_gi import run


def check(prefix, provider, resolution, viewport, lighting, bounce):
    inputs = json.loads(Path(str(prefix) + '.inputs.json').read_text())
    for entry in [inputs['executable']] + inputs['source_inputs'] + inputs['shaders']:
        if fingerprint(Path(entry['path']))['sha256'] != entry['sha256']:
            raise ValueError(f'Stale matrix input: {entry["path"]}')
    metadata, data = load(prefix)
    width, height = viewport
    valid = data[:, :, 8, 3] > 0
    components = np.fromfile(str(prefix) + '.lighting.bin', '<f4').reshape(height, width, 7, 4)
    sky = data[:, :, 1, :3][valid]
    indirect = data[:, :, 3, :3][valid]
    direct = components[:, :, 1, :3][valid]
    checks = dict(
        extent=metadata['gbuffer_extent'] == list(viewport),
        resolution=metadata['voxel_resolution'] == resolution,
        provider=metadata['ray_provider'] == provider,
        preset=metadata['sample_count'] == 4 and metadata['history_limit'] == 32
               and metadata['filter_iterations'] == 5 and metadata['frame'] == 64,
        bounce=metadata['indirect_intensity'] == bounce,
        lights=len(metadata['lights']) == (0 if lighting == 'environment' else 1),
        finite=bool(np.isfinite(data).all() and np.isfinite(components).all()),
        sky=bool(sky.max() == 0 if lighting == 'analytic' else sky.mean() > .01),
        direct=bool(direct.max() == 0 if lighting == 'environment' else direct.mean() > 1e-5),
        indirect=bool(indirect.max() == 0 if bounce == 0 else indirect.mean() > 1e-6),
        composition=float(np.max(abs(components[:, :, 0, :3] - components[:, :, 1:5, :3].sum(2)))) < 1e-5,
        diffuse_split=float(np.max(abs(components[:, :, 2, :3] - components[:, :, 5:7, :3].sum(2)))) < 1e-5,
        resources=bool(metadata['ray_scene_ready']) if provider == 'hardware' else
                  not metadata['ray_scene_ready'] and metadata['ray_scene_bytes'] == 0 and not metadata['reflections'])
    return dict(checks=checks, passed=all(checks.values()),
                means=dict(sky=float(sky.mean()), bounce=float(indirect.mean()), direct=float(direct.mean())),
                manifest=str(prefix) + '.inputs.json')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--providers', nargs='+', choices=('hardware', 'voxel'), default=['hardware', 'voxel'])
    parser.add_argument('--resolutions', nargs='+', type=int, choices=(64, 128, 256), default=[64, 128, 256])
    parser.add_argument('--viewports', nargs='+', choices=('960x540', '1920x1080'), default=['960x540', '1920x1080'])
    parser.add_argument('--capture', action='store_true')
    parser.add_argument('--retry-failed', action='store_true', help='Preserve verified passing captures and retry failed rows')
    parser.add_argument('--allow-present-baseline', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    configurations = []
    for provider, resolution, size, lighting, bounce in itertools.product(
            args.providers, args.resolutions, args.viewports, ('environment', 'analytic', 'both'), (0, 1)):
        configurations.append(dict(provider=provider, resolution=resolution,
                                   viewport=list(map(int, size.split('x'))), lighting=lighting, bounce=bounce,
                                   name=f'{provider}-{resolution}-{size}-{lighting}-bounce{bounce}'))
    (args.output / 'configurations.json').write_text(json.dumps(configurations, indent=2) + '\n')
    result_path = args.output / 'matrix-results.json'
    previous = json.loads(result_path.read_text()) if args.retry_failed and result_path.exists() else {}
    results = {}
    for configuration in configurations:
        provider, resolution = configuration['provider'], configuration['resolution']
        width, height = configuration['viewport']
        lighting, bounce = configuration['lighting'], configuration['bounce']
        folder = args.output / configuration['name']
        reuse = args.retry_failed and previous.get(configuration['name'], {}).get('passed', False)
        if args.capture and not reuse:
            run(args.exe, folder, ['thin_wall_light'], provider=provider, rt=provider == 'hardware',
                allow_present_baseline=args.allow_present_baseline, indirect=bounce, bounce_source='auto',
                resolution=resolution, width=width, height=height, reflections=provider == 'hardware',
                rt_shadows=True, environment_lighting=lighting != 'analytic', analytic_lighting=lighting != 'environment')
        result = check(folder / 'thin_wall_light', provider, resolution, (width, height), lighting, bounce)
        results[configuration['name']] = result
        print(configuration['name'], result['passed'], result['means'], flush=True)
        (args.output / 'matrix-results.json').write_text(json.dumps(results, indent=2) + '\n')
    failures = [name for name, result in results.items() if not result['passed']]
    print(json.dumps(dict(passed=not failures, failures=failures), indent=2))
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
