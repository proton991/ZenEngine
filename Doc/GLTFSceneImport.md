# glTF scene import and rendering

`FastGLTFLoader` and `GLTFLoader` share the glTF 2.0 importer for `.gltf` and `.glb`. Imports are transactional: parser, validation, decompression, texture, or deformation failures leave the previous scene and geometry intact. Unsupported required extensions produce named diagnostics. JSON uses a padded parser buffer, including GLB chunks at page boundaries. Valid integer properties written with decimals or exponents normalize exactly for parser compatibility; authored source JSON and binary chunk offsets are preserved. Windows paths and external resources support UTF-8 filenames.

The target is the supplied `D:/Dev/glTF-Sample-Assets` corpus and its mesh, material, animation, lighting, and visibility features. Gaussian splatting, interactivity graphs, and remaining vendor-specific runtime systems are deferred at the user's request. Named TODOs live beside the extension configuration in `FastGLTFLoader.cpp`. Optional extension data and extras remain in `SceneAssetData::sourceDocument`; retaining that data does not implement the extension's behavior.

## Geometry and textures

The default scene is selected, or the first scene when no default is authored. Without scenes, root-node forests are imported. Names, non-mesh ancestors, exact matrix/TRS transforms, and instances survive import. Iterative hierarchy traversal avoids stack overflow. Empty scenes and singular transforms have finite bounds and normal matrices.

All seven primitive modes are supported: points, line lists/loops/strips, and triangle lists/strips/fans. Sparse, interleaved, normalized, and quantized accessors are decoded. Native paths handle `KHR_draco_mesh_compression`, `EXT_meshopt_compression`, `KHR_meshopt_compression`, and `EXT_mesh_gpu_instancing`.

Positions, normals, tangents, vertex colors, all authored UV sets, and paired `JOINTS_n`/`WEIGHTS_n` sets are consumed. Additional colors and custom attributes retain decoded values and metadata. Custom morph attributes also retain their target index; their application-defined semantics do not alter the standard material. Sparse UV semantic numbers map to compact GPU slots; `vertexTexCoordSets` records original IDs. Active texture bindings require the corresponding primitive UV set. Flat-normal splitting preserves source indices for deformation. Mirrored transforms preserve face visibility and tangent handedness.

Skin and morph deformation run at import and during animation. Independent instances get independent vertex ranges. Morph deltas precede skinning; position, normal, tangent, UV, and primary-color deltas update without accumulation. All influence sets, inverse-bind matrices, normals/tangents, and posed bounds are handled. GPU instances follow their source node's animated weights, including when default weights are omitted. `Scene::SetMaterialVariant()` selects `KHR_materials_variants` mappings or restores default materials.

PNG/JPEG, `EXT_texture_webp`, and `KHR_texture_basisu` decode to RGBA pixels. Authored Basis Universal mip chains upload without regeneration. Color textures use sRGB and data textures use linear formats, including distinct interpretations of shared sources. Equivalent image/sampler/color-space definitions and samplers are deduplicated. Filtering, wrapping, arbitrary UV sets, and `KHR_texture_transform` apply in material, shadow, and voxel sampling.

## Materials

The expanded GPU layout and shaders consume core metallic/roughness, alpha modes, normal/occlusion/emissive maps and these extensions:

| Extensions | Implemented behavior |
| --- | --- |
| unlit, emissive_strength | Authored unlit colors, linear alpha composition, HDR emission |
| pbrSpecularGlossiness | Diffuse/specular/glossiness workflow |
| ior, specular | Dielectric IOR and specular weight/color, including IOR zero |
| clearcoat, sheen | Coat and cloth lobes with layer attenuation/energy compensation |
| transmission, volume, dispersion | Rough refraction, thickness, attenuation, wavelength-dependent refraction |
| iridescence, anisotropy | Thin-film interference and directional microfacet response |
| diffuse_transmission | Backside diffuse transmission |
| volume_scatter (draft) | Visible-surface Burley diffusion and scattering factors |
| retroreflection (draft) | Direct-light and environment retroreflection |

All material names above use the `KHR_materials_` prefix, including `diffuse_transmission`. Their texture bindings and authored factors contribute to shading. The corpus uses the older `KHR_materials_volume_scatter` draft; the newer renamed `KHR_materials_scatter` draft is not implemented.

Opaque core surfaces retain the deferred path. Advanced materials, transparency, and point/line primitives use forward HDR. Transmission samples opaque-color mips. BLEND triangles sort globally by camera depth; MASK uses the authored cutoff. Scenes with unlit BLEND composite in display-linear color to preserve opacity. Alpha and transmission affect stochastic shadow coverage.

## Animation, lighting and cameras

`SceneAnimation` evaluates STEP, LINEAR, and CUBICSPLINE, including duration-scaled tangents, scalar morph-weight groups, and shortest-path quaternion interpolation. The renderer plays the first clip by default; `SetAnimation()` changes it. Vertex buffers, transforms, materials, visibility, lights, camera parameters, and geometry/surface revisions update.

`KHR_animation_pointer` covers implemented material factors and texture transforms, node TRS/morph weights, visibility/selectability/hoverability, camera and punctual-light parameters, and image-based-light intensity/rotation. Samples validate before channel writes. `KHR_node_visibility` includes inherited mesh and light visibility; hidden authored lights do not trigger fallback lights. Selectability/hoverability are retained for interaction clients. XMP metadata remains in the source document.

