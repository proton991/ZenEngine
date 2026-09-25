"""M2 real GPU lifecycle: V0/V1 oracles, compaction, union, and DDA class queries."""
import argparse
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import struct
import subprocess

from validate_voxelization import compare, EMPTY
from voxelization_fixtures import geometry_fixture, material_fixture
from voxelization_reflectance import mixture_fixture

ROOT = Path(__file__).resolve().parents[1]
STAGES = ('initial', 'moved', 'deformed', 'removed', 'promoted', 'empty', 'restored', 'inflight')


def read(prefix, suffix):
    return Path(str(prefix)+suffix).read_bytes()


def volume(prefix):
    metadata = json.loads(read(prefix, '.json'))
    n = metadata['resolution']
    cells = {}
    for i, record in enumerate(struct.iter_unpack('<4I4f', read(prefix, '.voxels.bin'))):
        if record[0] != EMPTY:
            x, y, z = i % n, (i//n) % n, i//(n*n)
            cells[z+n*(y+n*x)] = (i, record)
    return metadata, cells


def compaction(prefix, cells, n):
    count, = struct.unpack('<I', read(prefix, '.occupied-count.bin'))
    listed = [v[0] for v in struct.iter_unpack('<I', read(prefix, '.occupied-list.bin'))]
    indices = [v[0] for v in struct.iter_unpack('<I', read(prefix, '.grid-to-list.bin'))]
    assert len(listed) == len(indices) == n**3
    assert count == len(cells) and set(listed[:count]) == set(cells)
    assert all(v == EMPTY for v in listed[count:])
    assert all(indices[cell] == index for index, cell in enumerate(listed[:count]))
    assert all(v == EMPTY for cell, v in enumerate(indices) if cell not in cells)


def check_queries(prefix, grids, metadata):
    n, size, minimum = metadata['resolution'], metadata['voxel_size'], metadata['grid_min']
    requests = list(struct.iter_unpack('<8f4I', read(prefix, '.queries.bin')))
    results = list(struct.iter_unpack('<20f8I', read(prefix, '.query-results.bin')))
    assert len(requests) == len(results) == 576
    hits = 0
    reflectance = [read(Path(str(prefix)+f'.class{m}'), '.reflectance.bin') for m in (1,2)]
    for ray, result in zip(requests, results):
        nearest = ray[7]
        expected = None
        for object_class, cells in enumerate(grids[:2], 1):
            if not ray[8] & object_class:
                continue
            for cell, (_, record) in cells.items():
                xyz = (cell//(n*n), (cell//n) % n, cell % n)
                if any(ray[axis+4] == 0 and not minimum[axis]+xyz[axis]*size <= ray[axis] < minimum[axis]+(xyz[axis]+1)*size for axis in range(3)):
                    continue
                lo, hi = ray[3], ray[7]
                intersects = True
                for axis in range(3):
                    a = minimum[axis] + xyz[axis]*size
                    b = a+size
                    if ray[axis+4] == 0:
                        intersects &= a <= ray[axis] < b
                    else:
                        t = sorted(((a-ray[axis])/ray[axis+4], (b-ray[axis])/ray[axis+4]))
                        lo, hi = max(lo, t[0]), min(hi, t[1])
                if intersects and lo < hi and lo < nearest:
                    expected = (object_class, cell, record)
                    nearest = lo
        status = 2 if expected else 1
        assert result[20] == result[24] == status, (ray, result, expected)
        if expected:
            hits += 1
            object_class, cell, record = expected
            assert result[21:24] == (object_class, cell, 3)
            assert abs(result[3]-nearest) < 2e-5
            for axis in range(3):
                assert abs(result[axis]-(ray[axis]+ray[axis+4]*nearest)) < 2e-5
            color = [((record[1] >> (i*8)) & 255)/255 for i in range(3)]
            metal = (record[2] >> 24)/255
            normal = [((record[2] >> (i*8)) & 255)/255*2-1 for i in range(3)]
            length = math.sqrt(sum(v*v for v in normal))
            assert max(abs(a-b) for a,b in zip(result[4:7], (v/length for v in normal))) < 2e-6
            assert max(abs(a-b) for a,b in zip(result[8:12], color+[metal])) < 2e-6
            assert result[12:16] == record[4:8]
            if metadata['reflectance_policy'] == 'averaged':
                index = grids[object_class-1][cell][0]
                packed, = struct.unpack_from('<I', reflectance[object_class-1], index*32+16)
                rho = [((packed >> (i*8)) & 255)/255 for i in range(3)]
            else:
                rho = [v*(1-metal)*.96 for v in color]
            assert max(abs(a-b) for a,b in zip(result[16:19], rho)) < 2e-6
    return hits


def check_state(prefix):
    summaries, metadata, grids = [], [], []
    for mask in (1, 2, 3):
        part = Path(str(prefix)+f'.class{mask}')
        result = compare(part)
        assert not any(result[k] for k in ('missing', 'extra', 'owner_mismatch', 'bad_attributes')), (part, result)
        assert not result.get('reflectance', {}).get('failures'), part
        meta, cells = volume(part)
        if mask != 3:
            compaction(part, cells, meta['resolution'])
        summaries.append(result)
        metadata.append(meta)
        grids.append(cells)
    assert all(m['grid_min'] == metadata[0]['grid_min'] and m['voxel_size'] == metadata[0]['voxel_size'] and
               m['scene_revision'] == metadata[0]['scene_revision'] for m in metadata)
    assert set(grids[2]) == set(grids[0]) | set(grids[1])
    for cell, (_, record) in grids[2].items():
        owners = [g[cell][1] for g in grids[:2] if cell in g]
        assert record == min(owners, key=lambda v: v[0]), ('combined owner', cell)
    if metadata[0]['reflectance_policy'] == 'averaged':
        values = [struct.iter_unpack('<5I3f', read(Path(str(prefix)+f'.class{m}'), '.reflectance.bin')) for m in (1,2,3)]
        for static, dynamic, combined in zip(*values):
            assert all(static[i]+dynamic[i] == combined[i] for i in range(4)), 'Combined mean lost sample weighting'
    hits = check_queries(prefix, grids, metadata[0])
    return dict(classes=summaries, query_hits=hits, revisions=[m['geometry_revision'] for m in metadata],
                scene_revision=metadata[0]['scene_revision'], grid_min=metadata[0]['grid_min'], voxel_size=metadata[0]['voxel_size'])


def check_lifecycle(prefix):
    """Check intended edits against GPU scene inputs, independently of voxelization."""
    from validate_voxel_gi import load_capture
    initial = Path(str(prefix)+'.initial.class3')
    original_nodes = read(initial, '.nodes.bin')
    original_vertices = read(initial, '.vertices.bin')
    original_records = list(struct.iter_unpack('<4I', read(initial, '.triangles.bin')))
    original_metadata = json.loads(read(initial, '.json'))
    node_stride = original_metadata['node_stride']
    node_count = len(original_nodes)//node_stride
    original_image = load_capture(Path(str(prefix)+'.initial.ppm'))[2]
    for stage in STAGES:
        base = Path(str(prefix)+'.'+stage)
        full = Path(str(base)+'.class3')
        records = list(struct.iter_unpack('<4I', read(full, '.triangles.bin')))
        assert len(records) == len(original_records)
        for original, record in zip(original_records, records):
            assert original[:3] == record[:3], 'Triangle/instance/material ID changed'
            mask = 2 if record[1] % 2 else 1
            if stage in ('removed', 'promoted') and mask == 2: mask = 0
            if stage == 'promoted' and record[1] == (2 if node_count > 2 else 0): mask = 2
            if stage == 'empty': mask = 0
            assert record[3] == mask
        nodes = read(full, '.nodes.bin')
        vertices = read(full, '.vertices.bin')
        for node in range(node_count):
            old = struct.unpack_from('<16f', original_nodes, node*node_stride)
            current = struct.unpack_from('<16f', nodes, node*node_stride)
            expected = list(old)
            if stage not in ('initial','restored','inflight') and node % 2:
                for axis, delta in enumerate((.023,-.011,.017)): expected[12+axis] += delta
            assert max(abs(a-b) for a,b in zip(current,expected)) < 2e-6, (stage,node)
        for offset in range(0,len(vertices),original_metadata['vertex_stride']):
            x,y,z = struct.unpack_from('<3f',original_vertices,offset)
            current = struct.unpack_from('<3f',vertices,offset)
            if stage in ('deformed','removed','promoted','empty'):
                z += 7*x*y*(.25-x*x)*(.25-y*y)
            assert max(abs(a-b) for a,b in zip(current,(x,y,z))) < 2e-6, (stage,offset)
        for mask in (1,2):
            part = Path(str(base)+f'.class{mask}')
            for suffix in ('.nodes.bin','.vertices.bin','.triangles.bin'):
                assert read(part,suffix) == read(full,suffix), 'Class input snapshots disagree'
        image = load_capture(Path(str(base)+'.ppm'))[2]
        if stage == 'empty': assert not any(image)
        meta = json.loads(read(full, '.json'))
        same_grid = all(meta[k] == original_metadata[k] for k in ('grid_min', 'voxel_size'))
        if stage == 'restored' and same_grid: assert image == original_image, 'Restored composition differs'
        if stage in ('initial','moved'):
            meta = json.loads(read(Path(str(base)+'.class1'), '.json'))
            if stage == 'initial': before = meta
            elif (meta['grid_min'],meta['voxel_size']) == (before['grid_min'],before['voxel_size']):
                assert meta['geometry_revision'] == before['geometry_revision'], 'Static class rebuilt during bounded dynamic motion'
                assert read(Path(str(base)+'.class1'),'.voxels.bin') == read(Path(str(prefix)+'.initial.class1'),'.voxels.bin')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', choices=('geometry', 'materials', 'mixtures'), default='materials')
    parser.add_argument('--policy', choices=('owner', 'averaged'), default='averaged')
    parser.add_argument('--variant', choices=('base', 'reverse', 'duplicate', 'tessellated'), default='base')
    parser.add_argument('--matrix', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compare-only', action='store_true')
    args = parser.parse_args()
    folder = args.output.resolve(); folder.mkdir(parents=True, exist_ok=True)
    model = mixture_fixture(folder, 64, args.variant) if args.fixture == 'mixtures' else \
        {'geometry': geometry_fixture, 'materials': material_fixture}[args.fixture](folder, 64)
    (folder/'calibration-profile.json').write_text(json.dumps(dict(profile='geometry' if args.fixture == 'geometry' else 'materials')))
    config = ROOT/'Data/engine.cfg'; original = config.read_bytes(); last = original
    env = os.environ.copy()
    env.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}
    modes = itertools.product(('geom', 'comp'), (0, 1), (0, 1)) if args.matrix else (('geom', 0, 0), ('comp', 0, 0))
    try:
        for backend, thread, queue in modes:
            tag = f'{backend}-thread{thread}-async{queue}'
            prefix = folder/tag
            settings = dict(default_model_path=model.as_posix(), voxelizer=backend, voxel_resolution=64,
                voxel_reflectance_policy=args.policy, voxel_reflectance_budget_mb=128,
                dynamic_voxel_gi_memory_budget_mb=512, voxel_gi_method='cone' if thread == 0 else 'dynamic_voxel',
                camera_position='0,0,2', environment_lighting='false', skybox_visible='false', light_count=1)
            settings.update({'dynamic_light.enabled':'false', 'light_markers.enabled':'false',
                'light.0.enabled':'true', 'light.0.type':'directional', 'light.0.direction':'0,0,-1',
                'light.0.color':'1,1,1', 'light.0.intensity':1, 'light.0.casts_shadows':'true'})
            if not args.compare_only:
                assert config.read_bytes() == last, 'Configuration changed externally'
                last = original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode()
                config.write_bytes(last); Path(str(prefix)+'.cfg').write_bytes(last)
                with Path(str(prefix)+'.log').open('w') as log:
                    run = subprocess.run([str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'), '--frames=2', '--mode=1',
                        f'--rhi-thread={thread}', f'--async-compute={queue}', f'--capture-voxels={prefix}',
                        '--voxel-classes'], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240)
                log = Path(str(prefix)+'.log').read_text(errors='replace')
                errors = [line for line in log.splitlines() if '[error]' in line or 'VUID-' in line or 'SYNC-HAZARD' in line]
                assert run.returncode == 0 and not errors, (tag, errors[:5])
            for stage in STAGES:
                name = tag+'.'+stage
                results[name] = check_state(folder/name)
                if stage == 'empty': assert all(c['occupied'] == 0 for c in results[name]['classes'])
                if stage == 'removed': assert results[name]['classes'][1]['occupied'] == 0
                print('PASS', name, [c['occupied'] for c in results[name]['classes']], 'query hits', results[name]['query_hits'], flush=True)
            assert sum(results[tag+'.'+s]['query_hits'] for s in STAGES) > 0
            check_lifecycle(prefix)
            for stage in ('restored', 'inflight'):
                assert read(folder/(tag+'.'+stage), '.class3.vertices.bin') == read(folder/(tag+'.initial'), '.class3.vertices.bin')
                assert read(folder/(tag+'.'+stage), '.class3.nodes.bin') == read(folder/(tag+'.initial'), '.class3.nodes.bin')
                assert read(folder/(tag+'.'+stage), '.class3.triangles.bin') == read(folder/(tag+'.initial'), '.class3.triangles.bin')
    finally:
        assert config.read_bytes() == last, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (folder/'results.json').write_text(json.dumps(results, indent=2))
        (folder/'config.sha256').write_text(hashlib.sha256(original).hexdigest())
    for stage in STAGES:
        for mask in (1,2,3):
            for suffix in ('voxels', 'reflectance'):
                hashes = {hashlib.sha256(read(folder/(tag+f'.class{mask}'), f'.{suffix}.bin')).hexdigest()
                          for tag in results if tag.endswith('.'+stage)}
                assert len(hashes) == 1, ('backend/scheduling mismatch', stage, mask, suffix)


if __name__ == '__main__':
    main()
