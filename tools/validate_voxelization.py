"""Raw-volume voxelization calibration; outputs stay under build/voxelization-calibration.

Uses double-precision polygon clipping, independent of the production triangle-box SAT.
The diagnostic's actual GPU vertex/index/node/triangle buffers define the oracle input.
"""

import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import zlib
from voxelization_fixtures import geometry_fixture, material_fixture
from voxelization_materials import MaterialOracle, compare_gbuffer
from voxelization_reflectance import compare_reflectance, mixture_fixture

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/voxelization-calibration"
EMPTY = 0xffffffff
AMBIGUITY = 1e-5  # grid units, applied to cell boundaries only
OPAQUE_CACHE = {}


def clip_polygon(points, axis, boundary, keep_greater):
    """Sutherland-Hodgman clipping in double precision, retaining boundary touches."""
    result = []
    if points:
        previous = points[-1]
        before = previous[axis] - boundary
        for current in points:
            after = current[axis] - boundary
            previous_inside = before >= 0 if keep_greater else before <= 0
            current_inside = after >= 0 if keep_greater else after <= 0
            if previous_inside != current_inside:
                fraction = before / (before - after)
                result.append(tuple(previous[i] + fraction * (current[i] - previous[i])
                                    for i in range(3)))
            if current_inside:
                result.append(current)
            previous, before = current, after
    return result


def clip_cell(points, cell, margin=0):
    for axis in range(3):
        points = clip_polygon(points, axis, cell[axis] - margin, True)
        points = clip_polygon(points, axis, cell[axis] + 1 + margin, False)
    return points


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def sub(a, b):
    return tuple(x-y for x, y in zip(a, b))


def candidate_cells(triangle, dimension):
    """Clip projected slabs first, then enumerate depth from the clipped polygon."""
    normal = cross(sub(triangle[1], triangle[0]), sub(triangle[2], triangle[0]))
    depth = max(range(3), key=lambda i: abs(normal[i]))
    if abs(normal[depth]) <= 1e-12:
        return
    x, y = (depth+1) % 3, (depth+2) % 3
    lo = [max(0, math.ceil(min(p[i] for p in triangle)-AMBIGUITY)-1) for i in range(3)]
    hi = [min(dimension-1, math.floor(max(p[i] for p in triangle)+AMBIGUITY)) for i in range(3)]
    for cx in range(lo[x], hi[x]+1):
        strip = clip_polygon(triangle, x, cx-AMBIGUITY, True)
        strip = clip_polygon(strip, x, cx+1+AMBIGUITY, False)
        for cy in range(lo[y], hi[y]+1):
            column = clip_polygon(strip, y, cy-AMBIGUITY, True)
            column = clip_polygon(column, y, cy+1+AMBIGUITY, False)
            if column:
                near = max(0, math.ceil(min(p[depth] for p in column)-AMBIGUITY)-1)
                far = min(dimension-1, math.floor(max(p[depth] for p in column)+AMBIGUITY))
                for cz in range(near, far+1):
                    cell = [0, 0, 0]
                    cell[x], cell[y], cell[depth] = cx, cy, cz
                    yield tuple(cell)


