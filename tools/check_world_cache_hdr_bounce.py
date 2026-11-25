"""Check that indirect illumination in a diffuse corner follows HDR source intensity."""
import argparse
import copy
import subprocess
from pathlib import Path

import numpy as np

from gi_stability import DEFAULT_EXE, ROOT, provenance, read_json, write_json, zenapp
from voxelization_fixtures import Fixture, write_half_float_cube


def prepare_corner(folder):
    fixture = Fixture()
    fixture.document.pop('extensionsUsed')
    fixture.document['materials'] = [dict(doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=[.72, .72, .72, 1], metallicFactor=0, roughnessFactor=1))]
    fixture.mesh([[[-2, 0, -2], [-2, 0, 2], [2, 0, 2]], [[-2, 0, -2], [2, 0, 2], [2, 0, -2]]])
    fixture.mesh([[[-2, 0, -1], [2, 0, -1], [2, 2, -1]], [[-2, 0, -1], [2, 2, -1], [-2, 2, -1]]])
    source = fixture.save(folder, 'corner')
    package = folder / 'corner.zenasset'
    subprocess.run([DEFAULT_EXE.with_name('zenassetc.exe'), 'cook', source, '--output', package,
                    '--profile', 'reference'], cwd=ROOT, check=True)
    return source, package


def capture(args, base, label, radiance):
    folder = args.output / label
    frame = folder / 'frames/center'
    frame.mkdir(parents=True)
    faces = np.zeros((6, 128, 128, 4), dtype='<f2')
    faces[..., 3] = 1
    faces[3, 62:66, 62:66, :3] = radiance
    environment = folder / 'sun.ktx'
    write_half_float_cube(environment, faces)
    setup = copy.deepcopy(base)
    setup['rendering']['environment'].update(texturePath=str(environment), lighting=True, skybox=False,
                                             intensity=1, rotationDegrees=0)
    path = frame / 'scene.zenrender'
    write_json(path, setup)
    write_json(folder / 'dataset.json', dict(version=1, frames=[dict(
        id='center', noise_rms=dict(D_direct=1., D_sky=1., D_bounce=1.))]))
    output = folder / 'capture'
    cmd = zenapp(args, [f'--ground-truth={folder}', f'--output={output}', '--frames=1024'], folder / 'run.log')
    actual = read_json(output / 'center.json')
    if (actual['backend'] != 'hardware' or actual['gather'] != 'screen_probes' or not actual['worldCacheCapacity']
            or (actual['width'], actual['height'], actual['sceneFrames']) != (256, 256, 1024)):
        raise ValueError(f'Unexpected capture configuration in {output}')
    raw = np.memmap(output / 'center.lighting.bin', np.float32, 'r').reshape(256, 256, actual['lightingComponents'], 4)
    # A fixed interior patch of the wall faces the floor. Report linear incident light before tone mapping.
    wall = raw[184:202, 96:160]
    if not (wall[..., 7, 3] > 0).all():
        raise ValueError('The wall patch must contain only valid receivers')
    result = {name: float(np.mean(wall[..., index, :3] @ [.2126, .7152, .0722]))
              for name, index in [('sky', 8), ('bounce', 9)]}
    identity = provenance(args, path, [cmd])
    write_json(folder / 'provenance.json', dict(identity, actual=actual))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.executable = DEFAULT_EXE
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        parser.error('Use an empty output directory')
    source, package = prepare_corner(args.output)
    base = read_json(args.setup)
    base.update(sourcePath=str(source), packagePath=str(package), animationEnabled=False)
    rendering = base['rendering']
    rendering.update(scenePath=str(source), normalizationCenter=[0, 0, 0], normalizationScale=1, normalMaps=False, lights=[])
    rendering['cameraLight']['enabled'] = False
    rendering['gi']['optimal'] = True
    rendering['gi']['cone'].update(worldRadianceCache=True, environmentLighting=True, rayTracingPipeline=True, rayProvider=2)
    base['camera'].update(eye=[0, .8, 1.6], target=[0, .48, -1], aspect=1, sceneScale=1, verticalFovDegrees=58.5,
                          nearPlane=.001, farPlane=10, orthographic=False)
    base['run'].update(windowWidth=256, windowHeight=256, renderWidth=256, renderHeight=256, fullscreen=False,
                       followWindow=False, validation=False)
    low = capture(args, base, 'low', 10.)
    high = capture(args, base, 'high', 1000.)
    if min(low.values()) <= 0:
        raise ValueError(f'Expected positive linear illumination in the low-intensity control: {low}')
    ratios = {k: high[k] / (100 * low[k]) for k in low}
    write_json(args.output / 'results.json', dict(low=low, high=high, normalized_response=ratios))
    if abs(ratios['sky'] - 1) > .02 or abs(ratios['bounce'] - 1) > .15:
        raise ValueError(f'HDR illumination did not scale with source intensity: {ratios}')
    print(f"100× source intensity produced {100 * ratios['bounce']:.1f}× bounced irradiance.")


if __name__ == '__main__':
    main()
