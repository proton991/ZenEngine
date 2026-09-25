"""Runtime material, fixed-grid and budget contracts; serial config ownership."""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess

import validate_dynamic_voxel_gi as hdr
from validate_dynamic_voxel_m4 import fixture

ROOT = Path(__file__).resolve().parents[1]
STAGES = ('initial', 'tint', 'metal', 'emission', 'alpha', 'restored',
          'dynamic-emission', 'dynamic-alpha', 'materials-restored',
          'outside', 'returned', 'outside-again', 'expanded', 'grid-restored')


def capture_stats(prefix):
    stats = hdr.analyze(prefix, 'fixture')
    metadata, data = hdr.load_lighting(prefix)
    stats.update({key: metadata[key] for key in ('method', 'voxel_coverage_mask',
        'scene_geometry_generation', 'scene_surface_generation', 'voxel_geometry_generation')})
    stats['sha256'] = hashlib.sha256(Path(str(prefix)+'.lighting.bin').read_bytes()).hexdigest()
    stats['diffuse_sum'] = [sum(data[i+c] for i in range(8, len(data), 28)) for c in range(3)]
    stats['emission_sum'] = [sum(data[i+c] for i in range(16, len(data), 28)) for c in range(3)]
    if metadata['method'] == 'dynamic_voxel':
        static = json.loads(Path(str(prefix)+'.static.json').read_text())
        stats.update({key: static[key] for key in ('cache_batches', 'static_visibility_generation',
            'dynamic_visibility_generation', 'occupied', 'dynamic_occupied', 'minimum_cell_size')})
    return stats


def check_lifecycle(results, name):
    stages = {stage: results[name+'.'+stage] for stage in STAGES}
    initial = stages['initial']
    for stage, result in stages.items():
        outside = stage in ('outside', 'outside-again')
        assert result['method'] == ('pbr' if outside else 'dynamic_voxel'), (stage, result)
        assert result['voxel_coverage_mask'] == (1 if outside else 3), (stage, result)
        if not outside and stage != 'expanded':
            assert result['minimum_cell_size'] == initial['minimum_cell_size'], stage
        if stage in ('tint', 'metal', 'emission'):
            for key in ('cache_batches', 'static_visibility_generation', 'scene_geometry_generation'):
                assert result[key] == initial[key], (stage, key)
            assert result['scene_surface_generation'] > initial['scene_surface_generation']
        if stage in ('restored', 'materials-restored', 'returned', 'grid-restored'):
            assert result['sha256'] == initial['sha256'], ('Stale restored lighting', stage)
    for stage in ('tint', 'metal', 'emission', 'alpha', 'dynamic-emission', 'dynamic-alpha'):
        assert stages[stage]['sha256'] != initial['sha256'], ('Ineffective edit', stage)
    assert stages['tint']['diffuse_sum'][1] > initial['diffuse_sum'][1]
    assert sum(stages['metal']['diffuse_sum']) < sum(initial['diffuse_sum'])
    assert sum(stages['emission']['diffuse_sum']) > sum(initial['diffuse_sum'])
    assert stages['alpha']['occupied'] < initial['occupied']
    assert stages['alpha']['static_visibility_generation'] > initial['static_visibility_generation']
    assert stages['dynamic-emission']['emission_sum'][1] > 0
    assert stages['dynamic-alpha']['dynamic_occupied'] == 0
    assert stages['dynamic-alpha']['occupied'] == initial['occupied']
    assert stages['dynamic-alpha']['cache_batches'] == stages['restored']['cache_batches']
    assert stages['returned']['cache_batches'] == stages['materials-restored']['cache_batches']
    assert stages['expanded']['minimum_cell_size'] != initial['minimum_cell_size']
    assert stages['expanded']['cache_batches'] > stages['returned']['cache_batches']
    for stage in STAGES[:9]:
        cone = results[name+'.'+stage+'.cone']
        assert cone['method'] == 'cone'
        restored = stage in ('initial', 'restored', 'materials-restored')
        assert (cone['sha256'] == results[name+'.initial.cone']['sha256']) == restored, stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/dynamic-voxel-gap-fixes/contracts')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--compare-only', action='store_true')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    model = fixture(out)
    config = ROOT/'Data/engine.cfg'
    original = config.read_bytes()
    last = original
    env = os.environ.copy()
    env.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}
    hashes = {}
    cases = [('comp', 0, 0)] if args.quick else itertools.product(('comp', 'geom'), (0, 1), (0, 1))
    try:
        for voxelizer, thread, queue in cases:
            name = f'{voxelizer}-t{thread}-q{queue}'
            prefix = out/name
            settings = dict(default_model_path=model.as_posix(), camera_position='0,0,2', voxelizer=voxelizer,
                voxel_resolution=64, voxel_reflectance_policy='averaged', voxel_reflectance_budget_mb=512,
                voxel_gi_method='dynamic_voxel', dynamic_voxel_gi_query_backend='voxel_dda',
                dynamic_voxel_gi_temporal_filter='off', dynamic_voxel_gi_spatial_filter='false',
                dynamic_voxel_gi_memory_budget_mb=3072, environment_lighting='false', skybox_visible='false',
                light_count=1, voxel_gi_indirect_intensity=1)
            settings.update({'dynamic_light.enabled':'false', 'light_markers.enabled':'false',
                'light.0.type':'point', 'light.0.position':'0,.2,.35', 'light.0.color':'1,1,1',
                'light.0.intensity':2, 'light.0.range':4, 'light.0.enabled':'true', 'light.0.casts_shadows':'true'})
            if not args.compare_only:
                assert config.read_bytes() == last, 'Configuration changed externally'
                last = original+b'\n'+''.join(f'{k}={v}\n' for k, v in settings.items()).encode()
                config.write_bytes(last)
                Path(str(prefix)+'.cfg').write_bytes(last)
                command = [str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'), '--disable-rt', '--frames=12', '--mode=3',
                    f'--rhi-thread={thread}', f'--async-compute={queue}', '--gbuffer-size=256',
                    '--width=320', '--height=180', '--gi-contracts', f'--capture-lighting={prefix}']
                with Path(str(prefix)+'.log').open('w') as log:
                    run = subprocess.run(command, cwd=ROOT, env=env, stdout=log,
                                         stderr=subprocess.STDOUT, timeout=300)
                text = Path(str(prefix)+'.log').read_text(errors='replace')
                errors = [line for line in text.splitlines() if any(term in line for term in
                    ('[error]', 'VUID-', 'SYNC-HAZARD'))]
                assert run.returncode == 0 and not errors, (name, run.returncode, errors[:5])
                assert '[OK] No memory leaks detected' in text
            for stage in (*STAGES, *(stage+'.cone' for stage in STAGES[:9])):
                tag = name+'.'+stage
                result = capture_stats(out/tag)
                results[tag] = result
                if stage in hashes:
                    assert result['sha256'] == hashes[stage], ('Producer/submission mismatch', tag)
                else:
                    hashes[stage] = result['sha256']
                print('PASS', tag, flush=True)
            check_lifecycle(results, name)
    finally:
        assert config.read_bytes() == last, 'Configuration changed externally; preserving it'
        config.write_bytes(original)
        (out/'results.json').write_text(json.dumps(results, indent=2))
        (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__ == '__main__':
    main()
