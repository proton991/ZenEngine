"""Run native GI coverage setups, including an abrupt camera cut, through ZenApp.

Supply prepared .zenrender files for animated/skinned, outdoor, emissive and glossy
scenes. Retains captures, actual runtime state, validation logs and input/build hashes.
This checks execution and captures coverage evidence; it is not an image-quality gate.
"""
import argparse
import copy
from pathlib import Path
from types import SimpleNamespace

from gi_stability import DEFAULT_EXE, digest, provenance, read_json, write_json, zenapp
from profile_hybrid_gi import running_renderers


def run(args, source, backend):
    folder = args.output / source.stem / backend
    folder.mkdir(parents=True, exist_ok=False)
    setup = copy.deepcopy(read_json(source))
    setup['rendering']['gi']['cone'].update(screenProbeGI=True, worldRadianceCache=True,
                                            worldCacheFeedback=True, nativeProbeBenchmark=False)
    setup['rendering']['gi']['asyncCompute'] = 0
    setup['run'].update(windowWidth=args.width, windowHeight=args.height,
                        renderWidth=args.width, renderHeight=args.height, fullscreen=False, followWindow=False)
    path = folder / 'setup.zenrender'
    write_json(path, setup)
    eye, target = setup['camera']['eye'], setup['camera']['target']
    moved = [target[axis] * 2 - eye[axis] for axis in range(3)]
    # Adjacent keys produce a cut, rather than interpolating through scene geometry.
    camera = folder / 'camera.json'
    write_json(camera, dict(keys=[dict(frame=1, eye=eye, target=target),
                                 dict(frame=96, eye=eye, target=target),
                                 dict(frame=97, eye=moved, target=target),
                                 dict(frame=160, eye=moved, target=target),
                                 dict(frame=161, eye=eye, target=target)]))
    frames = (64, 96, 97, 98, 128, 160, 161, 192)
    identity_args = SimpleNamespace(executable=args.executable, setup=source)
    before = provenance(identity_args, path, [])
    command = zenapp(args, [f'--setup={path}', f'--backend={backend}', '--frames=192', '--fixed-step',
                           '--no-ui', '--validation=1', '--async-compute=0', f'--camera-path={camera}',
                           f'--capture={folder / "capture"}', '--capture-frames=' + ','.join(map(str, frames))],
                     folder / 'console.log')
    log = (folder / 'console.log').read_text(errors='replace')
    for marker in ('[error]', 'VUID-', 'SYNC-HAZARD', 'Validation Error', 'Unknown or incompatible binding'):
        if marker in log:
            raise ValueError(f'{marker}: {folder}')
    after = provenance(identity_args, path, [command])
    for key in ('executable_sha256', 'shaders_sha256', 'inputs', 'setup_sha256'):
        if before[key] != after[key]:
            raise ValueError(f'Measurement input changed: {key}')
    states = []
    for frame in frames:
        state = read_json(str(folder / f'capture.frame-{frame}.ppm') + '.json')
        expected = dict(sceneFrames=frame, width=args.width, height=args.height, framebufferWidth=args.width,
                        framebufferHeight=args.height, fullscreen=False, backend=backend, gather='screen_probes',
                        giWidth=args.width, giHeight=args.height, giDenoiser=False,
                        glossyDenoiser=bool(setup['rendering']['gi']['cone'].get('specularOcclusion', True)))
        for key, value in expected.items():
            if state.get(key) != value:
                raise ValueError(f'{folder}: {key}={state.get(key)!r}, expected {value!r}')
        if state.get('screenProbeCount', 0) <= 0 or state.get('worldCacheCapacity', 0) <= 0:
            raise ValueError(f'{folder}: native resources not active')
        states.append(state)
    write_json(folder / 'report.json', dict(provenance=after, states=states, camera_sha256=digest(camera),
                                           animation_requested=setup.get('animationEnabled', False),
                                           validation_passed=True, image_quality_qualified=False))
    print(folder, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, nargs='+', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--backend', nargs='+', choices=('hardware', 'compute'), default=['hardware'])
    parser.add_argument('--executable', type=Path, default=DEFAULT_EXE)
    parser.add_argument('--width', type=int, default=960)
    parser.add_argument('--height', type=int, default=540)
    args = parser.parse_args()
    if running_renderers():
        parser.error('Close other Zen renderers before running coverage')
    if args.width < 1 or args.height < 1:
        parser.error('Dimensions must be positive')
    args.output = args.output.resolve()
    for source in args.setup:
        for backend in args.backend:
            run(args, source.resolve(), backend)


if __name__ == '__main__':
    main()
