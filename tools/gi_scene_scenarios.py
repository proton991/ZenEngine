"""Generate New Sponza denoiser replay inputs from its saved ZenApp setup."""
import argparse
from copy import deepcopy
from math import cos, sin, tau
from pathlib import Path

from gi_quick_gates import base_pose, key
from gi_spark_metric import rotate_target
from gi_stability import read_json, write_json


def scene_paths(light):
    start = {k: light[k] for k in ('position', 'color', 'intensity', 'enabled')}
    point = dict(color=[1, .9, .7], intensity=.003, enabled=True)
    proxy = dict(point, position=[.024, -.202, -.148], frame=1)
    cycle = dict(point, position=[-.02, -.15, -.12], frame=1)

    def lamp(keys):
        return dict(target='light', lightId=1, keys=keys)

    object_keys = [dict(frame=1, position=[0, 0, 0]), dict(frame=513, position=[.08, 0, .06])]
    sky_keys = [dict(frame=1, intensity=1, rotationDegrees=0), dict(frame=513, intensity=.5, rotationDegrees=45)]
    floor = [dict(frame=1, color=[1, 1, 1], emission=[0, 0, 0]),
             dict(frame=513, color=[.35, .8, .45], emission=[.025, .075, .025])]
    bulb = [dict(frame=1, color=[1, 1, 1], emission=[1, 1, 1]),
            dict(frame=1025, color=[.45, .85, .55], emission=[.25, .75, .4])]
    sweep = [dict(frame=1, position=[.178, -.243, -.069])]
    for step in range(1, 49):
        sweep.append(dict(frame=1024 + step, position=[.178, -.243, round(-.069 + .0025 * step, 6)]))
    rotation = [sky_keys[0]] + [dict(frame=1024+i, rotationDegrees=i, intensity=1-.5*i/45) for i in range(1, 46)]
    motion = [dict(cycle, frame=513+i, position=[round(-.02+.025*min(i%16, 16-i%16), 6), -.15, -.12])
              for i in range(512)]
    profile = []
    for i in range(7500):
        angle = tau * (i % 80) / 80
        profile.append(dict(point, frame=i+1, position=[round(.08+.12*sin(angle), 6), -.15, round(-.03-.09*cos(angle), 6)]))
    drag = [dict(start, frame=1)]
    column = [proxy]
    for i in range(1, 17):
        drag.append(dict(start, frame=1024+i, intensity=light['intensity']*(1+i/16), color=[1-.5*i/16, .9, .7+.25*i/16]))
        column.append(dict(proxy, frame=512+i, position=[round(.024+.005*i, 6), -.202, -.148]))
    return {
        'material-step': dict(target='material', index=19, keys=floor),
        'bulb-material': dict(target='material', index=12, keys=bulb),
        'object-step': dict(target='instance', index=5, keys=object_keys),
        'object-step-1024': dict(target='instance', index=5, keys=[object_keys[0], dict(object_keys[1], frame=1025)]),
        'column-sweep': dict(target='instance', index=5, keys=sweep),
        'stable-sky': dict(target='environment', keys=sky_keys[:1]),
        'environment-step': dict(target='environment', keys=sky_keys),
        'environment-step-1024': dict(target='environment', keys=[sky_keys[0], dict(sky_keys[1], frame=1025)]),
        'sky-rotation': dict(target='environment', keys=rotation),
        'lamp-step': lamp([dict(start, frame=1), dict(start, frame=1025, enabled=False)]),
        'lamp-drag': lamp(drag), 'proxy-column': lamp(column), 'proxy-dark': lamp([dict(proxy, enabled=False)]),
        'room-cycle': lamp([cycle] + motion), 'room-profile': lamp(profile),
        'skin-poses': dict(target='animation', keys=[dict(frame=1, seconds=0), dict(frame=257, seconds=.3),
                                                   dict(frame=321, seconds=.65)]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', required=True, type=Path, help='Saved native 4K New Sponza setup')
    parser.add_argument('--output', required=True, type=Path, help='New directory for paths and setup variants')
    args = parser.parse_args()
    source = read_json(args.setup)
    lights = source['rendering']['lights']
    light = next((entry['light'] for entry in lights if entry['id'] == 1), None)
    if not light or source['rendering']['cameraLight']['enabled']:
        parser.error('Use the New Sponza fixture with authored light ID 1 and the camera light disabled')
    args.output.mkdir(parents=True, exist_ok=False)
    for name, path in scene_paths(light).items():
        write_json(args.output / f'{name}.json', path)
    eye, target = base_pose(source)
    poses = [(1, 0), (512, 0), (513, 1), (516, 4), (520, 8), (524, 12), (532, 12)]
    write_json(args.output / 'age-pan.json', dict(keys=[key(f, (eye, rotate_target(eye, target, a) if a else target))
                                                        for f, a in poses]))
    for name, radius in [('local', None), ('finite-proxy', .12), ('room-wide', 1000), ('sky-only', None)]:
        setup = deepcopy(source)
        rendering = setup['rendering']
        if radius is not None:
            moving = next(entry['light'] for entry in rendering['lights'] if entry['id'] == 1)
            setup['rendering']['gi']['cone']['worldCacheSeed'] = False
            moving.update(range=radius, intensity=.003, position=[.08, -.15, -.12], movable=True)
        if name == 'sky-only':
            for entry in rendering['lights']:
                entry['light']['enabled'] = False
            rendering['gi']['optimal'] = False
            rendering['gi']['cone'].update(screenProbeGI=True, analyticLighting=False, emissiveLighting=False)
        for local in (True, False):
            rendering['gi']['cone']['localHistoryResponse'] = local
            output = name if local else 'global' if name == 'local' else f'{name}-global'
            write_json(args.output / f'{output}.zenrender', setup)


if __name__ == '__main__':
    main()
