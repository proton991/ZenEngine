"""P7 hard-shadow captures, independent CPU visibility, and map/ray cost comparison.

Generated assets and reports stay under --output. engine.cfg is restored byte-for-byte.
The image oracle multiplies an unshadowed direct-light capture by independent Embree
segment visibility. Injection is checked at the captured voxel owners against the
same triangle oracle. No temporal or spatial shadow reconstruction is involved.
"""
import argparse
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess

import numpy as np
from environment_reference import load_gltf, normalize, self_test
from hybrid_gi_fixtures import quad
from voxelization_fixtures import Fixture, rgba_png

ROOT = Path(__file__).resolve().parents[1]


def fixture(folder, name, kind='point', blocker='opaque', forward=False, count=1, moving=False, scatter=False,
            tilted=False):
    f = Fixture()
    material = dict(doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=[.5, .5, .5, 1], metallicFactor=0, roughnessFactor=.5))
    f.document['materials'] = [material, copy.deepcopy(material)]
    f.document['extensionsUsed'] = ['KHR_lights_punctual']
    if forward:
        material['extensions'] = {'KHR_materials_clearcoat': dict(clearcoatFactor=.3, clearcoatRoughnessFactor=.2)}
        f.document['extensionsUsed'].append('KHR_materials_clearcoat')
    if scatter:
        extensions = {'KHR_materials_diffuse_transmission': dict(diffuseTransmissionFactor=.5),
                      'KHR_materials_volume': dict(thicknessFactor=.1, attenuationDistance=.2),
                      'KHR_materials_volume_scatter': dict(multiscatterColorFactor=[.4, .5, .6])}
        material.setdefault('extensions', {}).update(extensions)
        f.document['extensionsUsed'].extend(extensions)
    f.mesh(quad([-2, 0, -2], [-2, 0, 2], [2, 0, 2], [2, 0, -2]))
    if tilted:
        # The light is below the triangle but above its shading hemisphere.
        # Offsetting along the interpolated normal makes the receiver shadow itself.
        normal = (np.array([2., 1., 0.])/math.sqrt(5.)).tolist()
        f.document['meshes'][0]['primitives'][0]['attributes']['NORMAL'] = f.attribute([normal]*6, 'VEC3')
    y = 2.000008 if blocker == 'beyond' else 1
    uv = [[0, 0], [0, 1], [1, 1], [0, 0], [1, 1], [1, 0]]
    if blocker == 'mask':
        mask = f.document['materials'][1]
        mask.update(alphaMode='MASK', alphaCutoff=.5)
        mask['pbrMetallicRoughness']['baseColorTexture'] = dict(index=0, texCoord=1,
            extensions={'KHR_texture_transform': dict(offset=[.17, .11], scale=[1.3, .7], rotation=.2)})
        f.document['extensionsUsed'].append('KHR_texture_transform')
        f.document['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(
            rgba_png(8, 8, [[255, 255, 255, 255 if x < 4 else 0] for z in range(8) for x in range(8)])).decode())]
        f.document['textures'] = [dict(source=0)]
    f.mesh(quad([-.7, y, -.65], [-.7, y, .45], [.7, y, .45], [.7, y, -.65]), material=1,
           uv0=[[0, 0]]*6, uv1=uv)
    lights = []
    for i in range(count):
        light = dict(type=kind, color=[1, .8, .6], intensity=4/count)
        if kind == 'spot':
            light['spot'] = dict(innerConeAngle=.5, outerConeAngle=.9)
        lights.append(light)
        node = dict(translation=[0 if count == 1 else .6*math.sin(i*2.4), 2,
                                 0 if count == 1 else .6*math.cos(i*2.4)],
                    rotation=[-math.sqrt(.5), 0, 0, math.sqrt(.5)],
                    extensions={'KHR_lights_punctual': dict(light=i)})
        if tilted:
            node['translation'] = [2, -1, 0]
        f.document['scenes'][0]['nodes'].append(len(f.document['nodes']))
        f.document['nodes'].append(node)
    f.document['extensions'] = {'KHR_lights_punctual': dict(lights=lights)}
    if moving:
        times = f.attribute([0, 1, 2], 'SCALAR')
        animation = dict(samplers=[], channels=[])
        for node_index in range(1, 2+count):
            base = f.document['nodes'][node_index].get('translation', [0, 0, 0])
            values = f.attribute([[base[0]+x, base[1], base[2]] for x in (-.4, .4, -.4)], 'VEC3')
            animation['channels'].append(dict(sampler=len(animation['samplers']),
                                              target=dict(node=node_index, path='translation')))
            animation['samplers'].append(dict(input=times, output=values, interpolation='LINEAR'))
        f.document['animations'] = [animation]
    angle = math.radians(-35)
    f.document['cameras'] = [dict(type='perspective', perspective=dict(yfov=math.radians(55), znear=.01, zfar=20))]
    f.document['scenes'][0]['nodes'].append(len(f.document['nodes']))
    f.document['nodes'].append(dict(camera=0, translation=[0, 2.7, 4], rotation=[math.sin(angle/2), 0, 0, math.cos(angle/2)]))
    return f.save(folder, name)


