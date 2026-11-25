"""Render compact colored HDR sources on a rough metal plate and check diffuse and specular RGB.
Run with a native-GI .zenrender template, an empty output directory, and the release ZenApp build.
"""
import argparse
import copy
import subprocess
from pathlib import Path

import numpy as np
from PIL import Image

from gi_stability import DEFAULT_EXE, ROOT, read_json, write_json, zenapp, provenance
from voxelization_fixtures import Fixture, write_half_float_cube

SIDE = 192
LUMA = np.array([.2126, .7152, .0722])


def prepare_plate(folder, executable):
    fixture = Fixture()
    fixture.document.pop('extensionsUsed')
    fixture.document['materials'] = [dict(doubleSided=True, pbrMetallicRoughness=dict(
        baseColorFactor=[1, 1, 1, 1], metallicFactor=1, roughnessFactor=.65))]
    fixture.mesh([[[-1, 0, -1], [-1, 0, 1], [1, 0, 1]], [[-1, 0, -1], [1, 0, 1], [1, 0, -1]]])
    source = fixture.save(folder, 'plate')
    package = folder / 'plate.zenasset'
    subprocess.run([executable.with_name('zenassetc.exe'), 'cook', source, '--output', package,
                    '--profile', 'reference'], cwd=ROOT, check=True)
    return source, package


def write_cube(path, name):
    side = 128
    faces = np.zeros((6, side, side, 4), dtype='<f2')
    faces[..., 3] = 1
    # Source cubemaps use inverted Y. Both directions are above the plate.
    if name in ('red', 'both', 'mixed'):
        faces[3, 69:72, 67:70, 0] = 900
    if name in ('blue', 'both'):
        faces[0, 87:90, 68:71, 2] = 1800
    if name == 'mixed':
        faces[3, 69:72, 74:77, 2] = 900 * LUMA[0] / LUMA[2]
    if name == 'mixed-white':
        for x in (67, 74):
            faces[3, 69:72, x:x + 3, :3] = 900 * LUMA[0]
    write_half_float_cube(path, faces)


def linear_radiance(display_rgb):
    srgb = np.asarray(display_rgb) / 255.
    linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    return linear / (1. - linear)


def check_rgb(rgb):
    # Two equal-luminance patches in the same tile have slightly different solid angles.
    # Each primary must carry roughly half of the neutral control's luminance.
    mixed = linear_radiance(rgb['mixed']) * LUMA
    neutral = np.dot(linear_radiance(rgb['mixed-white']), LUMA)
    if neutral < .05 or not np.all(np.abs(mixed[[0, 2]] / neutral - .5) < .04):
        raise ValueError(f'Mixed source tile lost per-channel energy: {rgb}')
    red, blue, both = (np.asarray(rgb[key]) for key in ('red', 'blue', 'both'))
    if min(red[0], blue[2]) < 32 or max(red[0], blue[2]) > 245 or np.max(np.abs(both - red - blue)) > 2:
        raise ValueError(f'Adding a disjoint-colored source changed the other source’s rough reflection: {rgb}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--setup', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.executable = DEFAULT_EXE
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error('Use an empty output directory for the new captures')
    source, package = prepare_plate(out, args.executable)
    base = read_json(args.setup)
    base.update(sourcePath=str(source), packagePath=str(package), animationEnabled=False)
    base['rendering'].update(scenePath=str(source), normalizationCenter=[0, 0, 0], normalizationScale=1,
                             normalMaps=False, lights=[])
    base['rendering']['cameraLight']['enabled'] = False
    base['rendering']['gi']['optimal'] = True
    base['rendering']['gi']['cone'].update(worldRadianceCache=True, environmentLighting=True, specularOcclusion=True)
    base['camera'].update(eye=[0, .85, .02], target=[0, 0, 0], aspect=1, sceneScale=1, nearPlane=.001,
                          farPlane=10, orthographic=False)
    base['run'].update(windowWidth=SIDE, windowHeight=SIDE, renderWidth=SIDE, renderHeight=SIDE,
                       fullscreen=False, followWindow=False)
    rgb, runs = {}, {}
    for name, face in [('mixed', 0), ('mixed-white', 0), ('red', 1), ('blue', 1), ('both', 1)]:
        folder = out / name
        folder.mkdir()
        cube = folder / 'environment.ktx'
        write_cube(cube, name)
        scene = copy.deepcopy(base)
        scene['rendering']['environment'].update(texturePath=str(cube), intensity=1, rotationDegrees=0, lighting=True, skybox=False)
        scene['rendering']['debug'].update(output=7, face=face)
        setup = folder / 'scene.zenrender'
        write_json(setup, scene)
        command = zenapp(args, [f'--setup={setup}', '--frames=100', f'--width={SIDE}', f'--height={SIDE}',
                                '--windowed', '--vsync=0', '--fixed-step', '--no-ui', '--rhi=threaded',
                                '--backend=hardware', '--async-compute=1', '--validation=0',
                                f'--capture={folder / "image"}', '--capture-frames=100'], folder / 'zenapp.log')
        capture = folder / 'image.frame-100.ppm'
        state = read_json(str(capture) + '.json')
        if (not state['rayTracingPipeline'] or state['gather'] != 'screen_probes' or not state['worldCacheCapacity']
                or (state['giWidth'], state['giHeight']) != (SIDE, SIDE)):
            raise ValueError(f'Expected native hardware screen probes in {capture}')
        with Image.open(capture) as image:
            rgb[name] = np.median(np.asarray(image)[88:104, 88:104], axis=(0, 1)).tolist()
        runs[name] = provenance(args, setup, [command])
    write_json(out / 'results.json', dict(rgb=rgb, runs=runs))
    check_rgb(rgb)
    print('Passed mixed-tile irradiance and rough-specular source additivity.')


if __name__ == '__main__':
    main()