def write_fixture(folder, dimension):
    triangles = []
    # Identical-point primitives fix bounds without contributing occupied cells.
    triangles.extend([[(0., 0., 0.)]*3, [(dimension-2.,)*3]*3])
    samples = [
        [(10.1, 10.1, 10.2), (10.3, 10.1, 10.2), (10.1, 10.3, 10.2)],
        [(15.0, 15.0, 15.0), (19.0, 15.0, 15.0), (15.0, 19.0, 15.0)],
        [(22.1, 22.1, 22.2), (28.8, 22.2, 28.8), (22.2, 28.8, 28.8)],
        [(35.01, 35.02, 35.03), (41.99, 41.98, 35.04), (41.98, 41.99, 35.05)],
        [(45.25, 10.25, 12.1), (48.75, 10.25, 12.1), (45.25, 13.75, 12.1)],
    ]
    for permutation in ((0, 1, 2), (1, 2, 0), (2, 0, 1)):
        for sample in samples:
            triangles.append([tuple(p[i]-1 for i in permutation) for p in sample])
            triangles.append(list(reversed(triangles[-1])))
    # Fixed seed, opaque triangles including subpixel areas and projection ties.
    import random
    rng = random.Random(9212026)
    for _ in range(30):
        center = [rng.uniform(4, dimension-10) for _ in range(3)]
        triangles.append([tuple(center[i]+rng.uniform(-2, 2) for i in range(3))
                          for _ in range(3)])
    positions = [p for triangle in triangles for p in triangle]
    normals = []
    for triangle in triangles:
        normal = cross(sub(triangle[1], triangle[0]), sub(triangle[2], triangle[0]))
        length = math.sqrt(sum(x*x for x in normal))
        normal = tuple(x/length for x in normal) if length else (0., 0., 1.)
        normals.extend([normal]*3)
    data = bytearray()
    views, accessors = [], []
    for values, kind, component in ((positions, "VEC3", 5126), (normals, "VEC3", 5126),
                                    (list(range(len(positions))), "SCALAR", 5125)):
        start = len(data)
        flat = [x for row in values for x in (row if isinstance(row, tuple) else (row,))]
        data.extend(struct.pack('<'+('f' if component == 5126 else 'I')*len(flat), *flat))
        views.append(dict(buffer=0, byteOffset=start, byteLength=len(data)-start))
        accessors.append(dict(bufferView=len(views)-1, componentType=component,
                              count=len(values), type=kind))
    accessors[0].update(min=[min(p[i] for p in positions) for i in range(3)],
                        max=[max(p[i] for p in positions) for i in range(3)])
    document = dict(asset={"version": "2.0"},
                    buffers=[dict(uri="analytic.bin", byteLength=len(data))],
                    bufferViews=views, accessors=accessors,
                    materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.2, .6, .8, 1],
                                    metallicFactor=.25, roughnessFactor=1),
                                    emissiveFactor=[1, .5, .25],
                                    extensions={"KHR_materials_emissive_strength": {"emissiveStrength": 4}})],
                    extensionsUsed=["KHR_materials_emissive_strength"],
                    meshes=[dict(primitives=[dict(attributes={"POSITION": 0, "NORMAL": 1},
                                                 indices=2, material=0)])],
                    nodes=[dict(mesh=0)], scenes=[dict(nodes=[0])], scene=0)
    (folder/'analytic.bin').write_bytes(data)
    (folder/'analytic.gltf').write_text(json.dumps(document), encoding='utf-8')
    return folder/'analytic.gltf'


def load_triangles(prefix, metadata):
    vertices = Path(str(prefix)+'.vertices.bin').read_bytes()
    indices = Path(str(prefix)+'.indices.bin').read_bytes()
    nodes = Path(str(prefix)+'.nodes.bin').read_bytes()
    records = Path(str(prefix)+'.triangles.bin').read_bytes()
    result = []
    for first, node, material, _ in struct.iter_unpack('<4I', records[:metadata['triangle_count']*16]):
        matrix = struct.unpack_from('<16f', nodes, node*metadata['node_stride'])
        points = []
        for index in struct.unpack_from('<3I', indices, first*4):
            point = struct.unpack_from('<3f', vertices, index*metadata['vertex_stride'])
            world = [sum(matrix[axis+4*j]*point[j] for j in range(3))+matrix[12+axis]
                     for axis in range(3)]
            points.append(tuple((world[i]-metadata['grid_min'][i])/metadata['voxel_size']
                                for i in range(3)))
        result.append(points)
    assert len(result) == metadata['triangle_count']
    return result


def load_flat_normals(prefix, metadata):
    """Analytic fixture normals, transformed independently with the model cofactor matrix."""
    vertices = Path(str(prefix)+'.vertices.bin').read_bytes()
    indices = Path(str(prefix)+'.indices.bin').read_bytes()
    nodes = Path(str(prefix)+'.nodes.bin').read_bytes()
    records = Path(str(prefix)+'.triangles.bin').read_bytes()
    normals = []
    for first, node, _, _ in struct.iter_unpack('<4I', records[:metadata['triangle_count']*16]):
        matrix = struct.unpack_from('<16f', nodes, node*metadata['node_stride'])
        columns = [matrix[i*4:i*4+3] for i in range(3)]
        cofactors = [cross(columns[1], columns[2]), cross(columns[2], columns[0]),
                     cross(columns[0], columns[1])]
        determinant = sum(columns[0][i]*cofactors[0][i] for i in range(3))
        source = [struct.unpack_from('<3f', vertices, index*metadata['vertex_stride']+16)
                  for index in struct.unpack_from('<3I', indices, first*4)]
        assert source[0] == source[1] == source[2], 'Flat-normal analytic fixture required'
        transformed = [sum(cofactors[j][i]*source[0][j] for j in range(3))/determinant
                       for i in range(3)]
        length = math.sqrt(sum(v*v for v in transformed))
        normals.append(tuple(v/length for v in transformed))
    return normals


