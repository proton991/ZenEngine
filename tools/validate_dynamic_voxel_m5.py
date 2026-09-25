"""M5 raw/temporal/full HDR lifecycle captures, with serial config ownership."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import validate_dynamic_voxel_m4 as lifecycle
import validate_dynamic_voxel_gi as hdr

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT/'build/dynamic-voxel-m5/scenes')
    parser.add_argument('--compare-only', action='store_true')
    parser.add_argument('--quick', action='store_true')
    parser.add_argument('--extended-lighting', action='store_true',
                        help='Exercise the M6 five-light and environment profile during geometry changes')
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    model = lifecycle.fixture(out)
    cfg = ROOT/'Data/engine.cfg'; original = cfg.read_bytes(); last = original
    environment = os.environ.copy()
    environment.update(VK_LAYER_VALIDATE_SYNC='1', VK_LOADER_LAYERS_DISABLE='~implicit~', DISABLE_RTSS_LAYER='1')
    results = {}
    try:
        for voxelizer, thread, queue in ([('comp', 0, 0)] if args.quick else [('comp', 0, 0), ('geom', 1, 1)]):
            for name, temporal, spatial in [('raw', 'off', 'false'), ('temporal', 'elapsed', 'false'), ('full', 'elapsed', 'true')]:
                prefix = out/f'{voxelizer}-{name}'
                settings = dict(default_model_path=model.as_posix(), camera_position='0,0,2', voxelizer=voxelizer,
                    voxel_resolution=64, voxel_reflectance_policy='averaged', voxel_reflectance_budget_mb=512,
                    voxel_gi_method='dynamic_voxel', dynamic_voxel_gi_query_backend='voxel_dda',
                    dynamic_voxel_gi_temporal_filter=temporal, dynamic_voxel_gi_spatial_filter=spatial,
                    dynamic_voxel_gi_memory_budget_mb=3072, environment_lighting='false', skybox_visible='false',
                    light_count=1, voxel_gi_indirect_intensity=1)
                settings.update({'dynamic_light.enabled':'false','light_markers.enabled':'false','light.0.type':'point',
                    'light.0.position':'0,.2,.35','light.0.color':'1,1,1','light.0.intensity':2,'light.0.range':4,
                    'light.0.enabled':'true','light.0.casts_shadows':'true'})
                if args.extended_lighting:
                    settings.update(environment_lighting='true', environment_intensity=1,
                                    environment_rotation_degrees=0, light_count=5)
                    for index, position in enumerate(('-.25,.25,-.1','.25,.25,-.1',
                                                       '-.25,.25,.1','.25,.25,.1','0,.15,.25')):
                        settings.update({f'light.{index}.type':'point', f'light.{index}.position':position,
                            f'light.{index}.color':'1,1,1', f'light.{index}.intensity':1,
                            f'light.{index}.range':4, f'light.{index}.enabled':'true',
                            f'light.{index}.casts_shadows':'true'})
                if not args.compare_only:
                    assert cfg.read_bytes() == last, 'Configuration changed externally'
                    last = original+b'\n'+''.join(f'{k}={v}\n' for k,v in settings.items()).encode()
                    cfg.write_bytes(last); Path(str(prefix)+'.cfg').write_bytes(last)
                    command = [str(ROOT/'build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe'),'--frames=12','--mode=3',
                        f'--rhi-thread={thread}',f'--async-compute={queue}','--gbuffer-size=256','--width=320','--height=180',
                        '--dynamic-gi-lifecycle',f'--capture-lighting={prefix}']
                    with Path(str(prefix)+'.log').open('w') as log:
                        run = subprocess.run(command,cwd=ROOT,env=environment,stdout=log,stderr=subprocess.STDOUT,timeout=300)
                    text = Path(str(prefix)+'.log').read_text(errors='replace')
                    errors = [line for line in text.splitlines() if any(t in line for t in ('[error]','VUID-','SYNC-HAZARD'))]
                    assert run.returncode == 0 and not errors, (prefix.name,run.returncode,errors[:3])
                    assert '[OK] No memory leaks detected' in text
                for stage in lifecycle.STAGES:
                    capture = out/(prefix.name+'.'+stage)
                    stats = lifecycle.analyze(capture)
                    meta = json.loads(Path(str(capture)+'.static.json').read_text())
                    assert (meta['temporal_mode'],meta['spatial_filter']) == (0 if temporal == 'off' else 2, int(spatial == 'true'))
                    _, pixels = hdr.load_lighting(capture)
                    sums = [sum(pixels[i+c] for i in range(8,len(pixels),28)) for c in range(3)]
                    stats.update(diffuse_sum=sums,history_time=meta['history_time'],history_reset=meta['history_reset'])
                    if stage == 'removed': assert stats['dynamic_occupied'] == stats['selected_dynamic'] == 0
                    results[capture.name] = stats
                    print('PASS',capture.name,'diffuse_sum',sums,'history_reset',meta['history_reset'],flush=True)
                baseline = results[prefix.name+'.initial']
                for stage in lifecycle.STAGES:
                    current = results[prefix.name+'.'+stage]
                    assert current['static_generation'] == baseline['static_generation']
                    assert current['cache_batches'] == baseline['cache_batches']
    finally:
        assert cfg.read_bytes() == last, 'Configuration changed externally; preserving it'
        cfg.write_bytes(original)
        (out/'results.json').write_text(json.dumps(results,indent=2))
        (out/'config.sha256').write_text(hashlib.sha256(original).hexdigest())


if __name__ == '__main__': main()
