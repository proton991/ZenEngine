"""M7 locked image-quality gates against independent CPU triangle transport.

Install requirements-gi-quality.txt. Runs serially, restores exact engine.cfg bytes,
retains linear HDR/reference/mask buffers, and exits nonzero on failed quality gates.
Use --compare-only to re-evaluate retained captures without touching the engine.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np
from PIL import Image

import validate_voxel_gi as legacy
import validate_voxelization as calibration
from voxelization_materials import MaterialOracle
from voxelization_fixtures import Fixture, rgba_png
import voxel_gi_triangle_reference as reference

ROOT = Path(__file__).resolve().parents[1]
LIMITS = Path(__file__).with_name('voxel_gi_quality_limits.json')
COVERAGE_CACHE = {}


def fixture(folder, name):
    asset = Fixture()
    asset.document['materials'] = [dict(pbrMetallicRoughness=dict(
        baseColorFactor=[.7, .7, .7, 1], metallicFactor=0, roughnessFactor=1)),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0),
             emissiveFactor=[1, 0, 0], extensions={'KHR_materials_emissive_strength': {'emissiveStrength': 4}}),
        dict(pbrMetallicRoughness=dict(baseColorFactor=[0, 0, 0, 1], metallicFactor=0))]
    asset.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])

    def panel(x0, x1, y0, y1, z, slope, material, reverse=False):
        points = [(x, y, z+slope*x) for x, y in ((x0,y0), (x1,y0), (x1,y1), (x0,y1))]
        corners = ((2,1,0), (3,2,0)) if reverse else ((0,1,2), (0,2,3))
        tex = [(0,0), (1,0), (1,1), (0,1)]
        asset.mesh([[points[i] for i in t] for t in corners], material=material,
                   uv0=[tex[i] for t in corners for i in t])

    panel(-.3, .3, -.3, .3, -.23, 0, 0)
    panel(-.24, -.10, -.10, .10, .18, 0, 1)
    if name == 'cutout':
        (folder/'cutout.png').write_bytes(rgba_png(4, 4,
            [(255,255,255,255 if (x+y)%2 else 0) for y in range(4) for x in range(4)]))
        asset.document.update(images=[dict(uri='cutout.png')], textures=[dict(source=0)])
        asset.document['materials'][2].update(alphaMode='MASK', alphaCutoff=.5)
        asset.document['materials'][2]['pbrMetallicRoughness']['baseColorTexture'] = dict(index=0)
    if name != 'emitter':
        panel(-.06, .06, -.18, .18, -.07, .8 if name == 'slanted' else 0, 2, name == 'backface')
    return asset.save(folder, name)


def umbra(surface, name):
    """Every segment to the rectangular emitter crosses the opaque blocker interior."""
    inside = np.ones(len(surface), dtype=bool)
    slope = .8 if name == 'slanted' else 0
    p = surface[:, :3]
    for x, y in itertools.product((-.24, -.10), (-.10, .10)):
        delta = np.array([x, y, .18])-p
        t = (-.07+slope*p[:, 0]-p[:, 2])/(delta[:, 2]-slope*delta[:, 0])
        hit = p+t[:, None]*delta
        # Margin excludes the analytical shadow boundary from the leakage gate.
        inside &= (t > 0) & (t < 1) & (np.abs(hit[:, 0]) < .055) & (np.abs(hit[:, 1]) < .175)
    return inside


def capture(out, tag, asset, n, producer, filtered, original, frames=64, temporal='off', overrides=None):
    config = ROOT/'Data/engine.cfg'
    is_sponza = tag.startswith('sponza')
    is_room = tag.startswith('room')
    settings = dict(default_model_path=asset.as_posix(), camera_position='.55,-.15,0' if is_sponza else '0,0,2',
        voxelizer=producer, voxel_resolution=n, voxel_reflectance_policy='averaged', voxel_reflectance_budget_mb=512,
        voxel_gi_method='dynamic_voxel', dynamic_voxel_gi_query_backend='voxel_dda',
        dynamic_voxel_gi_memory_budget_mb=12288 if n == 128 else 6144,
        dynamic_voxel_gi_temporal_filter=temporal, dynamic_voxel_gi_spatial_filter=str(filtered).lower(),
        dynamic_voxel_gi_analytic_lighting='true', dynamic_voxel_gi_emissive_lighting='true',
        dynamic_voxel_gi_environment_lighting='false', environment_lighting='false', skybox_visible='false',
        voxel_gi_indirect_intensity=1, voxel_gi_shadow_enabled='true', light_count=5 if is_sponza else (1 if is_room else 0))
    settings.update({'dynamic_light.enabled':'false', 'light_markers.enabled':'false'})
    positions = ('-.55,-.25,-.13', '.55,-.25,-.13', '-.55,-.25,.13', '.55,-.25,.13', '1,1,0') if is_sponza else ('0,.2,.3',)
    for i, position in enumerate(positions):
        settings.update({f'light.{i}.type':'point', f'light.{i}.position':position,
            f'light.{i}.color':'.2,.4,1' if is_sponza and i == 4 else '1,1,1',
            f'light.{i}.intensity':5 if is_sponza and i < 4 else 2,
            f'light.{i}.range':1000 if is_sponza and i < 4 else 4,
            f'light.{i}.enabled':'true', f'light.{i}.casts_shadows':'true'})
    settings.update(overrides or {})
    assert config.read_bytes() == original, 'Configuration changed externally'
    edited = original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode()
    prefix = out/tag
    Path(str(prefix)+'.cfg').write_bytes(edited)
    config.write_bytes(edited)
    command = [str(ROOT/'build/x64-windows-msvc-release/bin/scene_renderer_demo.exe'), '--disable-rt',
        f'--frames={frames}', '--fixed-step', '--mode=3', '--rhi-thread=1', '--async-compute=1',
        '--width=128', '--height=96', '--gbuffer-size=128', f'--capture-lighting={prefix}', f'--capture-voxels={prefix}']
    environment = os.environ.copy()
    environment.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    start = time.time()
    try:
        with Path(str(prefix)+'.log').open('w') as log:
            process = subprocess.run(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    finally:
        assert config.read_bytes() == edited, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
    log = Path(str(prefix)+'.log').read_text(errors='replace')
    errors = [line for line in log.splitlines() if any(token in line for token in ('[error]', 'VUID-', 'SYNC-HAZARD'))]
    assert process.returncode == 0 and not errors, (tag, process.returncode, errors[:3])
    assert '[OK] No memory leaks detected' in log
    assert all('Enabled Device Extension: '+extension not in log for extension in (
        'VK_KHR_acceleration_structure', 'VK_KHR_ray_tracing_pipeline', 'VK_KHR_ray_query'))
    status = json.loads(Path(str(prefix)+'.static.json').read_text())
    assert status['fallback_flags'] == 0 and status['cache_ready_receivers'] == status['occupied']
    print(f'CAPTURE {tag} ({time.time()-start:.1f}s)', flush=True)
    return prefix


def make_reference(out, name, prefix, asset, limits, refresh):
    meta, actual, surface, selected = reference.load_capture(prefix)
    if name not in ('room', 'sponza'):
        selected &= np.abs(surface[:, 2]+.23) < .001
    assert np.count_nonzero(selected) >= limits['minimum_pixels']
    base = out/(name+'-reference')
    # Input identity deliberately excludes voxel metadata, so producers/resolutions
    # must share exactly the same primary surface and light contract to reuse it.
    geometry = hashlib.sha256()
    for suffix in ('.vertices.bin', '.indices.bin', '.nodes.bin', '.triangles.bin'):
        geometry.update(Path(str(prefix)+suffix).read_bytes())
    source = hashlib.sha256(asset.read_bytes())
    document = json.loads(asset.read_text())
    for entry in document.get('images', []) + document.get('buffers', []):
        if 'uri' in entry and not entry['uri'].startswith('data:'):
            source.update((asset.parent/entry['uri']).read_bytes())
    identity = hashlib.sha256(surface.tobytes()+selected.tobytes()+geometry.digest()+source.digest()+
        json.dumps(meta['lights'], sort_keys=True).encode()+
        json.dumps(meta['camera_position']).encode()+LIMITS.read_bytes()+
        Path(__file__).with_name('voxel_gi_triangle_reference.py').read_bytes()).hexdigest()
    path = Path(str(base)+'.json')
    if path.exists() and not refresh:
        result = json.loads(path.read_text())
        assert result['input_sha256'] == identity, 'Reference inputs changed; rerun with --refresh-reference'
        values = np.fromfile(str(base)+'.rgb.f32', '<f4').reshape(-1, 3)
    else:
        scene = reference.TriangleScene.from_capture(prefix, asset)
        samples = limits['reference_samples_per_replica']
        while True:
            start = time.time()
            a, b = [reference.integrate(scene, surface[selected], meta, samples, seed,
                limits['primary_offset_world'], limits['secondary_offset_world']) for seed in (137, 7331)]
            values = (a+b)*.5
            uncertainty = reference.error_metrics(a, b)['relative_rmse'] / np.sqrt(2)
            print(f'REFERENCE {name}: {samples} x 2 samples, uncertainty {uncertainty:.4%}, {time.time()-start:.1f}s', flush=True)
            if uncertainty <= limits['reference_relative_rmse_limit'] or samples >= limits['reference_max_samples_per_replica']:
                break
            samples *= 2
        result = dict(input_sha256=identity, samples_per_replica=samples, replicas=2,
                      relative_replica_error=float(uncertainty), pixels=int(selected.sum()),
                      geometry_sha256=geometry.hexdigest(), asset_sha256=source.hexdigest(),
                      converged=bool(uncertainty <= limits['reference_relative_rmse_limit']))
        full = np.zeros_like(actual); full[selected] = values
        full.astype('<f4').tofile(str(base)+'.rgb.f32')
        selected.astype('u1').tofile(str(base)+'.mask.u8')
        path.write_text(json.dumps(result, indent=2))
        values = full
    return meta, actual, surface, selected, values, result


def compare(out, name, prefix, asset, limits, refresh=False):
    meta, actual, surface, selected, expected, oracle = make_reference(out, name, prefix, asset, limits, refresh)
    n = str(meta['voxel_resolution'])
    family = name if name in ('room', 'sponza') else 'fixture'
    result = reference.error_metrics(actual[selected], expected[selected])
    result.update(pixels=int(selected.sum()), reference=oracle, checks={})
    result['checks'].update(reference_converged=oracle['converged'],
        relative_rmse=result['relative_rmse'] <= limits['relative_rmse'][family][n],
        relative_mean_bias=abs(result['relative_mean_bias']) <= limits['relative_mean_bias'][n])
    if family == 'fixture':
        result['coverage'] = fixture_coverage(prefix, asset)
        result['checks']['cell_coverage'] = all(result['coverage'][key] == 0 for key in ('missing', 'extra', 'wrong_owner'))
    result['regions'] = {}
    if name in ('room', 'sponza'):
        normals = reference.normalize(surface[:, 8:11])
        for region, direction in (('floor', [0,1,0]), ('ceiling', [0,-1,0]), ('positive_x', [1,0,0]),
                                  ('negative_x', [-1,0,0]), ('positive_z', [0,0,1]), ('negative_z', [0,0,-1])):
            mask = selected & (normals @ direction > .9)
            if mask.sum() >= 8:
                result['regions'][region] = dict(pixels=int(mask.sum()), **reference.error_metrics(actual[mask], expected[mask]))
    if name in ('thin', 'slanted', 'backface'):
        dark = selected & umbra(surface, name)
        assert dark.sum() >= 8, 'Fixture must expose a measurable analytical umbra'
        assert np.max(expected[dark]) == 0, 'Independent integration disagrees with analytical umbra'
        fullscale = 4*.7*.96
        mean, maximum = float(actual[dark, 0].mean()/fullscale), float(actual[dark, 0].max()/fullscale)
        result['umbra'] = dict(pixels=int(dark.sum()), fullscale=fullscale, normalized_mean=mean, normalized_max=maximum)
        result['checks'].update(umbra_mean=mean <= limits['umbra_fullscale_mean_limit'],
                                umbra_max=maximum <= limits['umbra_fullscale_max_limit'])
    # Shared display scale for reference, actual and absolute error. Linear files
    # are the acceptance evidence; this contact sheet is only for visual inspection.
    scale = max(float(np.quantile(expected[selected], .99)), 1e-6)
    panels = [np.where(selected[:, None], v, 0).reshape(meta['height'], meta['width'], 3)
              for v in (expected, actual, np.abs(actual-expected))]
    image = np.concatenate(panels, axis=1)
    Image.fromarray((np.clip(image/scale, 0, 1)**(1/2.2)*255).astype('u1')).resize(
        (meta['width']*6, meta['height']*2)).save(str(prefix)+'.quality.png')
    return result


def fixture_coverage(prefix, asset):
    """Reuse V0's double-precision clipping/alpha oracle, independently of GI error."""
    grid = json.loads(Path(str(prefix)+'.json').read_text())
    n = grid['resolution']
    words = np.memmap(str(prefix)+'.voxels.bin', dtype='<u4', mode='r').reshape(-1,8)
    key = (asset.name, n, hashlib.sha256(words[:,0].tobytes()).hexdigest())
    if key not in COVERAGE_CACHE:
        document = json.loads(asset.read_text())
        textures = []
        for texture in document.get('textures', []):
            pixels = np.asarray(Image.open(asset.parent/document['images'][texture['source']]['uri']).convert('RGBA'))
            textures.append(dict(width=pixels.shape[1], height=pixels.shape[0], pixels=pixels.reshape(-1,4).tolist()))
        triangles = calibration.load_triangles(prefix, grid)
        material = MaterialOracle(prefix, grid, triangles, dict(materials=document['materials']+[{}], textures=textures))
        certain, possible = {}, {}
        ambiguous_alpha = set()
        for owner, triangle in enumerate(triangles):
            for cell in calibration.candidate_cells(triangle, n):
                expanded = bool(calibration.clip_cell(triangle, cell, calibration.AMBIGUITY))
                if expanded:
                    value = material.surface(owner, [x+.5 for x in cell])
                    if value['visible'] or value['alpha_ambiguous']:
                        possible.setdefault(cell, owner)
                    if value['alpha_ambiguous']:
                        ambiguous_alpha.add(cell)
                    if value['visible'] and not value['alpha_ambiguous'] and calibration.clip_cell(triangle, cell, -calibration.AMBIGUITY):
                        certain.setdefault(cell, owner)
        ids = np.flatnonzero(words[:,0] != 0xffffffff)
        actual = {(int(i%n), int(i//n%n), int(i//(n*n))):int(words[i,0]) for i in ids}
        missing = set(certain)-set(actual); extra = set(actual)-set(possible)
        wrong = [cell for cell in set(actual)&set(possible)
                 if not possible[cell] <= actual[cell] <= certain.get(cell, 0xffffffff)]
        COVERAGE_CACHE[key] = dict(occupied=len(actual), certain=len(certain), possible=len(possible),
            alpha_ambiguous=len(ambiguous_alpha), missing=len(missing), extra=len(extra), wrong_owner=len(wrong),
            missing_cells=sorted(missing), extra_cells=sorted(extra), wrong_owner_cells=sorted(wrong))
    return COVERAGE_CACHE[key]


def temporal_scene(out, name, asset, producer, original, limits, compare_only, overrides=None):
    images = []
    _, baseline, baseline_surface, baseline_selected = reference.load_capture(out/f'{name}-64-{producer}-spatial')
    for frames in (320, 352, 384):
        tag = f'{name}-64-{producer}-temporal{frames}'
        prefix = out/tag
        if not compare_only:
            capture(out, tag, asset, 64, producer, True, original, frames, 'fixed', overrides)
        meta, actual, surface, selected = reference.load_capture(prefix)
        assert surface.tobytes() == baseline_surface.tobytes() and np.array_equal(selected, baseline_selected)
        status = json.loads(Path(str(prefix)+'.static.json').read_text())
        assert status['temporal_mode'] == 1 and status['spatial_filter'] == 1
        images.append(actual[selected])
    samples = np.stack(images).astype(np.float64)
    relative_rms = float(np.linalg.norm(np.std(samples, axis=0)) / max(np.linalg.norm(samples.mean(axis=0)), 1e-15))
    spatial = baseline[selected]
    convergence = reference.error_metrics(samples[-1], spatial)['relative_rmse']
    result = dict(frames=[320,352,384], pixels=int(selected.sum()), relative_stationary_rms=relative_rms,
                  relative_error_to_spatial=convergence, checks=dict(
                      stationary=relative_rms <= limits['stationary_relative_rms_limit'],
                      converged=convergence <= limits['stationary_relative_rms_limit']))
    print(('PASS ' if all(result['checks'].values()) else 'FAIL ')+f'{name}-{producer}-temporal: '+json.dumps(result), flush=True)
    return result


def light_visibility_diagnostics(prefix, asset):
    """Diagnose a failed quality gate; direct-segment controls are not GI truth."""
    meta, _, surface, selected = reference.load_capture(prefix)
    grid = json.loads(Path(str(prefix)+'.json').read_text())
    n = grid['resolution']; cells = n**3
    minimum, size = np.array(grid['grid_min']), grid['voxel_size']
    owner = np.memmap(str(prefix)+'.voxels.bin', dtype='<u4', mode='r').reshape(n,n,n,8)[:,:,:,0]
    occupied = (owner != 0xffffffff).transpose(2,1,0).ravel()
    masks = np.memmap(str(prefix)+'.static.bin', dtype='<u4', offset=192*cells, shape=(cells,), mode='r')
    scene = reference.TriangleScene.from_capture(prefix, asset)
    positions = surface[selected, :3] + reference.normalize(surface[selected, 8:11])*.0006
    lights = []
    for index, light in enumerate(meta['lights']):
        position = np.array(light['position_range'][:3])
        cell = np.floor((position-minimum)/size).astype(int)
        inside = bool(np.all(cell >= 0) and np.all(cell < n))
        record = int(owner[cell[2],cell[1],cell[0]]) if inside else 0xffffffff
        direction, distance, _ = reference.light_incident(light, positions)
        visible = scene.trace(positions, direction, np.maximum(distance-1e-5, 0))[0] < 0
        floor = scene.trace(position[None, :], np.array([[0.,-1,0]]))[1][0]
        lights.append(dict(index=index, cell=cell.tolist(), occupied=record != 0xffffffff,
            owner_triangle=record if record != 0xffffffff else None,
            occupied_cells_with_light_bit=int(np.count_nonzero(occupied & ((masks & (1 << index)) != 0))),
            triangle_visible_primary_pixels=int(visible.sum()),
            downward_triangle_distance=float(floor) if np.isfinite(floor) else None))
    result = dict(cell_size=size, occupied_cells=int(occupied.sum()), primary_pixels=int(selected.sum()), lights=lights,
        note='Triangle primary segments are a direct-visibility control, not GI truth. Analytic visibility: '+meta.get('analytic_visibility', 'voxel_dda_corner_majority'))
    Path(str(prefix)+'.light-visibility.json').write_text(json.dumps(result, indent=2))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/dynamic-voxel-m7-quality-20260925/scenes')
    parser.add_argument('--cases', nargs='+', default=['emitter', 'thin', 'slanted', 'cutout', 'backface', 'room', 'sponza'],
                        choices=['emitter', 'thin', 'slanted', 'cutout', 'backface', 'room', 'sponza'])
    parser.add_argument('--resolutions', type=int, nargs='+', default=[64, 128], choices=[64, 128])
    parser.add_argument('--producers', nargs='+', default=['comp', 'geom'], choices=['comp', 'geom'])
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE',
                        help='Capture overrides for quality/performance preset comparisons')
    parser.add_argument('--refresh-reference', action='store_true')
    parser.add_argument('--temporal-scenes', action='store_true', help='Also measure converged stationary room/Sponza images')
    parser.add_argument('--temporal-only', action='store_true', help='Use existing spatial baselines and capture only stationary room/Sponza sequences')
    args = parser.parse_args()
    assert all('=' in item and '\n' not in item for item in args.set)
    overrides = dict(item.split('=', 1) for item in args.set)
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    folder = out/'fixtures'; folder.mkdir(exist_ok=True)
    contract = LIMITS.read_bytes()
    locked = out/'quality-limits.lock.json'
    if locked.exists():
        assert locked.read_bytes() == contract, 'Locked acceptance thresholds changed'
    else:
        locked.write_bytes(contract)
    limits = json.loads(contract)
    original = (ROOT/'Data/engine.cfg').read_bytes()
    (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())
    legacy.OUT = folder; legacy.make_fixture()
    result_path = out/'results.json'
    results = json.loads(result_path.read_text()) if result_path.exists() else {}
    for name in args.cases:
        asset = ((ROOT/'../glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf').resolve() if name == 'sponza' else
                 folder/'emissive.gltf' if name == 'room' else fixture(folder, name))
        refreshed = False
        for n, producer in (() if args.temporal_only else itertools.product(args.resolutions, args.producers)):
            pair = {}
            for filtered in (False, True):
                tag = f'{name}-{n}-{producer}-'+('spatial' if filtered else 'raw')
                prefix = out/tag
                if not args.compare_only:
                    capture(out, tag, asset, n, producer, filtered, original, overrides=overrides)
                result = compare(out, name, prefix, asset, limits, args.refresh_reference and not refreshed)
                refreshed = True
                pair[filtered] = result
                if filtered and 'umbra' in result:
                    added = result['umbra']['normalized_mean']-pair[False]['umbra']['normalized_mean']
                    result['umbra']['filter_added_normalized_mean'] = added
                    result['checks']['filter_added_umbra'] = added <= limits['filter_added_umbra_fullscale_mean_limit']
                if name == 'sponza' and not filtered:
                    result['light_visibility'] = light_visibility_diagnostics(prefix, asset)
                Path(str(prefix)+'.quality.json').write_text(json.dumps(result, indent=2))
                print(('PASS ' if all(result['checks'].values()) else 'FAIL ')+prefix.name+
                      f": NRMSE={result['relative_rmse']:.2%}, bias={result['relative_mean_bias']:+.2%}", flush=True)
                results[tag] = result
                result_path.write_text(json.dumps(results, indent=2))
        if (args.temporal_scenes or args.temporal_only) and name in ('room', 'sponza'):
            for producer in args.producers:
                results[f'{name}-64-{producer}-temporal'] = temporal_scene(
                    out, name, asset, producer, original, limits, args.compare_only, overrides)
                result_path.write_text(json.dumps(results, indent=2))
    failed = [name for name, result in results.items() if not all(result['checks'].values())]
    print(json.dumps(dict(cases=len(results), failed=failed, limits_sha256=hashlib.sha256(contract).hexdigest()), indent=2))
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
