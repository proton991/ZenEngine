"""Validate Cone contribution isolation and mesh-shadow injection on a Vulkan device.

Run with --exe pointing to scene_renderer_demo. Uses scene-linear HDR captures,
checks both voxelizers and reflectance policies, and restores engine.cfg exactly.
"""

import argparse
from array import array
import json
import math
import os
from pathlib import Path
import subprocess
import sys

import validate_voxel_gi as fixtures


def read_lighting(prefix):
    meta = json.loads(prefix.with_suffix('.lighting.json').read_text())
    assert meta['method'] == 'cone' and meta['bytes_per_pixel'] == 112
    values = array('f')
    values.frombytes(prefix.with_suffix('.lighting.bin').read_bytes())
    if sys.byteorder != 'little':
        values.byteswap()
    assert len(values) == meta['width'] * meta['height'] * 28
    assert all(math.isfinite(value) and value >= -1e-6 for value in values)
    return values


def component(values, index):
    return array('f', (values[pixel + index * 4 + channel]
                      for pixel in range(0, len(values), 28) for channel in range(3)))


def validate(executable, output):
    output.mkdir(parents=True, exist_ok=True)
    fixtures.OUT = output
    fixtures.make_fixture()
    config = fixtures.ROOT / 'Data/engine.cfg'
    original = config.read_bytes()
    last = original
    environment = os.environ.copy()
    environment.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~',
                       DISABLE_RTSS_LAYER='1')
    cases = {'all': (True, True, True), 'off': (False, False, False),
             'analytic': (True, False, False), 'environment': (False, True, False),
             'emissive': (False, False, True)}
    results = []
    try:
        for voxelizer, policy, threaded in [('comp', 'owner', 1), ('geom', 'averaged', 0)]:
            captures = {}
            for name, contributions in cases.items():
                settings = dict(default_model_path=(output / 'emissive.gltf').as_posix(),
                                camera_position='0,0,1.2', voxelizer=voxelizer,
                                voxel_resolution=64, voxel_reflectance_policy=policy,
                                voxel_reflectance_budget_mb=16, async_compute='auto',
                                light_count=1, environment_texture='papermill.ktx',
                                environment_lighting='true', environment_intensity=0.25,
                                environment_rotation_degrees=0, skybox_visible='false',
                                voxel_gi_indirect_intensity=1, voxel_gi_shadow_enabled='true',
                                shadow_map_resolution=256)
                settings.update({'light_markers.enabled': 'false', 'dynamic_light.enabled': 'false',
                                 'light.0.type': 'point', 'light.0.position': '0,0.25,0',
                                 'light.0.intensity': 0.2, 'light.0.range': 3,
                                 'light.0.color': '1,1,1', 'light.0.enabled': 'true',
                                 'light.0.casts_shadows': 'true'})
                for term, enabled in zip(('analytic', 'environment', 'emissive'), contributions):
                    settings[f'voxel_gi_{term}_lighting'] = str(enabled).lower()
                assert config.read_bytes() == last, 'engine.cfg changed externally'
                last = original + b'\n' + ''.join(f'{key}={value}\n' for key, value in settings.items()).encode()
                config.write_bytes(last)
                prefix = output / f'{voxelizer}-{policy}-{name}'
                command = [str(executable), '--frames=4', '--mode=3', '--width=320', '--height=180',
                           '--no-ui', '--disable-rt', '--fixed-step', f'--rhi-thread={threaded}',
                           f'--capture-lighting={prefix}']
                if name == 'all':
                    command.append(f'--profile={prefix}')
                with prefix.with_suffix('.log').open('w') as log:
                    result = subprocess.run(command, cwd=fixtures.ROOT, env=environment,
                                            stdout=log, stderr=subprocess.STDOUT, timeout=120)
                text = prefix.with_suffix('.log').read_text(errors='replace')
                errors = [line for line in text.splitlines()
                          if any(error in line for error in ('[error]', 'VUID-', 'SYNC-HAZARD', 'RDG [12]'))]
                assert result.returncode == 0 and not errors, (prefix, result.returncode, errors[:3])
                captures[name] = read_lighting(prefix)
                print(f'PASS {prefix.name}', flush=True)

            assert max(component(captures['off'], 2)) == 0, 'Disabled GI left diffuse light behind'
            for name in ('analytic', 'environment', 'emissive'):
                assert sum(component(captures[name], 2)) > 0.01, f'Missing {name} GI'
            for index in (1, 3, 4):
                reference = component(captures['all'], index)
                for values in captures.values():
                    assert max(abs(a - b) for a, b in zip(reference, component(values, index))) < 1e-5, \
                        'GI switch changed direct light, specular IBL or visible emission'
            isolated = [component(captures[name], 2) for name in ('analytic', 'environment', 'emissive')]
            combined = component(captures['all'], 2)
            relative_error = sum(abs(a - b - c - d) for a, b, c, d in zip(combined, *isolated)) / sum(combined)
            assert relative_error < 0.015, ('Contributions do not add up in linear HDR', relative_error)
            results.append(dict(voxelizer=voxelizer, reflectance=policy,
                                diffuse_additivity_relative_error=relative_error))
    finally:
        if config.read_bytes() == last:
            config.write_bytes(original)
        else:
            raise RuntimeError('engine.cfg changed externally; leaving it untouched')
    (output / 'results.json').write_text(json.dumps(results, indent=2))
    return results


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=fixtures.ROOT / 'build/cone-lighting-validation')
    arguments = parser.parse_args()
    print(json.dumps(validate(arguments.exe.resolve(), arguments.output.resolve()), indent=2))