Each selected `KHR_lights_punctual` node creates an inherited light instance. Range, cone angles, and zero intensity retain their meanings. Without authored lights, six generated lights lie outside scene bounds. `EXT_lights_image_based` uploads authored HDR cube faces/specular mips and reconstructs irradiance from authored SH coefficients. Pointer animation updates environment orientation/intensity.

Perspective and orthographic cameras retain orientation, FOV, aspect, clipping, and magnitudes; omitted far planes use infinite projection. The demo selects the first imported camera or frames the bounds. Normalization consistently changes geometry, cameras, light ranges, inverse-square intensities, and material attenuation distances. Physical thickness scaling remains available for CPU-skinned geometry.

## Practical limits

Corpus success does not establish pixel-perfect reference-renderer conformance for every glTF.

- Refraction uses one opaque screen-color snapshot and environment fallback. Recursive transparent refraction is not traced.
- Triangle sorting cannot solve cyclic/intersecting transparency.
- Visible-surface volume diffusion approximates offscreen/backface scattering.
- Stochastic depth shadows retain scalar transmitted coverage, without colored transmissive shadows or refractive caustics.
- The renderer has 32 simultaneous punctual lights and fixed heaps of 2048 2D textures, 64 cube textures, and 128 samplers. Device dimensions, allocation, and buffer limits apply.
- Forward scenes do not emit legacy diagnostic lighting-component capture buffers.
- Deferred extension TODOs need separate runtimes/renderers. `KHR_accessor_float64` also needs double-precision animation interpolation before support can be advertised. Unsupported required extensions fail explicitly; optional fallbacks remain available.

## Reproducible verification

Build `CommonTest`, `ConfigLoaderTest`, `RenderCoreTest`, `GLTFCorpusImport`, and `scene_renderer_demo`. Shader variants compile with the build. The native verifier isolates models, checks retained structure, samples every animation, tests variants, and verifies finite geometry/matrices and posed bounds.

```powershell
python tools/verify_gltf_corpus.py --assets D:/Dev/glTF-Sample-Assets --importer build/dependency-migration/debug/bin/GLTFCorpusImport.exe --report build/gltf-corpus-results.json --jobs 2
python tools/smoke_gltf_rendering.py --all --assets D:/Dev/glTF-Sample-Assets --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe --output build/gltf-corpus-render --mode 2 --frames 3 --width 640 --height 480 --timeout 180
python tools/smoke_gltf_rendering.py --assets D:/Dev/glTF-Sample-Assets --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe --output build/gltf-gi-final --mode 3
python tools/verify_gltf_material_effects.py prepare
python tools/verify_gltf_material_effects.py render --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe
python tools/verify_gltf_material_effects.py analyze
python tools/verify_gltf_unlit_alpha.py prepare
python tools/verify_gltf_unlit_alpha.py render --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe
python tools/verify_gltf_unlit_alpha.py analyze
python tools/verify_gltf_unit_scaling.py prepare
python tools/verify_gltf_unit_scaling.py render --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe
python tools/verify_gltf_unit_scaling.py analyze
python tools/verify_gltf_cameras.py prepare
python tools/verify_gltf_cameras.py render --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe
python tools/verify_gltf_cameras.py analyze
python tools/verify_gltf_review_regressions.py --renderer build/dependency-migration/debug/bin/scene_renderer_demo.exe
```

Run render scripts sequentially; they restore the original `Data/engine.cfg` bytes. Vulkan validation, capture integrity, material-factor ablations, known-color alpha probes, and equivalent-unit renders cover different failures. Completely black captures with active geometry fail the render check. Ablations establish visible foreground contributions, not reference-image conformance.

The review regression script checks specular-glossiness color, diffuse weight, masked coverage, and averaged reflectance through both voxelizers. It reads back deferred emission and voxel HDR values above the half-float range, and compares clearcoat images with different UV sets/transforms but identical normal texels and a shared base tangent frame. Unit tests cover composed glTF/GLB parser rewrites, invalid Draco references, skinning orientation independent of thickness scale, and UV-buffer reflection bindings.

Verified on 2026-10-01 with the supplied corpus:

- Native import: 339/339 variants, including 279 animation samples and 405 material-variant selections.
- Vulkan rendering: 339/339 variants produced captures with no validation errors, crashes, or timeouts.
- Voxel-GI rendering: 18/18 representative models passed. The complete final GPU verification included 390 renders across the corpus and targeted probes, with no Vulkan validation errors.
- Unit suites: CommonTest 84/84, RenderCoreTest 518/518, ConfigLoaderTest 21/21; no allocator leaks.
- Material probes: 11/11 extension ablations changed occupied geometry across 22 successful renders.
- Unlit alpha probes match the expected sRGB values exactly: white over red `(255, 188, 188)` and white over black `(188, 188, 188)`.
- Camera probes: six correctness cases across eight captures, covering both rendering paths. Orthographic lighting/background probes are uniform; equivalent finite/infinite far-plane renders are pixel-identical.
- Equivalent physical-unit renders are pixel-identical.

Reports and capture previews are written under `build/gltf-corpus-results.json`, `build/gltf-corpus-render`, `build/gltf-gi-final`, `build/gltf-material-effects`, `build/gltf-unlit-alpha`, `build/gltf-cameras`, and `build/gltf-unit-scaling`. They distinguish structural/runtime checks from reference-image conformance. Contact-sheet inspection covered all 151 distinct models.

References: [glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html), [extension registry](https://github.com/KhronosGroup/glTF/tree/main/extensions), [sample assets](https://github.com/KhronosGroup/glTF-Sample-Assets).
