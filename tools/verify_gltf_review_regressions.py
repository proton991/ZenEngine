"""Render workflow, HDR-storage, and clearcoat-frame regression fixtures.

Runs serially because the renderer uses Data/engine.cfg; restores its exact bytes.
Uses raw GPU readbacks and an independent material oracle, plus an equivalent-image
clearcoat pair. Generated fixtures, logs, and reports stay in the output directory.
"""

import argparse
import base64
from collections import Counter
from copy import deepcopy
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from validate_voxelization import load_triangles
from voxelization_fixtures import Fixture, rgba_png
from voxelization_materials import MaterialOracle
from voxelization_reflectance import compare_reflectance


ROOT = Path(__file__).resolve().parents[1]


def texture(pixel):
    return dict(width=1, height=1, pixels=[pixel])


def add_textures(fixture, textures):
    fixture.document['images'] = [dict(uri='data:image/png;base64,' + base64.b64encode(
        rgba_png(t['width'], t['height'], t['pixels'])).decode()) for t in textures]
    fixture.document['textures'] = [dict(source=i) for i in range(len(textures))]


def quad(fixture, x, material, size=.07):
    a, b, c, d = [(x+dx, dy, -.247) for dx, dy in
                  ((-size, -size), (size, -size), (size, size), (-size, size))]
    fixture.mesh([[a, b, c], [a, c, d]], material,
                 uv0=[(0, 0), (1, 0), (1, 1), (0, 0), (1, 1), (0, 1)],
                 uv1=[(1, 0), (1, 1), (0, 1), (1, 0), (0, 1), (0, 0)])


def surface_fixture(folder):
    fixture = Fixture()
    textures = [texture([128, 192, 64, 255]), texture([128, 64, 192, 255])]
    add_textures(fixture, textures)
    materials = [dict(pbrMetallicRoughness=dict(metallicFactor=0))]
    for spec_gloss in (
        dict(diffuseFactor=[.8, .6, .4, 1], specularFactor=[.2, .4, .1]),
        dict(diffuseFactor=[.5, .75, .9, 1], specularFactor=[.8, .6, .4],
             diffuseTexture=dict(index=0, texCoord=1), specularGlossinessTexture=dict(index=1)),
        dict(diffuseFactor=[.4, .6, .8, 1], specularFactor=[1, 1, 1]),
        dict(diffuseFactor=[1, 1, 1, 0], specularFactor=[0, 0, 0]),
    ):
        materials.append(dict(extensions={'KHR_materials_pbrSpecularGlossiness': spec_gloss}))
    materials[4].update(alphaMode='MASK', alphaCutoff=.5)
    materials.append(dict(pbrMetallicRoughness=dict(metallicFactor=0), emissiveFactor=[1, .5, .25],
                          extensions={'KHR_materials_emissive_strength': {'emissiveStrength': 1e6}}))
    fixture.document['materials'] = materials
    fixture.document['extensionsUsed'].append('KHR_materials_pbrSpecularGlossiness')
    fixture.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])
    for index in range(1, 6):
        quad(fixture, (index-3)*.18, index)
    (folder/'material-contract.json').write_text(json.dumps(dict(materials=materials, textures=textures)))
    return fixture.save(folder, 'surfaces')


def clearcoat_fixtures(folder):
    fixture = Fixture()
    add_textures(fixture, [texture([128, 128, 255, 255]), texture([230, 128, 204, 255])])
    fixture.document['materials'] = [dict(
        pbrMetallicRoughness=dict(baseColorFactor=[.1, .1, .1, 1], metallicFactor=0, roughnessFactor=.5),
        normalTexture=dict(index=0), extensions={'KHR_materials_clearcoat': dict(
            clearcoatFactor=1, clearcoatRoughnessFactor=.15, clearcoatNormalTexture=dict(index=1))})]
    fixture.document['extensionsUsed'] = ['KHR_materials_clearcoat', 'KHR_texture_transform']
    quad(fixture, 0, 0, .4)
    baseline = fixture.save(folder, 'clearcoat-base')
    rotated = deepcopy(fixture.document)
    # Constant texels produce the same normal even with a different coordinate set/transform.
    binding = rotated['materials'][0]['extensions']['KHR_materials_clearcoat']['clearcoatNormalTexture']
    binding.update(texCoord=1, extensions={'KHR_texture_transform': {'rotation': .7, 'scale': [2, .5]}})
    changed = folder/'clearcoat-uv.gltf'
    changed.write_text(json.dumps(rotated))
    uncoated = deepcopy(fixture.document)
    uncoated['materials'][0]['extensions']['KHR_materials_clearcoat']['clearcoatFactor'] = 0
    control = folder/'clearcoat-disabled.gltf'
    control.write_text(json.dumps(uncoated))
    return baseline, changed, control


