# FBX import fixtures

Except for `combined_morph_skin_ascii.fbx`, the `.fbx` files in this directory are unchanged copies of `data/<filename>`
from [ufbx v0.23.1](https://github.com/ufbx/ufbx/tree/v0.23.1/data),
commit `26a482ae66871d7de36eb722aa060bce95bce274`. They are redistributed
under the upstream MIT license (copyright Samuli Raivio), reproduced in
`LICENSE.ufbx`. The upstream repository also offers a public-domain alternative.

- `maya_cube_7500_ascii` and `maya_cube_7500_binary`: equivalent cube, corner
  normals, tangents, UV seams, centimetre source units, ASCII/binary parsing.
- `max_instanced_material_7700_ascii`: shared geometry with instance materials.
- `synthetic_geometric_transform_7500_ascii`: geometry-only transforms.
- `maya_instanced_pivots_7700_ascii`: pivot transforms with shared meshes.
- `maya_axes_7700_ascii`: coordinate conversion.
- `maya_camera_light_axes_y_up_7700_ascii`: camera/light local axes.
- `blender_293_embedded_textures_7400_binary`: embedded image ownership and roles.
- `maya_uv_and_color_sets_7500_ascii`: named UV sets and secondary corner colors.
- `blender_331_static_blend_shape_7400_binary`: authored nonzero morph weights.
- `blender_293_half_skinned_7400_binary`: weighted and unweighted vertices in one mesh.
- `maya_anim_pivot_rotate_7700_ascii`: animated rotation pivot.
- `maya_anim_no_inherit_scale_7700_ascii`: generated scale-inheritance helpers.
- `maya_anim_interpolation_7700_ascii`: cubic, linear and stepped curves.
- `maya_blend_inbetween_7500_binary`: animated in-between shapes.
- `maya_instanced_skin_7700_binary`: shared skinned mesh with multiple instances.
- `maya_transformed_skin_7700_binary`: transformed skeleton and mesh.
- `blender_279_sausage_7400_binary`: multiple animation clips and clip switching.
- `maya_anim_diffuse_curve_7700_ascii`: animated material factors.
- `maya_anim_light_7500_binary`: animated light parameters.
- `max_transformed_skin_7500_binary`: unsupported dual-quaternion skinning diagnostic.

`combined_morph_skin_ascii.fbx` was authored for these tests and is covered by
the project's license. It combines a morph with six influences per vertex,
mirrored/nonuniform joint scales, a geometric transform and bone/weight
animation. It contains no third-party geometry or textures.

The fixtures are small correctness inputs, not performance benchmarks.
Missing/external-resource and malformed-file tests create temporary variants;
the originals above remain unchanged.

`equivalent_cube.gltf` is a derived counterpart of the ASCII Maya cube under the
same license. Its explicit corner positions/normals/UVs come from that file,
with centimetres converted to metres, V flipped to the engine image convention,
quads triangulated as fans and the Lambert diffuse color/factor mapped to an
explicit metallic/roughness material. Tests match those declared core fields;
they do not claim that arbitrary FBX shader graphs equal glTF materials.
