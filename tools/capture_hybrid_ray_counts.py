"""Capture the rays one frame actually issues on a frozen P8 view; owns engine.cfg serially.

Uses benchmark_hybrid_gi.py's baseline workload with the all-feature Compute or HW RT selector,
optionally with its ring of shadow-casting point lights. The demo's --capture-ray-counts frame uses
diagnostic counting shader variants, so its timing is not a production timing. Sky-cache and
radiance-injection rays are issued only on frames that update them; the report records whether
the counted frame did. Counts are also reported per pixel of the capture extent.
"""
import argparse
import json
from pathlib import Path
import subprocess

from benchmark_hybrid_gi import BASELINE, check_frozen_view, light_settings
from capture_hybrid_gi import CAMERAS, ROOT, capture_configuration, fingerprint


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--scene', type=Path, required=True, help='Frozen view glTF written by capture_hybrid_gi.py')
    parser.add_argument('--camera', choices=CAMERAS, required=True)
    parser.add_argument('--backend', choices=('compute', 'hardware'), required=True)
    parser.add_argument('--output', type=Path, required=True, help='Output JSON path')
    parser.add_argument('--frames', type=int, default=100, help='Frames rendered before the counted frame')
    parser.add_argument('--width', type=int, default=1920)
    parser.add_argument('--height', type=int, default=1080)
    parser.add_argument('--lights', type=int, default=0, help='Shadow-casting point lights (benchmark ring)')
    parser.add_argument('--moving-light', action='store_true', help='One light orbits, so each frame injects')
    parser.add_argument('--update-scene', type=Path,
                        help='Animated scene that benchmark_hybrid_updates.py --prepare-only derived from --scene; '
                             'its geometry changes every frame, so the counted frame updates the sky cache')
    parser.add_argument('--set', action='append', default=[], metavar='KEY=VALUE')
    args = parser.parse_args()
    assert args.frames > 0 and min(args.width, args.height) > 0 and args.lights >= 0
    assert not args.moving_light or args.lights > 0, 'A moving light needs --lights'
    assert all('=' in item and '\n' not in item for item in args.set)
    scene = args.scene.resolve()
    check_frozen_view(scene, args.camera)
    out = args.output.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    assert not out.exists(), f'{out} exists; use a fresh path'
    config = ROOT / 'Data/engine.cfg'
    original = config.read_bytes()
    rendered = args.update_scene.resolve() if args.update_scene else scene
    workload = BASELINE | {'default_model_path': rendered.as_posix()}
    if args.lights:
        workload = light_settings(workload, args.lights, args.moving_light)
    workload.update(item.split('=', 1) for item in args.set)
    workload['voxel_gi_backend'] = args.backend
    settings = capture_configuration(original, workload)
    counts = out.with_suffix('.raw.json')
    command = [str(args.exe.resolve()), '--mode=3', '--fixed-step', '--no-ui', '--vsync=0', '--disable-validation',
               *(['--disable-rt'] if args.backend == 'compute' else []), f'--frames={args.frames}',
               f'--width={args.width}', f'--height={args.height}', f'--capture-ray-counts={counts}']
    try:
        config.write_bytes(settings)
        with out.with_suffix('.log').open('w') as log:
            process = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, timeout=900)
    finally:
        if config.read_bytes() != settings:
            raise RuntimeError('External engine.cfg edit; leaving it untouched')
        config.write_bytes(original)
    assert process.returncode == 0, f'Capture failed ({process.returncode}); see {out.with_suffix(".log")}'
    raw = json.loads(counts.read_text())
    assert raw['version'] == 2, raw.get('version')
    pixels = raw['width'] * raw['height']
    providers = ('compute', 'hardware')
    trace = {p: raw[p] for p in providers}
    outside = {p: raw['outside_trace'][p] for p in providers}
    report = dict(executable=fingerprint(args.exe.resolve()), scene=fingerprint(scene),
                  update_scene=fingerprint(rendered) if args.update_scene else None, camera=args.camera,
                  backend=args.backend, width=raw['width'], height=raw['height'], frame=raw['frame'],
                  lights=args.lights, moving_light=args.moving_light, settings=workload,
                  sky_cache_updated=raw['outside_trace']['sky_cache_updated'],
                  radiance_injected=raw['outside_trace']['radiance_injected'],
                  hybrid_trace=trace, outside_trace=outside,
                  per_pixel={p: {k: v / pixels for k, v in (trace[p] | outside[p]).items()} for p in providers},
                  counter_point=raw['counter_point'], notes=[raw['note'], raw['outside_trace']['note']])
    out.write_text(json.dumps(report, indent=2) + '\n')
    issued = report['per_pixel'][args.backend]
    print(f'{out.name}: ' + ', '.join(f'{k} {v:.3f}' for k, v in issued.items()) + ' rays/pixel'
          + f' (sky cache {"updated" if report["sky_cache_updated"] else "unchanged"},'
          + f' radiance {"injected" if report["radiance_injected"] else "unchanged"})', flush=True)


if __name__ == '__main__':
    main()