def check_surface(prefix, averaged):
    metadata = json.loads(prefix.with_suffix('.json').read_text())
    oracle = MaterialOracle(prefix, metadata, load_triangles(prefix, metadata))
    size = metadata['resolution']
    counts = Counter()
    for cell, record in enumerate(struct.iter_unpack('<4I4f', prefix.with_suffix('.voxels.bin').read_bytes())):
        owner, albedo, normal, occupied, *emission = record
        assert all(math.isfinite(v) and 0 <= v <= 65504 for v in emission)
        if not occupied:
            continue
        material = oracle.records[owner][0]
        counts[material] += 1
        position = (cell % size, cell//size % size, cell//(size*size))
        color = [(albedo >> (8*i) & 255)/255 for i in range(4)]
        weight = (normal >> 24)/255
        assert not oracle.attribute_error(owner, position, color, weight, emission[:3]), (material, position)
        if material == 5:
            assert emission[:3] == [65504.0]*3, emission
    assert all(counts[i] > 0 for i in (1, 2, 3, 5)) and counts[4] == 0, counts
    result = dict(occupied_by_material=dict(counts))
    if averaged:
        result['reflectance'] = compare_reflectance(prefix, metadata, oracle.triangles)
        assert result['reflectance']['failures'] == 0, result['reflectance']
        radiance = [r[5:] for r in struct.iter_unpack('<5I3f', prefix.with_suffix('.reflectance.bin').read_bytes()) if r[3]]
        assert metadata['has_radiance'] == 1 and radiance
        assert all(math.isfinite(v) and 0 <= v <= 65504 for rgb in radiance for v in rgb)
        assert max(max(rgb) for rgb in radiance) == 65504
        result['finite_radiance_cells'] = len(radiance)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT/'build/gltf-review-regressions')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    surface = surface_fixture(output)
    clearcoat = clearcoat_fixtures(output)
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    last = original
    results = {}
    environment = os.environ.copy()
    environment.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    cases = [(f'{backend}-{policy}', surface, backend, policy, True)
             for backend in ('geom', 'comp') for policy in ('owner', 'averaged')]
    cases += [(p.stem, p, 'geom', 'owner', False) for p in clearcoat]
    try:
        for name, model, backend, policy, capture_volume in cases:
            prefix = output/name
            settings = dict(default_model_path=model.as_posix(), camera_position='0,0,2',
                            voxelizer=backend, voxel_resolution=64, voxel_reflectance_policy=policy,
                            voxel_reflectance_budget_mb=512, environment_lighting='false', skybox_visible='false',
                            scene_lighting_override='true', light_count=1, shadow_map_resolution=256,
                            voxel_gi_analytic_lighting='true', voxel_gi_emissive_lighting='true')
            settings.update({'dynamic_light.enabled': 'false', 'light_markers.enabled': 'false',
                             'light.0.type': 'directional', 'light.0.direction': '-.6,-.2,-1',
                             'light.0.color': '1,1,1', 'light.0.intensity': math.pi,
                             'light.0.enabled': 'true', 'light.0.casts_shadows': 'false'})
            assert config.read_bytes() == last, 'Configuration changed externally'
            last = original + b'\n' + ''.join(f'{k}={v}\n' for k, v in settings.items()).encode()
            config.write_bytes(last)
            command = [str(args.renderer.resolve()), '--disable-rt', '--no-ui', '--frames=3',
                       '--width=256', '--height=256', '--rhi-thread=0', '--async-compute=0',
                       '--mode=3' if capture_volume else '--mode=2', f'--capture={prefix}.ppm']
            if capture_volume:
                command += [f'--capture-voxels={prefix}', '--voxel-grid-percent=100']
                if name == 'geom-owner':
                    command += ['--voxel-gbuffer']
            with prefix.with_suffix('.log').open('w') as log:
                process = subprocess.run(command, cwd=ROOT, env=environment, stdout=log,
                                         stderr=subprocess.STDOUT, timeout=180)
            errors = [line for line in prefix.with_suffix('.log').read_text(errors='replace').splitlines()
                      if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
            assert process.returncode == 0 and not errors, (name, process.returncode, errors[:3])
            results[name] = check_surface(prefix, policy == 'averaged') if capture_volume else dict(rendered=True)
            if name == 'geom-owner':
                emissions = [r[8:11] for r in struct.iter_unpack('<4f4I4f', prefix.with_suffix('.gbuffer.bin').read_bytes()) if r[3]]
                assert emissions and all(math.isfinite(v) and 0 <= v <= 65504 for rgb in emissions for v in rgb)
                assert max(max(rgb) for rgb in emissions) == 65504
                results[name]['finite_gbuffer_pixels'] = len(emissions)
            print('PASS', name, flush=True)
        assert (output/'clearcoat-base.ppm').read_bytes() == (output/'clearcoat-uv.ppm').read_bytes(), 'Clearcoat UVs changed the shared tangent frame'
        assert (output/'clearcoat-base.ppm').read_bytes() != (output/'clearcoat-disabled.ppm').read_bytes(), 'Clearcoat fixture has no visible coating contribution'
        results['clearcoat_equal_images'] = True
        results['clearcoat_visible_contribution'] = True
    finally:
        assert config.read_bytes() == last, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (output/'results.json').write_text(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
