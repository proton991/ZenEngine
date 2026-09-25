"""M4 dynamic scene lifecycle verification; serial configuration ownership."""
import argparse
import array
import hashlib
import itertools
import json
import os
from pathlib import Path
import struct
import subprocess

import validate_static_voxel_gi as static
from voxelization_fixtures import Fixture

ROOT = Path(__file__).resolve().parents[1]
STAGES = ('initial', 'moved', 'deformed', 'teleported', 'removed', 'restored', 'camera', 'resized', 'returned')


def fixture(folder):
    asset = Fixture()
    asset.document['materials'] = [dict(pbrMetallicRoughness=dict(baseColorFactor=color,
        metallicFactor=0, roughnessFactor=.8)) for color in ([.7,.7,.7,1], [.8,.15,.1,1], [.1,.3,.8,1])]
    asset.mesh([[(-.5,)*3]*3, [(.5,)*3]*3])
    def plane(x0, x1, y0, y1, z, material=0, reverse=False):
        corners = [(x0,y0,z),(x1,y0,z),(x1,y1,z),(x0,y1,z)]
        order = ((0,2,1),(0,3,2)) if reverse else ((0,1,2),(0,2,3))
        asset.mesh([[corners[i] for i in t] for t in order], material=material)
        # The hidden sender uses a +Z representative normal to reflect this light.
        if reverse:
            asset.document['meshes'][-1]['primitives'][0]['attributes']['NORMAL'] = asset.attribute([(0,0,1)]*6,'VEC3')
    plane(-.38, 0, -.3, .3, -.2)
    plane(-.2, .2, -.2, .2, .08, material=1, reverse=True)
    plane(0, .38, -.3, .3, -.2, material=2)
    return asset.save(folder, 'm4-lifecycle')


def analyze(prefix):
    stats, _ = static.analyze(prefix, allow_dynamic=True)
    metadata = json.loads(Path(str(prefix)+'.static.json').read_text())
    lighting_meta = json.loads(Path(str(prefix)+'.lighting.json').read_text())
    data = Path(str(prefix)+'.static.bin').read_bytes()
    n = metadata['resolution']; cells = n**3
    static_ids = struct.unpack_from('<'+str(metadata['selected_static'])+'I', data, metadata['offsets']['static_list'])
    dynamic_ids = struct.unpack_from('<'+str(metadata['selected_dynamic'])+'I', data, metadata['offsets']['dynamic_list'])
    assert len(set(static_ids)) == len(static_ids) and len(set(dynamic_ids)) == len(dynamic_ids)
    assert all(i < cells for i in static_ids+dynamic_ids)
    raw = array.array('f'); raw.frombytes(data)
    for ids, offset in ((set(static_ids), 0), (set(dynamic_ids), metadata['offsets']['dynamic_faces']//4)):
        for face in (0, 4):
            for cell in range(cells):
                assert (raw[offset+8*(face*cells+cell)+3] == 1) == (cell in ids), 'Stale or missing receiver'
    surface = Path(str(prefix)+'.surface.bin').read_bytes()
    classes = {1:0, 2:0}
    for pixel in range(len(surface)//96):
        identity, kind = struct.unpack_from('<2I', surface, pixel*96+48)
        if identity:
            assert kind == (2 if identity == 4 else 1), (identity, kind)
            classes[kind] += 1
    assert classes[1] > 100
    assert classes[2] > 100 if prefix.name.endswith('.removed') is False else classes[2] == 0
    stats.update({k:metadata[k] for k in ('selected_static','selected_dynamic','dynamic_occupied',
        'cache_updated','cache_batches','static_generation','dynamic_generation')})
    stats.update(static_pixels=classes[1], dynamic_pixels=classes[2], width=lighting_meta['width'], height=lighting_meta['height'])
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/dynamic-voxel-m4/scenes')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--case', choices=[f'{v}-t{t}-q{q}' for v,t,q in itertools.product(('comp','geom'),(0,1),(0,1))])
    args = parser.parse_args(); out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    model = fixture(out)
    config = ROOT/'Data/engine.cfg'; original = config.read_bytes(); last = original
    environment = os.environ.copy(); environment.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}; hashes = {}
    cases = [('comp',0,0)] if args.quick else itertools.product(('comp','geom'),(0,1),(0,1))
    try:
        for voxelizer, thread, queue in cases:
            name = f'{voxelizer}-t{thread}-q{queue}'; prefix = out/name
            if args.case and name != args.case: continue
            settings = dict(default_model_path=model.as_posix(), camera_position='0,0,2', voxelizer=voxelizer,
                voxel_resolution=64, voxel_reflectance_policy='averaged', voxel_reflectance_budget_mb=512,
                voxel_gi_method='dynamic_voxel', dynamic_voxel_gi_query_backend='voxel_dda',
                dynamic_voxel_gi_temporal_filter='off', dynamic_voxel_gi_spatial_filter='false',
                dynamic_voxel_gi_memory_budget_mb=3072, environment_lighting='false', skybox_visible='false',
                light_count=1, voxel_gi_indirect_intensity=1)
            settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false','light.0.type':'point',
                'light.0.position':'0,.2,.35','light.0.color':'1,1,1','light.0.intensity':2,'light.0.range':4,
                'light.0.enabled':'true','light.0.casts_shadows':'true'})
            if not args.compare_only:
                assert config.read_bytes() == last, 'Configuration changed externally'
                last = original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode(); config.write_bytes(last)
                Path(str(prefix)+'.cfg').write_bytes(last)
                command = [str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'),'--frames=12','--mode=3',
                    f'--rhi-thread={thread}',f'--async-compute={queue}','--gbuffer-size=256','--width=320','--height=180',
                    '--dynamic-gi-lifecycle',f'--capture-lighting={prefix}']
                with Path(str(prefix)+'.log').open('w') as log:
                    run = subprocess.run(command,cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=300)
                text = Path(str(prefix)+'.log').read_text(errors='replace')
                errors = [line for line in text.splitlines() if any(t in line for t in ('[error]','VUID-','SYNC-HAZARD'))]
                assert run.returncode == 0 and not errors, (name,run.returncode,errors[:3])
                assert '[OK] No memory leaks detected' in text
            for stage in STAGES:
                tag = name+'.'+stage; capture = out/tag
                result = analyze(capture); results[tag] = result
                initial = results[name+'.initial']
                assert result['cache_batches'] == initial['cache_batches'] and result['cache_updated'] == 0
                assert result['static_generation'] == initial['static_generation'], 'Dynamic/camera change rebuilt static cache'
                if stage == 'removed':
                    assert result['dynamic_occupied'] == result['selected_dynamic'] == 0
                else:
                    assert result['dynamic_occupied'] > 0 and result['selected_dynamic'] > result['dynamic_occupied']
                if stage == 'resized': assert (result['width'], result['height']) == (515,321)
                digest = hashlib.sha256(Path(str(capture)+'.lighting.bin').read_bytes()).hexdigest()
                if stage in hashes: assert digest == hashes[stage], 'Voxelizer/submission output mismatch'
                else: hashes[stage] = digest
                if stage in ('restored','returned'): assert digest == hashes['initial'], 'Restored scene retained stale lighting'
                print('PASS', tag, 'receivers', result['selected_static'], result['selected_dynamic'], flush=True)
            assert len({hashes[s] for s in ('initial','moved','deformed','teleported','removed')}) == 5
    finally:
        assert config.read_bytes() == last, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (out/'results.json').write_text(json.dumps(results,indent=2))
        (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__ == '__main__': main()
