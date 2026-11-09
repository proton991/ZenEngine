"""Analytic GPU estimator fixtures; no engine capture is an input."""
import json
from pathlib import Path
import tempfile

import numpy as np

from environment_reference import Environment
from ground_truth_mitsuba import build_scene, setup_mitsuba
from ground_truth_reference import Materials, PanoramaRadiance, bounce, direct, sky


def plane(height=0, normal=(0, 1, 0), size=100):
    positions = np.array([[-size, height, -size], [-size, height, size],
                          [size, height, size], [size, height, -size]], np.float32)
    return dict(positions=positions, normals=np.tile(normal, (4, 1)), faces=np.array([[0, 1, 2], [0, 2, 3]], np.uint32),
                uv=np.zeros((4, 2)), metal_uv=np.zeros((4, 2)), vertex_color=np.ones((4, 3)),
                material=0, double_sided=True, masked=False, cutoff=.5, alpha=1, vertex_alpha=np.ones(4),
                receiver=True, texture=None)


def fixtures(mi=None):
    mi = mi or setup_mitsuba()
    import drjit as dr
    primitive = plane()
    scene, _, _ = build_scene(mi, [primitive], None, Environment(constant=(0, 0, 0)), 4)
    env = Environment(equirect=np.ones((8, 16, 3), np.float32), intensity=1)
    radiance = PanoramaRadiance(mi, dr, env)
    proposal, _, _ = build_scene(mi, [], None, env, 32)
    points, normals = np.array([[0, .01, 0]], np.float32), np.array([[0, 1, 0]], np.float32)
    open_sky, split = sky(mi, dr, scene, proposal, radiance, points, normals, normals, 32768, 256, 43)
    np.testing.assert_allclose(open_sky, 1, atol=.015)
    light = dict(type=0, intensity=float(np.pi), direction=[0, -1, 0], color=[1, 1, 1], castsShadows=True, range=0)
    d = np.array(direct(mi, dr, scene, mi.Point3f(*points.T), mi.Normal3f(*normals.T), mi.Normal3f(*normals.T),
                        [light], mi.Bool(True))).T
    np.testing.assert_allclose(d, 1, atol=1e-6)
    point_light = dict(type=1, intensity=float(np.pi), position=[0, 2, 0], color=[1, 1, 1], castsShadows=True, range=0)
    d_point = np.array(direct(mi, dr, scene, mi.Point3f(0, 0, 0), mi.Normal3f(0, 1, 0), mi.Normal3f(0, 1, 0),
                              [point_light], mi.Bool(True))).T
    np.testing.assert_allclose(d_point, .25, atol=1e-6)
    # A real bitmap and vertex colors exercise GPU secondary material lookup.
    from PIL import Image
    with tempfile.TemporaryDirectory(prefix='zen-reference-fixture-') as directory:
        directory = Path(directory)
        Image.new('RGB', (2, 2), (128, 128, 128)).save(directory/'color.png')
        document = dict(materials=[dict(pbrMetallicRoughness=dict(baseColorFactor=[.5, .5, .5, 1],
                                                                baseColorTexture=dict(index=0), metallicFactor=0))],
                        textures=[dict(source=0)], images=[dict(uri='color.png')])
        (directory/'scene.gltf').write_text(json.dumps(document))
        materials = Materials(mi, dr, directory/'scene.gltf', [primitive])
        black = PanoramaRadiance(mi, dr, Environment(equirect=np.zeros((4, 8, 3), np.float32)))
        value, halves = bounce(mi, dr, scene, materials, black, np.array([[0, 1, 0]], np.float32),
                               -normals, -normals, [light], 32768, 256, 13)
        expected = ((128/255+.055)/1.055)**2.4*.5*.96
        np.testing.assert_allclose(value, expected, atol=.0002)
    # A box around the receiver blocks all escape directions, including alpha traversal.
    box = []
    for axis in range(3):
        for side in (-1, 1):
            face = plane(side, size=1)
            face['positions'][:, [1, axis]] = face['positions'][:, [axis, 1]]
            face['normals'] = np.zeros((4, 3), np.float32)
            face['normals'][:, axis] = -side
            box.append(face)
    enclosed, _, _ = build_scene(mi, box, None, Environment(constant=(0, 0, 0)), 4)
    closed, _ = sky(mi, dr, enclosed, proposal, radiance, np.zeros((1, 3), np.float32), normals, normals, 1024, 64, 7)
    np.testing.assert_array_equal(closed, 0)
    # Source equirect CPU/GPU convention, rotation, poles and seam agree.
    rng = np.random.default_rng(71)
    panorama = Environment(equirect=rng.random((16, 32, 3)).astype(np.float32), rotation=.7, intensity=.2)
    gpu = PanoramaRadiance(mi, dr, panorama)
    directions = rng.normal(size=(1024, 3))
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    np.testing.assert_allclose(np.array(gpu.eval(mi.Vector3f(*directions.T))).T, panorama.sample(directions), atol=2e-6)
    return dict(open_sky=open_sky.tolist(), direct=d.tolist(), point_light=d_point.tolist(),
                textured_bounce=value.tolist(), closed_box=closed.tolist(), panorama_cpu_gpu=True)


if __name__ == '__main__':
    print(json.dumps(fixtures(), indent=2))