class Captures:
    def __init__(self, args):
        self.args = args
        self.config = ROOT/'Data/engine.cfg'
        self.original = self.config.read_bytes()
        self.active = self.original
        self.env = os.environ.copy()
        self.env.update(VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1', VK_LAYER_VALIDATE_SYNC='1')

    def run(self, scene, tag, hardware=True, shadows=True, rt=True, frames=4, width=320, height=180, voxels=False,
            benchmark=False):
        prefix = self.args.output/tag
        settings = dict(default_model_path=scene.as_posix(), scene_lighting_override='false',
            environment_lighting='false', skybox_visible='false', voxelizer='comp', voxel_resolution=64,
            voxel_reflectance_policy='averaged' if voxels else 'owner', voxel_reflectance_budget_mb=512,
            voxel_gi_indirect_intensity=0, voxel_gi_environment_lighting='false', voxel_gi_emissive_lighting='false',
            voxel_gi_analytic_lighting='true', voxel_gi_ray_provider='auto', voxel_gi_samples=1,
            voxel_gi_reference_samples=0, voxel_gi_reflections='false', voxel_gi_bounce_source='cone',
            voxel_gi_rt_shadows=str(hardware).lower(), voxel_gi_shadow_enabled=str(shadows).lower(),
            shadow_map_resolution=1024, light_count=0)
        settings.update({'dynamic_light.enabled': 'false', 'light_markers.enabled': 'false'})
        assert self.config.read_bytes() == self.active, 'External config edit; aborting'
        self.active = self.original+b'\n'+''.join(f'{k}={v}\n' for k, v in settings.items()).encode()
        self.config.write_bytes(self.active)
        command = [str(self.args.exe), '--no-ui', '--fixed-step', '--mode=3', f'--frames={frames-1}',
                   f'--width={width}', f'--height={height}', f'--profile={prefix}', f'--capture-lighting={prefix}',
                   f'--capture={prefix}.ppm']
        if not rt:
            command.append('--disable-rt')
        if voxels:
            command.append(f'--capture-voxels={prefix}-voxels')
        if benchmark:
            command += ['--disable-validation', '--vsync=0', '--warmup=16']
        with Path(str(prefix)+'.log').open('w') as log:
            result = subprocess.run(command, cwd=ROOT, env=self.env, stdout=log, stderr=subprocess.STDOUT, timeout=300)
        lines = Path(str(prefix)+'.log').read_text(errors='replace').splitlines()
        errors = [line for line in lines if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
        baseline = [line for line in errors if 'SYNC-HAZARD-PRESENT-AFTER-WRITE' in line]
        if self.args.allow_present_baseline:
            errors = [line for line in errors if line not in baseline]
        assert result.returncode == 0 and not errors, (tag, result.returncode, errors[:4])
        meta = json.loads(Path(str(prefix)+'.lighting.json').read_text())
        assert meta['hardware_shadows'] == (hardware and rt and shadows), meta
        lighting = np.fromfile(str(prefix)+'.lighting.bin', '<f4').reshape(height, width, 7, 4)
        hybrid = np.fromfile(str(prefix)+'.hybrid.bin', '<f4').reshape(height, width, 13, 4)
        assert np.isfinite(lighting).all() and np.isfinite(hybrid[:, :, :12]).all()
        return prefix, meta, lighting, hybrid, len(baseline)

    def restore(self):
        assert self.config.read_bytes() == self.active, 'External config edit; refusing to overwrite'
        self.config.write_bytes(self.original)


def visibility(oracle, positions, normals, light):
    direction = np.broadcast_to(-np.array(light['direction_type'][:3]), positions.shape).copy()
    if light['direction_type'][3] != 0:
        direction = np.array(light['position_range'][:3])-positions
    direction = normalize(direction)
    hemisphere = np.where(np.sum(normals*direction, axis=1) >= 0, 1., -1.)
    origins = positions+normals*hemisphere[:, None]*1e-6
    maximum = np.inf
    if light['direction_type'][3] != 0:
        delta = np.array(light['position_range'][:3])-origins
        maximum = np.linalg.norm(delta, axis=1)
        direction = normalize(delta)
    return oracle.intersect(origins, direction, maximum)[0] < 0


def compare(scene, shadowed, unshadowed):
    prefix, meta, lighting, hybrid, baseline = shadowed
    oracle, _ = load_gltf(scene)
    mask = hybrid[:, :, 8, 3] > 0
    positions = hybrid[:, :, 8, :3][mask]
    normals = hybrid[:, :, 10, :3][mask]
    visible = visibility(oracle, positions, normals, meta['lights'][0])
    direct = lighting[:, :, 1, :3][mask]
    unblocked = unshadowed[2][:, :, 1, :3][mask]
    expected = unblocked*visible[:, None]
    error = direct-expected
    scale = max(float(unblocked.mean()), 1e-10)
    # Two different raster receiver representations affect unshadowed baseline BRDFs slightly.
    # Any visibility mismatch on a meaningfully lit receiver is independently counted and fails.
    lit = np.max(unblocked, axis=1) > scale*.01
    mismatch = (lit & (np.max(direct, axis=1) > scale*.005)) != (lit & visible)
    report = dict(receivers=int(mask.sum()), blocked=int((~visible & lit).sum()), visible=int((visible & lit).sum()),
        visibility_mismatches=int(mismatch.sum()), rms=float(np.sqrt(np.mean(error**2))/scale),
        maximum_error=float(np.max(abs(error))/scale), preexisting_present_hazards=baseline)
    report['passed'] = report['visibility_mismatches'] == 0 and report['rms'] < .001
    return report


def compare_injection(scene, capture):
    prefix, meta = capture[:2]
    root = str(prefix)+'-voxels'
    volume = json.loads(Path(root+'.json').read_text())
    words = np.fromfile(root+'.voxels.bin', '<u4').reshape(-1, 8)
    occupied = (words[:, 1] >> 24) > 127
    records = np.fromfile(root+'.triangles.bin', '<u4').reshape(-1, 4)
    assert np.all(words[occupied, 0] < len(records)), 'Fixture has ownerless occupied voxels'
    cells = np.flatnonzero(occupied)
    owner = records[words[cells, 0]]
    vertices = np.fromfile(root+'.vertices.bin', '<f4').reshape(-1, volume['vertex_stride']//4)
    indices = np.fromfile(root+'.indices.bin', '<u4')
    nodes = np.fromfile(root+'.nodes.bin', '<f4').reshape(-1, volume['node_stride']//4)
    corners = vertices[indices[owner[:, 0, None]+np.arange(3)], :3]
    model = nodes[owner[:, 1], :16].reshape(-1, 4, 4).transpose(0, 2, 1)
    world = np.einsum('nij,nkj->nki', model[:, :3, :3], corners)+model[:, None, :3, 3]
    a, b, c = world[:, 0], world[:, 1], world[:, 2]
    size = volume['resolution']
    grid = np.stack((cells % size, cells//size % size, cells//(size*size)), axis=1)
    center = np.array(volume['grid_min'])+(grid+.5)*volume['voxel_size']
    ab, ac, ap = b-a, c-a, center-a
    d00, d01, d11 = (np.sum(u*v, axis=1) for u, v in ((ab, ab), (ab, ac), (ac, ac)))
    denominator = d00*d11-d01*d01
    v = (d11*np.sum(ap*ab, axis=1)-d01*np.sum(ap*ac, axis=1))/denominator
    w = (d00*np.sum(ap*ac, axis=1)-d01*np.sum(ap*ab, axis=1))/denominator
    weights = np.maximum(np.stack((1-v-w, v, w), axis=1), 0)
    weights /= weights.sum(axis=1)[:, None]
    positions = np.sum(world*weights[:, :, None], axis=1)
    geometric = normalize(np.cross(ab, ac))
    shading = normalize(((words[cells, 2, None] >> (8*np.arange(3))) & 255)/255.*2-1)
    geometric *= np.where(np.sum(geometric*shading, axis=1) >= 0, 1., -1.)[:, None]
    oracle, _ = load_gltf(scene)
    light = meta['lights'][0]
    visible = visibility(oracle, positions, geometric, light)
    direction = np.broadcast_to(-np.array(light['direction_type'][:3]), positions.shape).copy()
    attenuation = np.ones(len(cells))
    if light['direction_type'][3] != 0:
        direction = np.array(light['position_range'][:3])-positions
        distance = np.linalg.norm(direction, axis=1)
        attenuation = 1/np.maximum(distance**2, 1e-6)
        if light['position_range'][3] > 0:
            attenuation *= np.clip(1-(distance/light['position_range'][3])**4, 0, 1)
    direction = normalize(direction)
    if light['direction_type'][3] == 2:
        inner, outer = light['cone_shadow'][:2]
        attenuation *= np.clip((-direction@np.array(light['direction_type'][:3])-outer)/max(inner-outer, .001), 0, 1)**2
    reflectance = np.fromfile(root+'.reflectance.bin', '<u4').reshape(-1, 8)[cells]
    diffuse = ((reflectance[:, 4, None] >> (8*np.arange(3))) & 255)/255.
    actual = reflectance[:, 5:].copy().view('<f4')
    unshadowed = diffuse*np.array(light['color_intensity'][:3])*light['color_intensity'][3]*(
        attenuation*np.maximum(np.sum(shading*direction, axis=1), 0)/np.pi)[:, None]
    expected = unshadowed*visible[:, None]
    scale = max(float(unshadowed.mean()), 1e-10)
    error = actual-expected
    lit = unshadowed.max(axis=1) > .01*scale
    mismatches = (lit & (actual.max(axis=1) > .005*scale)) != (lit & visible)
    report = dict(voxels=len(cells), visibility_mismatches=int(mismatches.sum()),
        rms=float(np.sqrt(np.mean(error**2))/scale), maximum_error=float(np.max(abs(error))/scale))
    report['passed'] = report['visibility_mismatches'] == 0 and report['rms'] < .002
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--allow-present-baseline', action='store_true')
    parser.add_argument('--profile', action='store_true')
    args = parser.parse_args()
    args.exe = args.exe.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    assets = args.output/'fixtures'
    assets.mkdir(exist_ok=True)
    reference_fixtures = self_test()
    runner = Captures(args)
    shaders = hashlib.sha256()
    for shader in sorted((ROOT/'Data/SpvShaders').rglob('*.spv')):
        shaders.update(shader.relative_to(ROOT).as_posix().encode())
        shaders.update(shader.read_bytes())
    results = dict(executable_sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(),
                   shader_bundle_sha256=shaders.hexdigest(), cpu_reference_fixtures=reference_fixtures,
                   fixtures={}, profiles={})
    try:
        for kind, blocker, forward in [('point', 'opaque', False), ('point', 'beyond', False),
                ('spot', 'opaque', False), ('directional', 'opaque', False), ('point', 'mask', False),
                ('point', 'opaque', True), ('point', 'mask', True)]:
            name = f'{kind}-{blocker}'+('-forward' if forward else '')
            scene = fixture(assets, name, kind, blocker, forward)
            a = runner.run(scene, name, voxels=blocker in ('opaque', 'mask') and not forward)
            b = runner.run(scene, name+'-unshadowed', shadows=False)
            results['fixtures'][name] = compare(scene, a, b)
            if blocker in ('opaque', 'mask') and not forward:
                injection = compare_injection(scene, a)
                results['fixtures'][name]['injection'] = injection
                results['fixtures'][name]['passed'] &= injection['passed']
            print(name, results['fixtures'][name], flush=True)
        for forward in (False, True):
            name = 'tilted-forward' if forward else 'tilted-deferred'
            scene = fixture(assets, name, forward=forward, tilted=True)
            a = runner.run(scene, name)
            b = runner.run(scene, name+'-unshadowed', shadows=False)
            results['fixtures'][name] = compare(scene, a, b)
            print(name, results['fixtures'][name], flush=True)
        scene = assets/'point-opaque.gltf'
        a = runner.run(scene, 'fallback-requested', rt=False)
        b = runner.run(scene, 'fallback-maps', hardware=False, rt=False)
        results['fallback_identical'] = bool(np.array_equal(a[2], b[2]))
        scene = fixture(assets, 'scatter', scatter=True)
        capture = runner.run(scene, 'scatter')
        profile = json.loads(Path(str(capture[0])+'.profile.json').read_text())
        results['scatter_pass_executed'] = any(p['pass'] == 'ForwardScatterSource' for p in profile['pass_statistics'])
        assert results['scatter_pass_executed']
        scene = fixture(assets, 'tilted-scatter', scatter=True, tilted=True)
        a = runner.run(scene, 'tilted-scatter')
        b = runner.run(scene, 'tilted-scatter-unshadowed', shadows=False)
        profile = json.loads(Path(str(a[0])+'.profile.json').read_text())
        # Both planes have an unobstructed segment to this light below the floor.
        # Full composition checks the scattering source, which is excluded from hybrid receiver captures.
        results['tilted_scatter'] = dict(
            scatter_pass_executed=any(p['pass'] == 'ForwardScatterSource' for p in profile['pass_statistics']),
            unshadowed_image_identical=Path(str(a[0])+'.ppm').read_bytes() == Path(str(b[0])+'.ppm').read_bytes())
        assert all(results['tilted_scatter'].values()), results['tilted_scatter']
        scene = fixture(assets, 'moving', moving=True)
        for frames in (24, 80):
            name = f'moving-{frames}'
            a = runner.run(scene, name, frames=frames, voxels=True)
            assert a[1]['scene_geometry_generation'] > 1 and a[1]['lighting_generation'] > 2
            # All animated nodes share an authored X displacement; the floor fixes
            # the normalization. Read the actual light position rather than assume
            # which animation tick was captured.
            snapshot = json.loads(scene.read_text())
            _, transform = load_gltf(scene)
            displacement = a[1]['lights'][0]['position_range'][0]/transform['scale']
            snapshot['nodes'][1]['translation'] = [displacement, 0, 0]
            snapshot['nodes'][2]['translation'][0] = displacement
            snapshot.pop('animations')
            reference = assets/(name+'-reference.gltf')
            reference.write_text(json.dumps(snapshot))
            # The demo's final capture tick uses wall time even with --fixed-step.
            # Freeze the measured pose for the unshadowed comparison.
            b = runner.run(reference, name+'-unshadowed', shadows=False)
            results['fixtures'][name] = compare(reference, a, b)
            injection = compare_injection(reference, a)
            results['fixtures'][name]['injection'] = injection
            results['fixtures'][name]['passed'] &= injection['passed']
            print(name, results['fixtures'][name], flush=True)
        if args.profile:
            for count in (1, 4, 16, 32):
                for moving in (False, True):
                    name = f'lights-{count}-'+('moving' if moving else 'static')
                    scene = fixture(assets, name, count=count, moving=moving)
                    for hardware in (False, True):
                        tag = name+'-'+('rays' if hardware else 'maps')
                        capture = runner.run(scene, tag, hardware=hardware, frames=96, width=960, height=540, benchmark=True)
                        results['profiles'][tag] = str(capture[0])+'.profile.json'
                        print(tag, flush=True)
        results['passed'] = (all(r['passed'] for r in results['fixtures'].values()) and results['fallback_identical']
                             and results['scatter_pass_executed'])
        (args.output/'results.json').write_text(json.dumps(results, indent=2))
        assert results['passed'], results
    finally:
        runner.restore()


if __name__ == '__main__':
    main()
