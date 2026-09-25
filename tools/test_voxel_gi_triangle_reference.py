"""Oracle self-tests: analytic integrals, triangle intersection, texture and light units."""
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

import voxel_gi_triangle_reference as ref
from validate_dynamic_voxel_m7 import triangle_distance


def scene(triangles, materials=None, ids=None, document=None):
    triangles = np.array(triangles, dtype=np.float32)
    count = len(triangles)
    normals = ref.normalize(np.cross(triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0]))
    return ref.TriangleScene(triangles, np.repeat(normals[:, None], 3, axis=1), np.zeros((count, 3, 2, 2)),
        np.ones((count, 3, 4)), np.array(ids if ids is not None else [0]*count),
        document or dict(materials=materials or [{}]), Path('.'))


class TriangleReferenceTests(unittest.TestCase):
    def test_two_sided_nearest_and_finite_segments(self):
        mesh = scene([[[-1,-1,0], [1,-1,0], [0,1,0]], [[-1,-1,-1], [1,-1,-1], [0,1,-1]]])
        origins = np.array([[0,0,1], [0,0,-2], [2,0,1]], dtype=float)
        dirs = np.array([[0,0,-1], [0,0,1], [0,0,-1]], dtype=float)
        ids, t, _, _ = mesh.trace(origins, dirs)
        np.testing.assert_array_equal(ids, [0,1,-1])
        np.testing.assert_allclose(t[:2], [1,1])
        np.testing.assert_array_equal(mesh.trace(origins, dirs, .99)[0], [-1,-1,-1])

    def test_embree_against_independent_double_precision_moller(self):
        rng = np.random.default_rng(41)
        triangles = rng.uniform(-1, 1, (35, 3, 3))
        mesh = scene(triangles)
        origins = rng.uniform(-2, 2, (128, 3))
        dirs = ref.normalize(rng.uniform(-1, 1, (128, 3)))
        ids, distance, _, _ = mesh.trace(origins, dirs)
        for i in range(len(origins)):
            expected = min((triangle_distance(origins[i], dirs[i], tri), index) for index, tri in enumerate(mesh.vertices))
            self.assertEqual(ids[i], expected[1] if np.isfinite(expected[0]) else -1)
            if ids[i] >= 0:
                self.assertAlmostEqual(distance[i], expected[0], delta=2e-5)

    def test_alpha_continuation_and_opaque_alpha_zero(self):
        triangles = [[[-1,-1,z], [1,-1,z], [0,1,z]] for z in (0,-1)]
        materials = [dict(alphaMode='MASK', alphaCutoff=.5, pbrMetallicRoughness=dict(baseColorFactor=[1,1,1,0])),
                     dict(pbrMetallicRoughness=dict(baseColorFactor=[1,1,1,0]))]
        mesh = scene(triangles, materials, [0,1])
        ids, distance, _, _ = mesh.trace(np.array([[0.,0,1]]), np.array([[0.,0,-1]]))
        self.assertEqual(ids[0], 1)
        self.assertAlmostEqual(distance[0], 2, delta=1e-6)

    def test_srgb_before_bilinear_repeat_and_linear_alpha(self):
        pixels = np.array([[[0,0,0,0], [255,255,255,255]]], dtype='u1')
        actual = ref.sample_image(pixels, np.array([[.5,.5], [1.5,.5], [.25,.5]]), True)
        np.testing.assert_allclose(actual, [[.5]*4, [.5]*4, [0]*4])
        gray = np.array([[[128,128,128,128]]], dtype='u1')
        sample = ref.sample_image(gray, np.array([[0,0]]), True)[0]
        self.assertAlmostEqual(sample[0], .2158605001)
        self.assertAlmostEqual(sample[3], 128/255)

    def test_cosine_sampler_normalization_and_integral(self):
        normals = ref.normalize(np.array([[0.,0,1], [1,2,3], [0,-1,0]]))
        rays = ref.cosine_directions(normals, 8192, 12)
        np.testing.assert_allclose(np.linalg.norm(rays, axis=2), 1, atol=1e-12)
        cosine = np.einsum('psj,pj->ps', rays, normals)
        self.assertGreaterEqual(cosine.min(), 0)
        np.testing.assert_allclose(cosine.mean(axis=1), 2/3, atol=1e-4)

    def test_point_directional_and_spot_units(self):
        light = dict(position_range=[0,0,2,4], direction_type=[0,0,-1,1],
                     color_intensity=[1,.5,.25,3], cone_shadow=[.9,.8,1,0])
        direction, distance, energy = ref.light_incident(light, np.array([[0.,0,0], [0,0,-3]]))
        np.testing.assert_allclose(direction, [[0,0,1]]*2)
        np.testing.assert_allclose(distance, [2,5])
        np.testing.assert_allclose(energy[0], np.array([3,1.5,.75])*(1-.5**4)**2/4.01)
        np.testing.assert_array_equal(energy[1], [0,0,0])
        light['direction_type'][3] = 0
        np.testing.assert_allclose(ref.light_incident(light, np.array([[0.,0,0]]))[2], [[3,1.5,.75]])
        light['direction_type'][3] = 2
        self.assertGreater(ref.light_incident(light, np.array([[0.,0,0]]))[2][0,0], 0)
        np.testing.assert_array_equal(ref.light_incident(light, np.array([[3.,0,2]]))[2], [[0,0,0]])

    def test_constant_emitter_integral_receiver_pi_metal_ao(self):
        triangles = [[[-1000,-1000,1], [1000,-1000,1], [1000,1000,1]],
                     [[-1000,-1000,1], [1000,1000,1], [-1000,1000,1]]]
        mesh = scene(triangles, [dict(emissiveFactor=[4,2,1])])
        surfaces = np.zeros((3,24)); surfaces[:,3] = 1
        surfaces[:,4:7] = surfaces[:,8:11] = [0,0,1]
        surfaces[:,16:19] = [.7,.5,.3]; surfaces[:,20] = 1
        surfaces[1,19] = 1; surfaces[2,20] = .5
        meta = dict(lights=[], camera_position=[0,0,2], indirect_intensity=1)
        value = ref.integrate(mesh, surfaces, meta, 1024, 41, .0006, 1e-5)
        expected = np.array([4,2,1])*np.array([.7,.5,.3])*.96
        np.testing.assert_allclose(value[0], expected, rtol=1e-6)
        np.testing.assert_allclose(value[1], 0, atol=1e-12)
        np.testing.assert_allclose(value[2], expected*.5, rtol=1e-6)

    def test_export_transform_and_attribute_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp)/'mesh'
            Path(str(prefix)+'.json').write_text(json.dumps(dict(vertex_stride=112, node_stride=128, triangle_count=1)))
            vertex = np.zeros((3,28), dtype='<f4')
            vertex[:,:3] = [[0,0,0], [1,0,0], [0,1,0]]
            vertex[:,4:7] = [0,0,1]; vertex[:,24:28] = [.2,.4,.6,1]
            vertex[:,12:16] = [.1,.2,.3,.4]
            vertex.tofile(str(prefix)+'.vertices.bin')
            np.array([0,1,2], '<u4').tofile(str(prefix)+'.indices.bin')
            transform = np.diag([2.,3.,4.,1.]); transform[:3,3] = [1,2,3]
            np.stack([transform, np.linalg.inv(transform).T]).transpose(0,2,1).astype('<f4').tofile(str(prefix)+'.nodes.bin')
            np.array([[0,0,0,1]], '<u4').tofile(str(prefix)+'.triangles.bin')
            asset = Path(tmp)/'asset.gltf'; asset.write_text('{}')
            mesh = ref.TriangleScene.from_capture(prefix, asset)
            np.testing.assert_allclose(mesh.vertices[0], [[1,2,3], [3,2,3], [1,5,3]])
            np.testing.assert_allclose(ref.normalize(mesh.normals[0]), [[0,0,1]]*3)
            np.testing.assert_allclose(mesh.uv[0,0], [[.1,.2], [.3,.4]])

    def test_smooth_normal_interpolation_after_nonuniform_transform(self):
        mesh = scene([[[-1,-1,1], [1,-1,1], [0,1,1]]])
        mesh.normals[0] = [[0,0,.25], [.5,0,0], [0,1/3,0]]
        actual = mesh.surface(np.array([0]), np.array([.25]), np.array([.25]))[1]
        expected = ref.normalize(np.array([[.125, 1/12, .125]]))
        np.testing.assert_allclose(actual, expected)

    def test_shading_normal_does_not_create_self_bounce_through_back_hemisphere(self):
        mesh = scene([[[-1000,-1000,0], [1000,-1000,0], [1000,1000,0]],
                      [[-1000,-1000,0], [1000,1000,0], [-1000,1000,0]]], [dict(emissiveFactor=[4,2,1])])
        surface = np.zeros((1,24)); surface[:,4:7] = [1,0,0]; surface[:,8:11] = [0,0,1]
        surface[:,16:19] = .7; surface[:,20] = 1
        meta = dict(lights=[], camera_position=[2,0,2], indirect_intensity=1)
        value = ref.integrate(mesh, surface, meta, 1024, 41, .0006, 1e-5)
        np.testing.assert_array_equal(value, [[0,0,0]])

    def test_visibility_fast_path_matches_full_material_evaluation(self):
        materials = [dict(alphaMode='MASK', alphaCutoff=.5, pbrMetallicRoughness=dict(baseColorFactor=[1,1,1,.6]))]
        mesh = scene([[[-1,-1,0], [1,-1,0], [0,1,0]]], materials)
        mesh.colors[0,:,3] = [.1, .9, 1]
        ids = np.zeros(3, dtype=int); u = np.array([0., .8, .1]); v = np.array([0., .1, .8])
        np.testing.assert_array_equal(mesh.visible(ids, u, v), mesh.surface(ids, u, v)[0])


if __name__ == '__main__':
    unittest.main()
