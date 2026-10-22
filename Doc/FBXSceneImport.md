# FBX scene import

FBX files use the same `ImportScene`, `LoadScene` and `SceneLoader::LoadAsync`
entry points as glTF/GLB. The private adapter uses ufbx v0.23.1, pinned by archive
checksum in `External/CMakeLists.txt`. Parser objects are destroyed before the
canonical `SceneAsset` is returned. No ufbx types appear in public engine headers.

Polygon scenes, linear/rigid skinning, morphs and baked animation stacks are
supported. Empty animation takes without curves are accepted as static scenes.

Coordinates are right-handed, Y-up and metres. ufbx converts units and axes and
introduces helper nodes for geometric transforms and unusual scale inheritance.
The importer preserves helper hierarchy, pivots, shared meshes, node visibility,
and material slot overrides per instance. Viewer centering and normalization
remain a separate runtime operation. Source units and axes are retained in
optional metadata.

Polygon corners are triangulated with independent normal, tangent, color and UV
indices. Mesh jobs run on the shared processing pool, with a bounded result
window and source-ordered assembly. Generated vertices retain source control
point mappings. Missing normals use flat triangle normals. UV sets are retained
in compact slots: slot zero is each mesh's default and named sets have common
scene-wide slots. Additional color sets are retained as attribute records.
Unused material slots create no primitives and impose no UV requirements. If a
material names a UV set absent from a mesh, compatibility mode uses that mesh's
first UV set and reports the requested and selected names. Set
`SceneImportSettings::fallbackMissingNamedUVSets` to `false` for strict rejection.
Bindings on meshes without any UV coordinates always fail. Mesh-worker warnings
are collected per job and published in source order.

The engine uses top-left image rows and UVs. FBX V coordinates are reversed,
texture transforms are converted by the same reversal, and tangent handedness
is adjusted. Normal maps are interpreted as positive-Y tangent-space maps;
their green channel is reversed in a derived image to preserve the original
surface direction. Color/data views share immutable decoded pixels; transformed
normal maps and packed material channels own derived pixels. Source images
remain unchanged. With different normal UV sets across instances, the renderer
derives each instance's tangent frame from its bound UVs.

Each referenced image is decoded once, in parallel, through the shared image
decoder. Embedded content and external files are supported. An external texture
is looked up at ufbx's path relative to the FBX, then at the authored absolute
path, then by file name beside the FBX, which finds textures of relocated assets
whose recorded paths are stale. Network and device paths (`\\server\share`)
outside the FBX's own folder are skipped, so an untrusted file cannot make the
importer contact another host. The file actually read is retained as a
dependency, and earlier missing candidates are recorded so their later
appearance invalidates caches. Missing files and images no decoder accepts (for
example TIFF or DDS) fail the candidate with the decoder's reason; an image is
never published without pixels. The importer creates no GPU resources.

Materials map ufbx PBR values into engine material factors. Base color, normal,
emission and occlusion textures are supported. Separate roughness/metalness
maps are packed into G/B; opacity is packed into base-color alpha. Packing
requires compatible UV sets, transforms and samplers and uses nearest
resampling if map dimensions differ. Legacy Lambert/Phong conversion and
unmapped feature textures produce warnings; scalar factors remain available.
Layered/procedural textures and geometry, constraints, polygon holes, point/line
polygons, and nodes with multiple attributes produce named unsupported-feature
errors. Subdivision control cages are imported with a warning.

Perspective/orthographic cameras and point/directional/spot lights are
converted. FBX relative light intensity has no universal photometric meaning;
it is mapped numerically with a warning. Non-physical decay uses inverse-square
falloff with a warning. Area/volume lights are rejected.

Skins retain every positive influence, normalize their weights and use
geometry-to-bone bind matrices. Unweighted vertices follow the source
evaluator's first-instance geometry transform. Morphs precede skinning.
Each in-between shape becomes an engine morph target; its effective weight is
evaluated during baking. Splitting polygon corners preserves control-point
indices and all influences. Dual-quaternion/blended skinning, multiple skin
deformers, non-normalized cluster modes and geometry caches fail explicitly.
Cluster modes are only available from ufbx's retained document tree, which
copies every array; files are therefore reparsed with it only when they contain
skin clusters.
Arbitrary post-skin deformation graphs are outside this representation.
For combined deformation, source connections must list blend deformers before
the skin. A blend after a skin in that list is conservatively rejected; it is
not silently reordered to match the evaluator's morph-before-skin behavior.
Authored morph normal offsets are retained. When they are absent, smooth
vertices retain their base normals before skinning; this does not reproduce
the source evaluator's topology-based normal regeneration.

