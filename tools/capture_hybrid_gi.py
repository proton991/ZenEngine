"""Capture a frozen Sponza view, including reproducible inputs and linear diagnostics.

The scene argument is the glTF Sample Assets Sponza/glTF/Sponza.gltf. Cameras
are authored in that asset's world units, before ZenEngine's unit-cube scaling.
No source asset or shader is modified. engine.cfg is restored transactionally.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
CAMERAS = {
    'top': [0, 0, -1, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 16, 0, 1],
    'hall': [0, 0, -1, 0, -.06237828615518053, .9980525784828885, 0, 0,
             .9980525784828885, .06237828615518053, 0, 0, 8, 1.5, 0, 1],
}


def fingerprint(path):
    return dict(path=str(path), bytes=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Capture filename prefix')
    parser.add_argument('--camera', choices=CAMERAS, default='top')
    parser.add_argument('--frames', type=int, default=64, help='Successful frames including the linear capture frame')
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=540)
    parser.add_argument('--resolution', type=int, choices=(64, 128, 256), default=64)
    parser.add_argument('--provider', choices=('auto', 'voxel', 'hardware', 'legacy'), default='voxel')
    parser.add_argument('--as-budget-mb', type=int, default=0, help='Acceleration-structure preflight cap; zero leaves it unset')
    parser.add_argument('--samples', type=int, choices=(1, 2, 4), default=4)
    parser.add_argument('--reference-samples', type=int, choices=(0, 1024, 4096), default=0)
    parser.add_argument('--bounce', type=float, default=0)
    parser.add_argument('--environment', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--environment-texture', default='papermill.ktx',
                        help='Environment path, relative to Data/Textures or absolute')
    parser.add_argument('--temporal', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--filter', action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument('--rt', action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument('--strip-normal-maps', action='store_true',
                        help='Remove normal textures from the captured scene copy, for ground-truth comparisons with vertex normals')
    parser.add_argument('--provider-switching', action='store_true',
                        help='Also capture hardware/voxel/hardware at frames 1 and 32 under <output>-switch')
    parser.add_argument('--origin-stress', action='store_true',
                        help='Also translate scene and camera by 0, 100 and 10000 normalized X units; capture each after 64 frames')
    parser.add_argument('--stability', action='store_true',
                        help='Capture camera motion/cut, vertex deformation and alpha edits at frames 1/4/8/32 and fresh 1024-sample references')
    args = parser.parse_args()
    assert args.frames >= 2
    assert args.as_budget_mb >= 0
    prefix = args.output.resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    source = args.scene.resolve()
    document = json.loads(source.read_text())
    inputs = [fingerprint(source)]
    for entry in document.get('buffers', []) + document.get('images', []):
        uri = entry.get('uri', '')
        if uri and not uri.startswith('data:'):
            dependency = (Path(unquote(uri[5:]).lstrip('/')) if uri.startswith('file:')
                          else source.parent / unquote(uri)).resolve()
            inputs.append(fingerprint(dependency))
            entry['uri'] = dependency.as_uri()
    if args.strip_normal_maps:
        for material in document.get('materials', []):
            material.pop('normalTexture', None)
    camera = len(document.setdefault('cameras', []))
    document['cameras'].append(dict(type='perspective', perspective=dict(
        yfov=1.0471975511965976, znear=.05, zfar=100)))
    # Only the new camera may be selected by the importer.
    for node in document['nodes']:
        node.pop('camera', None)
    document['scenes'][document.get('scene', 0)]['nodes'].append(len(document['nodes']))
    document['nodes'].append(dict(camera=camera, matrix=CAMERAS[args.camera]))
    scene = Path(str(prefix) + '.gltf')
    scene.write_text(json.dumps(document))
    environment_texture = Path(args.environment_texture)
    if not environment_texture.is_absolute():
        environment_texture = ROOT / 'Data/Textures' / environment_texture
    inputs.append(fingerprint(environment_texture.resolve()))
    settings = dict(default_model_path=scene.as_posix(), environment_texture=args.environment_texture,
                    environment_lighting=str(args.environment).lower(), environment_intensity=1,
                    environment_rotation_degrees=0, skybox_visible='false', scene_lighting_override='true',
                    light_count=0, voxel_resolution=args.resolution, voxel_gi_indirect_intensity=args.bounce,
                    voxel_gi_shadow_enabled='false', voxel_gi_ray_provider=args.provider,
                    voxel_gi_acceleration_structure_budget_mb=args.as_budget_mb,
                    voxel_gi_samples=args.samples, voxel_gi_reference_samples=args.reference_samples,
                    voxel_gi_history_frames=32, voxel_gi_temporal=str(args.temporal).lower(),
                    voxel_gi_filter=str(args.filter).lower(), voxel_gi_specular_occlusion='true')
    settings.update({'dynamic_light.enabled': 'false', 'light_markers.enabled': 'false'})
    config = ROOT / 'Data/engine.cfg'
    original = config.read_bytes()
    changed = original + b'\n' + ''.join(f'{k}={v}\n' for k, v in settings.items()).encode()
    environment = os.environ.copy()
    environment.update(VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1', VK_LAYER_VALIDATE_SYNC='1')
    command = [str(args.exe.resolve()), '--no-ui', '--fixed-step', '--mode=3',
               f'--frames={args.frames-1}', f'--width={args.width}', f'--height={args.height}',
               f'--capture={prefix}.ppm', f'--capture-lighting={prefix}', f'--profile={prefix}']
    if not args.rt:
        command.append('--disable-rt')
    if args.provider_switching:
        command.append(f'--capture-provider-switching={prefix}-switch')
    if args.origin_stress:
        command.append(f'--capture-origin-stress={prefix}-origin')
    if args.stability:
        command.append(f'--capture-stability={prefix}-stability')
    try:
        config.write_bytes(changed)
        with Path(str(prefix) + '.log').open('w') as log:
            result = subprocess.run(command, cwd=ROOT, env=environment, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=600)
        assert result.returncode == 0, f'Capture failed; inspect {prefix}.log'
    finally:
        if config.read_bytes() != changed:
            raise RuntimeError('External engine.cfg edit; leaving it untouched')
        config.write_bytes(original)
    manifest = dict(command=command, settings=settings, source_inputs=inputs,
                    configuration_sha256=hashlib.sha256(changed).hexdigest(),
                    executable=fingerprint(args.exe.resolve()),
                    shaders=[fingerprint(p) for p in sorted((ROOT / 'Data/SpvShaders').rglob('*.spv'))])
    Path(str(prefix) + '.inputs.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(prefix, flush=True)


if __name__ == '__main__':
    main()
