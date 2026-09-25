"""Static directional GI scene/HDR checks. Runs serially and restores engine.cfg byte for byte."""
import argparse
import array
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

import validate_dynamic_voxel_gi as hdr
import validate_voxel_gi as legacy
from voxelization_fixtures import Fixture, rgba_png

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = dict(diffuse_abs=8e-5, diffuse_rel=8e-4, normal_abs=8e-4,
                direct_same_texel_abs=2e-5, samples_per_image=2500,
                surface_bytes_per_pixel=96, raw_face_bytes_per_cell=32, subtexel_precision_bits=8)


def normalize(v):
    length = math.sqrt(sum(x*x for x in v))
    return [x/length for x in v]


def normal_fixture(folder, slope, mirrored, mapped=True):
    fixture = Fixture()
    fixture.document['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[.7, .7, .7, 1], metallicFactor=0, roughnessFactor=.8))]
    if mapped:
        png = rgba_png(1, 1, [[220, 160, 200, 255]])
        fixture.document['images'] = [dict(uri='data:image/png;base64,'+base64.b64encode(png).decode())]
        fixture.document['textures'] = [dict(source=0)]
        fixture.document['materials'][0]['normalTexture'] = dict(index=0)
    # Fix normalization independently of the receiver translation/slope.
    fixture.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])
    points = [(-.38, -.3, -.2-.38*slope), (.38, -.3, -.2+.38*slope),
              (.38, .3, -.2+.38*slope), (-.38, .3, -.2-.38*slope)]
    order = [(0, 2, 1), (0, 3, 2)] if mirrored else [(0, 1, 2), (0, 2, 3)]
    uv = [(0, 0), (1, 0), (1, 1), (0, 1)]
    fixture.mesh([[points[i] for i in t] for t in order],
                 transform=dict(scale=[-1, 1, 1]) if mirrored else None,
                 uv0=[uv[i] for t in order for i in t])
    # The mirrored fixture has front-facing world winding and authored +Z normals;
    # this isolates normal transformation from the existing backface-culling policy.
    primitive = fixture.document['meshes'][-1]['primitives'][0]
    primitive['attributes']['NORMAL'] = fixture.attribute([normalize([-slope, 0, 1])]*6, 'VEC3')
    # A back-facing triangle is invisible to the camera and casts a mesh shadow.
    fixture.mesh([[(-.12, -.12, .05), (-.12, .12, .05), (.12, -.12, .05)]])
    name = f'normal-{slope}-{mirrored}-{mapped}'
    path = fixture.save(folder, name)
    return path, normalize([slope if mirrored else -slope, 0, 1])


def floats(path):
    data = array.array('f')
    data.frombytes(path.read_bytes())
    if sys.byteorder != 'little':
        data.byteswap()
    return data


def trilinear(faces, face, coordinate, n):
    p = [v-.5 for v in coordinate]
    lo = [math.floor(v) for v in p]
    fraction = [p[i]-lo[i] for i in range(3)]
    result = [0., 0., 0., 0.]
    texels = {}
    for dx in (0, 1):
        for dy in (0, 1):
            for dz in (0, 1):
                delta = (dx, dy, dz)
                weight = math.prod(fraction[i] if delta[i] else 1-fraction[i] for i in range(3))
                x, y, z = [min(n-1, max(0, lo[i]+delta[i])) for i in range(3)]
                offset = 8*(face*n**3+z+n*(y+n*x))+4
                texels[delta] = faces[offset:offset+4]
                for c in range(4):
                    result[c] += faces[offset+c]*weight
    # Bound the permitted texture-coordinate snap using local field gradients.
    # RTX 5080 vulkaninfo records subTexelPrecisionBits=8. This is independent
    # of the shader and does not fit a tolerance to the measured lighting error.
    # https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html
    quantum = 2**-CONTRACT['subtexel_precision_bits']
    bound = [0., 0., 0., 0.]
    for axis in range(3):
        for c in range(4):
            difference = 0.
            for delta, value in texels.items():
                if delta[axis] == 0:
                    other = list(delta); other[axis] = 1
                    difference = max(difference, abs(value[c]-texels[tuple(other)][c]))
            bound[c] += quantum*difference
    return result, bound