Animation is evaluated only during import. Node TRS (including generated
helpers), visibility, effective morph weights, mapped material factors,
camera projection parameters and light parameters become engine tracks. Only
elements that some animation stack animates are baked: nodes with their own
curves, helpers of animated parents, nodes whose non-normal scale inheritance
follows an animated node, meshes with animated blend channels, and animated
materials, cameras and lights. Static elements keep their authored values, so
the bake budget scales with animated content rather than scene size.
Clip times start at zero and retain the source duration and name. Properties
animated in one clip have constant tracks in other clips where needed, so
switching clips restores their authored values. Runtime scenes share immutable
assets but own their material, pose and deformed-vertex state.

Default baking uses 60 samples/second plus source keys and their neighboring
representable float times. Intervals are checked against the source evaluator
at quarter, middle and three-quarter points and subdivided when needed, up to
10 levels. Default absolute local tolerances are 1e-5 metres for translation,
1e-4 radians for rotation, 1e-5 for scale and 1e-4 for other values.
These are sampled local-property checks, not a mathematical bound on all
world-space positions. Settings are recorded on each clip. A 64-million-value
budget bounds bake output across clips; exhaustion or failure to meet the
sampled tolerances rejects the candidate. Cancellation is checked between
evaluations. Baking is serial within a request and can overlap other imports.

Animated texture properties, material shading-mode changes, camera projection
mode changes, light type/decay changes and animated gloss factors combined with
gloss textures are rejected. Existing warnings for unmapped material textures
and approximate material/light conversion also apply to animated scenes.

Tests and fixture provenance are in
`ZenSamples/CommonTest/Assets/FBX/README.md`. CPU tests compare canonical world
geometry and material bindings with ufbx, compare ASCII/binary and equivalent
glTF assets, exercise external/embedded images, and compare serial/parallel
loads. Animation tests compare world matrices and deformed positions with the
source evaluator at keys, neighboring float times and intermediate samples
within 1e-4 metres/component on the fixtures. They cover pivots, inheritance
helpers, in-between shapes, multiple clips, instanced skins and six-influence
combined morph/skin deformation with mirrored/nonuniform scales.
Native editor rendering tests exercise initial deformed poses through PBR and
voxel GI in both RHI execution modes. Native GPU buffer tests advance combined
morph/skin animation and compare uploaded vertices with CPU poses under Vulkan
validation in inline/threaded and async-compute configurations.

## Intel main Sponza validation

The completed `E:/Dev/main_sponza.zip` download was extracted under
`build/scene-import/sponza-source/` for validation. Both
`NewSponza_Main_Yup_003.fbx` and `NewSponza_Main_Zup_003.fbx` passed the Release
CPU import/runtime harness: 8,509,638 vertices, 11,241,066 indices, 224 nodes,
115 renderable nodes, 115 meshes, 405 submeshes and 149 runtime texture views.
Their exports contain no skins or animation clips. Measured complete harness
runs took 33.57 and 32.50 seconds on this workstation; these are validation run
times, not an optimization comparison or a general loading-time guarantee.

This asset exposed two material-part issues. Empty parts retain unused material
slots and must not demand UV streams. Some active `stones_01_tile` bindings also
request `UVChannel_1` on meshes containing only `DiffuseUV` and `UVChannel_2`.
The importer uses the first set with a warning under the default compatibility
policy approved for this task. Strict callers can disable
`fallbackMissingNamedUVSets`; a mesh with no UV set still fails. Worker warnings
merge in source mesh order, so parallel imports preserve diagnostic ordering.

The FBX Y-up source and the supplied glTF source also completed hidden editor
startup and PBR/GI captures with Vulkan validation enabled and no validation
errors reported. The default camera faces an exterior wall against the default
environment; those captures establish upload/render integration, not interior
GI image quality. Source files were left unchanged. Logs and images are in
`build/scene-import/sponza-validation/`.

Internal files and source-cache behavior are documented in
[SceneAssetFormat.md](SceneAssetFormat.md).