def enabled_records(prefix, metadata):
    records = struct.iter_unpack('<4I', Path(str(prefix)+'.triangles.bin').read_bytes()[:metadata['triangle_count']*16])
    return [bool(record[3] & metadata['class_mask']) if 'class_mask' in metadata else True for record in records]


def compare(prefix):
    metadata = json.loads(Path(str(prefix)+'.json').read_text())
    n = metadata['resolution']
    triangles = load_triangles(prefix, metadata)
    enabled = enabled_records(prefix, metadata)
    profile_path = prefix.parent/'calibration-profile.json'
    profile = json.loads(profile_path.read_text())['profile'] if profile_path.exists() else 'analytic'
    assert profile != 'paired_model', 'Capture-only models have no independent material oracle; use voxelization_report.py --other'
    generic = profile == 'opaque_model'
    normals = None if generic else load_flat_normals(prefix, metadata)
    material_oracle = MaterialOracle(prefix, metadata, triangles) if profile == 'materials' or (
        not profile_path.exists() and (prefix.parent/'material-contract.json').exists()) else None
    expected, certain_owner, possible_owner = {}, {}, {}
    alpha_ambiguous, opaque_certain, opaque_possible = set(), set(), set()
    cache_key = None
    if generic:
        digest = hashlib.sha256(json.dumps({k:metadata[k] for k in ('resolution','grid_min','voxel_size','triangle_count')}).encode())
        for suffix in ('vertices','indices','nodes','triangles'):
            digest.update(Path(str(prefix)+'.'+suffix+'.bin').read_bytes())
        cache_key = digest.hexdigest()
        cache_key += str(metadata.get('class_mask', 3))
    cached = OPAQUE_CACHE.get(cache_key)
    for owner, triangle in (() if cached else enumerate(triangles)):
        if not enabled[owner]:
            continue
        for cell in candidate_cells(triangle, n):
            expanded = bool(clip_cell(triangle, cell, AMBIGUITY))
            shrunken = bool(clip_cell(triangle, cell, -AMBIGUITY))
            visible, ambiguous_alpha = True, False
            if material_oracle is not None and expanded:
                surface = material_oracle.surface(owner, [x+.5 for x in cell])
                visible, ambiguous_alpha = surface['visible'], surface['alpha_ambiguous']
            if expanded:
                opaque_possible.add(cell)
                if visible or ambiguous_alpha:
                    possible_owner.setdefault(cell, owner)
                if ambiguous_alpha:
                    alpha_ambiguous.add(cell)
            if shrunken:
                opaque_certain.add(cell)
                if visible and not ambiguous_alpha:
                    certain_owner.setdefault(cell, owner)
            if visible and clip_cell(triangle, cell):
                expected.setdefault(cell, owner)
    if cached:
        expected,certain_owner,possible_owner,opaque_certain,opaque_possible = cached
    elif generic:
        OPAQUE_CACHE[cache_key] = (expected,certain_owner,possible_owner,opaque_certain,opaque_possible)
    certain, possible = set(certain_owner), set(possible_owner)
    occupied = {}
    bad_attributes = []
    data = Path(str(prefix)+'.voxels.bin').read_bytes()
    assert len(data) == n**3*32
    for index, (owner, albedo, normal, occupancy, er, eg, eb, ea) in enumerate(
            struct.iter_unpack('<4I4f', data)):
        cell = (index % n, (index//n) % n, index//(n*n))
        assert occupancy == (owner != EMPTY)
        if occupancy:
            occupied[cell] = owner
            color = [(albedo >> (8*i)) & 255 for i in range(4)]
            assert owner < len(triangles), 'Invalid triangle owner'
            assert enabled[owner], 'Owner belongs to a disabled instance or another class'
            normal_error = max(abs(((normal >> (8*i)) & 255)/255 - (normals[owner][i]*.5+.5))
                               for i in range(3)) if normals is not None else 0
            bad_material = (any(abs(color[i]-v) > 1 for i, v in enumerate((51,153,204,255))) or
                abs(((normal >> 24) & 255)-64)>1 or
                max(abs(a-b) for a,b in zip((er,eg,eb),(4,2,1)))>.004)
            if material_oracle is not None:
                bad_material = material_oracle.attribute_error(owner, cell, [x/255 for x in color],
                    ((normal >> 24) & 255)/255, (er,eg,eb))
            elif generic:
                length_squared = sum((((normal >> (8*i)) & 255)/255*2-1)**2 for i in range(3))
                bad_material = color[3] != 255 or abs(length_squared-1)>.025
            if not all(math.isfinite(v) for v in (er, eg, eb, ea)) or ea != 0 or \
               normal_error > 1.5/255 or bad_material:
                bad_attributes.append(cell)
        elif albedo or normal or any((er, eg, eb, ea)):
            bad_attributes.append(cell)
    actual = set(occupied)
    missing, extra = certain-actual, actual-possible
    ambiguous = possible-certain
    owner_mismatch = [cell for cell in actual & possible
                      if not possible_owner[cell] <= occupied[cell] <= certain_owner.get(cell, EMPTY)
                      or not clip_cell(triangles[occupied[cell]], cell, AMBIGUITY)]
    if material_oracle is not None:
        for cell in actual & possible:
            surface = material_oracle.surface(occupied[cell], [x+.5 for x in cell])
            if not surface['visible'] and not surface['alpha_ambiguous'] and cell not in owner_mismatch:
                owner_mismatch.append(cell)
    result = dict(resolution=n, triangles=len(triangles), occupied=len(actual),
                  attribute_profile='finite HDR, occupancy alpha, unit normal only' if generic else 'analytic material and normal oracle',
                  oracle_occupied=len(expected), ambiguity_grid_units=AMBIGUITY,
                  nominal_missing=len(set(expected)-actual), nominal_extra=len(actual-set(expected)),
                  certain_cells=len(certain), possible_cells=len(possible),
                  ambiguous_cells=len(ambiguous), missing=len(missing), extra=len(extra),
                  alpha_ambiguous_cells=len(alpha_ambiguous),
                  owner_mismatch=len(owner_mismatch), bad_attributes=len(bad_attributes),
                  precision=(len(actual)-len(extra))/max(1,len(actual)),
                  recall=(len(certain)-len(missing))/max(1,len(certain)),
                  missing_cells=sorted(missing), extra_cells=sorted(extra),
                  owner_mismatch_cells=sorted(owner_mismatch),
                  bad_attribute_cells=sorted(bad_attributes))
    reference_path = Path(str(prefix)+'.reference.bin')
    reference = None
    if reference_path.exists():
        reference_data = reference_path.read_bytes()
        assert len(reference_data) == n**3*4
        reference = {(i % n, (i//n) % n, i//(n*n))
                     for i, (owner,) in enumerate(struct.iter_unpack('<I', reference_data))
                     if owner != EMPTY}
        reference_missing, reference_extra = opaque_certain-reference, reference-opaque_possible
        result['reference'] = dict(profile='pinned-source-equivalent occupancy harness; 8x MSAA',
            occupied=len(reference), missing=len(reference_missing), extra=len(reference_extra),
            precision=(len(reference)-len(reference_extra))/max(1, len(reference)),
            recall=(len(opaque_certain)-len(reference_missing))/max(1, len(opaque_certain)),
            contract='opaque geometry; reference has no alpha cutoff',
            missing_cells=sorted(reference_missing), extra_cells=sorted(reference_extra))
    write_slices(prefix, n, set(expected), actual, certain, possible, reference)
    if Path(str(prefix)+'.gbuffer.bin').exists():
        assert material_oracle is not None, 'G-buffer comparisons require the material atlas contract'
        result['gbuffer'] = compare_gbuffer(prefix, metadata, material_oracle, data, alpha_ambiguous)
    if metadata.get('reflectance_policy') == 'averaged':
        result['reflectance'] = compare_reflectance(prefix, metadata, triangles)
    Path(str(prefix)+'.comparison.json').write_text(json.dumps(result, indent=2))
    return result


def write_slices(prefix, dimension, expected, actual, certain, possible, reference):
    """RGB PNG panels: oracle | engine errors | reference errors (when available)."""
    for z in sorted({cell[2] for cell in expected}):
        if z not in (10, 15, 22, 28, 35, 41, 48):
            continue
        sets = [expected, actual] + ([reference] if reference is not None else [])
        missing = [certain-panel for panel in sets]
        extra = [panel-possible for panel in sets]
        ambiguous = possible-certain
        pixels = bytearray()
        for y in range(dimension):
            pixels.append(0)  # PNG filter: none
            for panel_index, panel in enumerate(sets):
                for x in range(dimension):
                    cell = (x, y, z)
                    color = (230, 230, 230) if cell in panel else (16, 16, 16)
                    if panel is not expected:
                        if cell in missing[panel_index]:
                            color = (255, 40, 40)
                        elif cell in extra[panel_index]:
                            color = (255, 210, 0)
                        elif cell in ambiguous:
                            color = (120, 70, 160) if cell in panel else (40, 24, 55)
                    pixels.extend(color)
        def chunk(kind, data):
            return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
        png = b'\x89PNG\r\n\x1a\n'
        png += chunk(b'IHDR', struct.pack('>2I5B', dimension*len(sets), dimension, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
        Path(str(prefix)+f'.z{z}.png').write_bytes(png)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument('--output', type=Path, default=OUT/'current')
    parser.add_argument('--resolution', type=int, choices=(64, 128, 256), default=64)
    parser.add_argument('--fixture', choices=('analytic', 'geometry', 'materials', 'mixtures'), default='analytic')
    parser.add_argument('--reflectance-policy', choices=('owner','averaged'), default='owner')
    parser.add_argument('--reflectance-budget-mb', type=int, default=512)
    parser.add_argument('--mixture-variant', choices=('base','reverse','duplicate','tessellated'), default='base')
    parser.add_argument('--model', type=Path, help='Existing glTF scene for paired or opaque geometry comparison')
    parser.add_argument('--opaque-model', action='store_true', help='Normalize material alpha mode to opaque in a generated copy')
    parser.add_argument('--capture-only', action='store_true', help='Capture and compare backend bytes, without an independent scene oracle')
    parser.add_argument('--reverse-order', action='store_true', help='Reverse material-atlas draw/record order')
    parser.add_argument('--matrix', action='store_true')
    parser.add_argument('--reference', action='store_true', help='Run the 8x reference occupancy harness')
    parser.add_argument('--gbuffer', action='store_true', help='Render GBufferSP at matching atlas surface points')
    parser.add_argument('--lifecycle', action='store_true', help='Capture rebuild, mode switch, move, removal, restore')
    parser.add_argument('--grid-percent', type=int, default=0, help='Fixed centered calibration cube as percent of normalized unit extent')
    parser.add_argument('--compare', type=Path, help='Compare an existing analytic capture prefix')
    args = parser.parse_args()
    if args.compare:
        print(json.dumps(compare(args.compare), indent=2))
        return
    folder = args.output.resolve()
    folder.mkdir(parents=True, exist_ok=True)
    if args.model:
        fixture=args.model.resolve()
        assert args.opaque_model or args.capture_only, 'General textured models require --opaque-model or --capture-only'
        if args.opaque_model:
            document=json.loads(fixture.read_text(encoding='utf-8'))
            for collection in ('buffers','images'):
                for item in document.get(collection,[]):
                    if 'uri' in item and not item['uri'].startswith('data:'):
                        item['uri']=os.path.relpath(fixture.parent/item['uri'],folder).replace('\\','/')
            for material in document.get('materials',[]):
                material['alphaMode']='OPAQUE'
            fixture=folder/'opaque-model.gltf'
            fixture.write_text(json.dumps(document),encoding='utf-8')
        profile='paired_model' if args.capture_only else 'opaque_model'
    elif args.fixture == 'mixtures':
        fixture = mixture_fixture(folder, args.resolution, args.mixture_variant)
        profile='materials'
    elif args.fixture == 'materials':
        fixture = material_fixture(folder, args.resolution, args.reverse_order)
        profile='materials'
    else:
        assert not args.reverse_order, '--reverse-order requires the material fixture'
        fixture = {'analytic': write_fixture, 'geometry': geometry_fixture}[args.fixture](folder, args.resolution)
        profile=args.fixture
    (folder/'calibration-profile.json').write_text(json.dumps(dict(profile=profile,fixture=str(fixture))))
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    last = original
    env = os.environ.copy()
    env.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}
    try:
        modes = itertools.product(('geom', 'comp'), (0, 1), (0, 1)) if args.matrix else \
            (('geom', 0, 0), ('comp', 0, 0))
        for backend, thread, queue in modes:
            tag = f'{backend}-{args.resolution}-thread{thread}-async{queue}'
            prefix = folder/tag
            settings = dict(default_model_path=fixture.as_posix(), voxelizer=backend,
                            voxel_resolution=args.resolution, light_count=0,
                            environment_lighting='false', skybox_visible='false')
            settings.update({'dynamic_light.enabled': 'false', 'light_markers.enabled': 'false'})
            settings.update(voxel_reflectance_policy=args.reflectance_policy,
                            voxel_reflectance_budget_mb=args.reflectance_budget_mb)
            assert config.read_bytes() == last, 'Configuration changed externally'
            last = original + b'\n' + ''.join(f'{k}={v}\n' for k,v in settings.items()).encode()
            config.write_bytes(last)
            with Path(str(prefix)+'.log').open('w') as log:
                command = [str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'), '--frames=2',
                    '--mode=1', f'--rhi-thread={thread}', f'--async-compute={queue}',
                    f'--capture-voxels={prefix}']
                if args.reference:
                    command.append('--voxel-reference')
                if args.gbuffer:
                    command.append('--voxel-gbuffer')
                if args.lifecycle:
                    command.append('--voxel-lifecycle')
                if args.grid_percent:
                    command.append(f'--voxel-grid-percent={args.grid_percent}')
                completed = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                    stderr=subprocess.STDOUT, timeout=180)
            text = Path(str(prefix)+'.log').read_text(errors='replace')
            errors = [line for line in text.splitlines() if '[error]' in line or
                      'VUID-' in line or 'SYNC-HAZARD' in line]
            assert completed.returncode == 0 and not errors, f'{tag}: {errors[:3]}; see log'
            assert json.loads(Path(str(prefix)+'.json').read_text())['reflectance_policy']==args.reflectance_policy
            results[tag] = dict(captured=True) if args.capture_only else compare(prefix)
            print(tag, {k: results[tag][k] for k in ('captured','occupied', 'missing', 'extra',
                   'owner_mismatch', 'bad_attributes') if k in results[tag]}, flush=True)
            if args.lifecycle:
                original_volume = Path(str(prefix)+'.voxels.bin').read_bytes()
                for step, revision in (('rebuild', 2), ('modes', 2), ('moved', 3), ('empty', 4), ('restored', 5)):
                    step_prefix = Path(str(prefix)+'.'+step)
                    result = compare(step_prefix)
                    results[tag+'.'+step] = result
                    metadata = json.loads(Path(str(step_prefix)+'.json').read_text())
                    assert metadata['geometry_revision'] == revision, (step, metadata)
                    volume = Path(str(step_prefix)+'.voxels.bin').read_bytes()
                    if step in ('rebuild', 'modes', 'restored'):
                        assert volume == original_volume, f'{tag} {step}: volume not restored exactly'
                        if args.reflectance_policy == 'averaged':
                            assert Path(str(step_prefix)+'.reflectance.bin').read_bytes() == Path(str(prefix)+'.reflectance.bin').read_bytes(), f'{tag} {step}: reflectance not restored exactly'
                    elif step == 'moved':
                        assert volume != original_volume, f'{tag}: movement did not change volume'
                    elif step == 'empty':
                        assert result['occupied'] == 0, f'{tag}: removal left occupied cells'
                    print(tag+'.'+step, {k: result[k] for k in ('occupied', 'missing', 'extra',
                          'owner_mismatch', 'bad_attributes')}, flush=True)
    finally:
        assert config.read_bytes() == last, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (folder/'results.json').write_text(json.dumps(results, indent=2))
        (folder/'config.sha256').write_text(hashlib.sha256(original).hexdigest()+'\n')
        paths = [path for path in folder.iterdir() if path.suffix in ('.bin', '.gltf')]
        (folder/'data-hashes.json').write_text(json.dumps(
            {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}, indent=2))
    verify_volume_hashes(folder, results)
    assert all(not any(result.get(k,0) for k in ('missing','extra','owner_mismatch','bad_attributes')) and
               not result.get('gbuffer',{}).get('failures',0) and
               not result.get('reflectance',{}).get('failures',0)
               for result in results.values()), 'Calibration failed; see comparison JSON files'


def verify_volume_hashes(folder, results):
    hashes=json.loads((folder/'data-hashes.json').read_text())
    digests={tag+'.voxels.bin':hashes[tag+'.voxels.bin'] for tag in results}
    (folder/'volume-hashes.json').write_text(json.dumps(digests,indent=2))
    # Lifecycle states intentionally differ; all backends/scheduling modes must
    # agree within each state, including owners, attributes, and ambiguous cells.
    for step in {tag.partition('.')[2] for tag in results}:
        for suffix in ('.voxels.bin','.reflectance.bin'):
            values={hashes[tag+suffix] for tag in results if tag.partition('.')[2]==step and tag+suffix in hashes}
            assert len(values)<=1, f'Paired {suffix} volumes differ at {step or "initial"}; investigate before accepting'


if __name__ == '__main__':
    main()