def analyze(prefix, expected_flags=0, expected_normal=None, allow_dynamic=False):
    meta, lighting = hdr.load_lighting(prefix)
    stat = json.loads(Path(str(prefix)+'.static.json').read_text())
    assert stat['fallback_flags'] == expected_flags, stat
    assert meta['method'] == ('cone' if expected_flags else 'dynamic_voxel'), meta['method']
    surface_bytes = Path(str(prefix)+'.surface.bin').read_bytes()
    surface = array.array('f'); surface.frombytes(surface_bytes)
    static_bytes = Path(str(prefix)+'.static.bin').read_bytes()
    faces = array.array('f'); faces.frombytes(static_bytes)
    dynamic_faces = faces[stat['offsets']['dynamic_faces']//4:] if allow_dynamic else None
    n = stat['resolution']; width, height = meta['width'], meta['height']
    extent = meta['gbuffer_extent']; minimum = stat['minimum_cell_size']
    valid = ready = changed_normals = probes = 0
    max_error = max_normal_error = 0.
    tuples = {}
    stride = max(1, width*height//CONTRACT['samples_per_image'])
    for pixel in range(width*height):
        offset = pixel*24
        if surface[offset+3] == 0:
            assert not any(lighting[pixel*28:pixel*28+28]), 'Background has lighting'
            continue
        valid += 1
        ready += int(surface[offset+22] == 1)
        position = surface[offset:offset+3]
        normal = surface[offset+4:offset+7]
        geometric = surface[offset+8:offset+11]
        identity, object_class, tx, ty = struct.unpack_from('<4I', surface_bytes, (offset+12)*4)
        assert identity and (object_class in (1, 2) if allow_dynamic else object_class == 1)
        x, y = pixel % width, pixel//width
        assert (tx, ty) == (min(extent-1, int((x+.5)*extent/width)),
                            min(extent-1, int((y+.5)*extent/height)))
        key = (tx, ty)
        entry = (tuple(surface[offset:offset+12]), tuple(lighting[pixel*28+4:pixel*28+8]))
        if key in tuples:
            assert tuples[key] == entry, 'Repeated native texel changed surface/shadow shading'
        tuples[key] = entry
        if expected_normal is not None:
            error = max(abs(geometric[c]-expected_normal[c]) for c in range(3))
            assert error <= CONTRACT['normal_abs'], (identity, geometric, expected_normal)
            max_normal_error = max(max_normal_error, error)
            changed_normals += int(sum((normal[c]-geometric[c])**2 for c in range(3)) > .01)
        if pixel % stride == 0 and not expected_flags:
            assert surface[offset+22] == 1, 'Valid static surface fell back during composition'
            coordinate = [(position[c]-minimum[c])/minimum[3] for c in range(3)]
            direction = normalize(normal)
            weight = [.5*(direction[f//2]**2 + (1 if f%2 == 0 else -1)*direction[f//2]) for f in range(6)]
            sampled = [trilinear(dynamic_faces if object_class == 2 else faces, f, coordinate, n) for f in range(6)]
            irradiance = [max(0, sum(weight[f]*sampled[f][0][c] for f in range(6))) for c in range(3)]
            view = normalize([meta['camera_position'][c]-position[c] for c in range(3)])
            ndotv = max(sum(normal[c]*view[c] for c in range(3)), .001)
            albedo = surface[offset+16:offset+19]; metal = surface[offset+19]; ao = surface[offset+20]
            for c in range(3):
                f0 = .04*(1-metal)+albedo[c]*metal
                fresnel = f0+(1-f0)*(1-ndotv)**5
                expected = irradiance[c]/math.pi*albedo[c]*(1-fresnel)*(1-metal)*ao
                actual = lighting[pixel*28+8+c]
                error = abs(expected-actual)
                sample_bound = sum(abs(weight[f])*sampled[f][1][c] for f in range(6))
                bound = sample_bound/math.pi*albedo[c]*(1-fresnel)*(1-metal)*ao
                assert error <= bound+CONTRACT['diffuse_abs']+CONTRACT['diffuse_rel']*abs(expected), (pixel, expected, actual, bound)
                max_error = max(max_error, error)
            probes += 1
    assert valid > 100
    assert ready == (0 if expected_flags else valid), (ready, valid, expected_flags)
    if expected_normal is not None:
        assert changed_normals > valid*.9, 'Normal map fixture did not perturb shading normals'
    # Every raw-valid cell is occupied. Padding never becomes a donor: compare
    # selected empty cells against the independently enumerated occupied 3^3 mean.
    filtered = bool(stat.get("temporal_mode", 0) or stat.get("spatial_filter", 0))
    padding_checks = 0
    for cell in range(0, n**3, 101):
        x, y, z = cell//(n*n), (cell//n) % n, cell % n
        for face in (0, 4):
            offset = 8*(face*n**3+cell)
            if faces[offset+3] != 0:
                assert faces[offset+7] == 1
                if not filtered: assert faces[offset:offset+4] == faces[offset+4:offset+8]
            elif stat.get('format_version', 1) == 1 or struct.unpack_from('<I', static_bytes, stat['offsets']['static_map']+4*cell)[0] == 0xffffffff:
                donors = []
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        for dz in (-1, 0, 1):
                            q = (x+dx, y+dy, z+dz)
                            if all(0 <= v < n for v in q):
                                donor = 8*(face*n**3+q[2]+n*(q[1]+n*q[0]))
                                if faces[donor+3] == 1:
                                    donors.append(faces[donor+4:donor+7] if filtered else faces[donor:donor+3])
                if donors:
                    for c in range(3):
                        expected = sum(d[c] for d in donors)/len(donors)
                        assert abs(faces[offset+4+c]-expected) <= max(.001, abs(expected)*.0011)
                    assert faces[offset+7] == 1
                    padding_checks += 1
                else:
                    assert not any(faces[offset+4:offset+8])
    stats = dict(valid_pixels=valid, directional_pixels=ready, diffuse_probes=probes,
                 max_diffuse_error=max_error, max_geometric_normal_error=max_normal_error,
                 padding_checks=padding_checks, occupied=stat['occupied'], capacity=stat['capacity'],
                 fallback_flags=stat['fallback_flags'])
    Path(str(prefix)+'.m3-stats.json').write_text(json.dumps(stats, indent=2))
    return stats, tuples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/dynamic-voxel-m3/scenes')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--frames', type=int, default=12, help='Warmup includes bounded static cache initialization')
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    (out/'locked-contract.json').write_text(json.dumps(CONTRACT, indent=2))
    fixtures = out/'fixtures'; fixtures.mkdir(exist_ok=True)
    legacy.OUT = fixtures; legacy.make_fixture()
    room = fixtures/'room.gltf'
    dark = json.loads(room.read_text())
    for material in dark['materials'][1:3]:
        material['pbrMetallicRoughness']['baseColorFactor'] = [0, 0, 0, 1]
    (fixtures/'black-senders.gltf').write_text(json.dumps(dark))
    cfg = ROOT/'Data/engine.cfg'; original = cfg.read_bytes(); last = original
    env = os.environ.copy(); env.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}

    def run(tag, model=room, voxelizer='comp', thread=0, queue=0, changes=None,
            extent=(320, 180, 256), flags=0, normal=None, dynamic=True, frames=None,
            mode=3, fallback='cone'):
        nonlocal last
        settings = dict(default_model_path=model.as_posix(), camera_position='0,0,2', voxelizer=voxelizer,
            voxel_resolution=64, voxel_reflectance_policy='averaged', voxel_reflectance_budget_mb=512,
            voxel_gi_method='dynamic_voxel', dynamic_voxel_gi_query_backend='voxel_dda',
            dynamic_voxel_gi_temporal_filter='off', dynamic_voxel_gi_spatial_filter='false',
            dynamic_voxel_gi_memory_budget_mb=3072, environment_lighting='false', skybox_visible='false',
            light_count=1, voxel_gi_indirect_intensity=1)
        settings.update({'dynamic_light.enabled':'false', 'light_markers.enabled':'false',
            'light.0.type':'point', 'light.0.position':'0,0.1,0.25', 'light.0.color':'1,1,1',
            'light.0.intensity':2, 'light.0.range':4, 'light.0.enabled':'true', 'light.0.casts_shadows':'true'})
        settings.update(changes or {})
        assert cfg.read_bytes() == last, 'Config changed externally'
        last = original+b'\n'+''.join(f'{k}={v}\n' for k, v in settings.items()).encode()
        cfg.write_bytes(last)
        prefix = out/tag; Path(str(prefix)+'.cfg').write_bytes(last)
        command = [str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'), f'--frames={args.frames if frames is None else frames}', f'--mode={mode}',
            f'--rhi-thread={thread}', f'--async-compute={queue}', f'--width={extent[0]}',
            f'--height={extent[1]}', f'--gbuffer-size={extent[2]}',
            f'--capture-lighting={prefix}', f'--capture={prefix}.ppm']
        with Path(str(prefix)+'.log').open('w') as log:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
        text = Path(str(prefix)+'.log').read_text(errors='replace')
        errors = [line for line in text.splitlines() if any(t in line for t in ('[error]', 'VUID-', 'SYNC-HAZARD'))]
        assert result.returncode == 0 and not errors, (tag, result.returncode, errors[:3])
        assert '[OK] No memory leaks detected' in text
        stats = hdr.analyze(prefix, 'room' if model == room else 'fixture')
        tuples = {}
        if dynamic:
            static, tuples = analyze(prefix, flags, normal)
            stats.update(static)
        else:
            assert hdr.load_lighting(prefix)[0]['method'] == fallback
        results[tag] = stats
        print('PASS', tag, flush=True)
        return prefix, tuples

    try:
        base, _ = run('room-comp-t0-q0')
        if not args.quick:
            reference = Path(str(base)+'.lighting.bin').read_bytes()
            for voxelizer in ('comp', 'geom'):
                for thread, queue in ((0, 0), (0, 1), (1, 0), (1, 1)):
                    if (voxelizer, thread, queue) != ('comp', 0, 0):
                        prefix, _ = run(f'room-{voxelizer}-t{thread}-q{queue}', voxelizer=voxelizer, thread=thread, queue=queue)
                        assert Path(str(prefix)+'.lighting.bin').read_bytes() == reference
                run('room-'+voxelizer+'-owner', voxelizer=voxelizer, changes=dict(voxel_reflectance_policy='owner'))
            zero, _ = run('zero-indirect', changes=dict(voxel_gi_indirect_intensity=0))
            _, lighting = hdr.load_lighting(zero)
            assert all(lighting[i+c] == 0 for i in range(8, len(lighting), 28) for c in range(3))
            for component in (1, 3, 4, 5): hdr.compare_component(base, zero, component)
            doubled, _ = run('double-indirect', changes=dict(voxel_gi_indirect_intensity=2))
            _, before = hdr.load_lighting(base); _, after = hdr.load_lighting(doubled)
            assert max(abs(after[i+c]-2*before[i+c]) for i in range(8, len(after), 28) for c in range(3)) < 1e-6
            black, _ = run('black-senders', model=fixtures/'black-senders.gltf')
            floor = hdr.analyze(black, 'room')['regions']['floor']['component_mean_rgb'][2]
            colored = results[base.name]['regions']['floor']['component_mean_rgb'][2]
            assert colored[0] > floor[0]*1.1 and colored[1] > floor[1]*1.1, (colored, floor)
            markers, _ = run('markers', changes={'light_markers.enabled':'true'})
            assert Path(str(markers)+'.lighting.bin').read_bytes() == reference
            assert Path(str(markers)+'.ppm').read_bytes() != Path(str(base)+'.ppm').read_bytes()
            run('cache-initializing', frames=1, flags=8)
            run('capacity-overflow', changes=dict(dynamic_voxel_gi_memory_budget_mb=200), flags=1)
            run('point-shadows-disabled', changes={'voxel_gi_shadow_enabled':'false'})
            run('directional', changes={'light.0.type':'directional', 'light.0.direction':'0.1,-1,-0.2'})
            run('spot', changes={'light.0.type':'spot', 'light.0.direction':'0,-1,-1',
                'light.0.inner_angle_degrees':20, 'light.0.outer_angle_degrees':40})
            for tag, changes in (('rt-unavailable', dict(dynamic_voxel_gi_query_backend='hardware_rt')),
                ('preflight', dict(dynamic_voxel_gi_memory_budget_mb=16))):
                requested, _ = run('fallback-'+tag, changes=changes, dynamic=False)
                cone, _ = run('cone-'+tag, changes=dict(changes, voxel_gi_method='cone'), dynamic=False)
                assert Path(str(requested)+'.lighting.bin').read_bytes() == Path(str(cone)+'.lighting.bin').read_bytes()
            exhausted, _ = run('budget-exhausted', changes=dict(dynamic_voxel_gi_memory_budget_mb=1),
                               dynamic=False, fallback='pbr')
            pbr, _ = run('explicit-pbr', dynamic=False, mode=2, fallback='pbr')
            assert Path(str(exhausted)+'.lighting.bin').read_bytes() == Path(str(pbr)+'.lighting.bin').read_bytes()
            assert hdr.load_lighting(exhausted)[0]['voxel_geometry_generation'] == 0
            for slope, mirrored in ((0., False), (.3, False), (.3, True)):
                model, normal = normal_fixture(fixtures, slope, mirrored)
                common = {'light.0.position':'.1,.1,.35', 'light_markers.enabled':'false'}
                first, native = run(f'normal-{slope}-{mirrored}-native', model=model, changes=common,
                                     extent=(256, 256, 256), normal=normal)
                for name, extent in (('2x', (512, 512, 256)), ('1.5x', (384, 384, 256)), ('odd', (515, 321, 257))):
                    _, tuples = run(f'normal-{slope}-{mirrored}-{name}', model=model, changes=common, extent=extent, normal=normal)
                    if name != 'odd':
                        assert set(tuples) == set(native)
                        for key in native:
                            assert native[key][0] == tuples[key][0]
                            assert max(abs(a-b) for a,b in zip(native[key][1], tuples[key][1])) <= CONTRACT['direct_same_texel_abs']
    finally:
        assert cfg.read_bytes() == last, 'Config changed externally; preserving it'
        cfg.write_bytes(original)
        (out/'results.json').write_text(json.dumps(results, indent=2))
        assert hashlib.sha256(cfg.read_bytes()).digest() == hashlib.sha256(original).digest()


if __name__ == '__main__':
    main()
