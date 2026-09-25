# Voxel GI implementation and verification

Date: 2026-09-21. Implementation follows [VoxelGIImplementationPlan.md](VoxelGIImplementationPlan.md), with execution refinements recorded there.

## Directional GI update (2026-09-25)

The explicit `voxel_gi_method=dynamic_voxel` path implements the paper-derived single-bounce diffuse pipeline using ordinary compute voxel DDA. It includes static/dynamic transport, zero through 32 analytic lights, environment and emissive transport, temporal/spatial filtering, and runtime method switching. See [M7 verification](DynamicVoxelGIM7Verification.md) and the [directional GI plan](DynamicVoxelGIImplementationPlan.md) for acceptance evidence and remaining work. The original cone implementation and its historical results below remain applicable to `auto` and `cone`.

Use `dynamic_voxel_gi_query_backend=voxel_dda`, 128 rays per face, a verified 64/128 grid and an explicit `dynamic_voxel_gi_memory_budget_mb`. The M7 fixtures use 3072 MiB; 64-cubed Sponza uses 6144 MiB. These caps cover the checked method resources, not total measured device memory or a shipping preset. If resolution is omitted, explicit directional GI defaults to 64 and cone/auto to 256; an existing explicit resolution is preserved. Budget/capability rejection, cache initialization, overflow and unknown visibility use cone fallback. Only rejection/fallback is verified at 256 cubed. `auto` remains cone pending M8 profiling.

`dynamic_voxel_gi_temporal_filter=off|fixed|elapsed`, `dynamic_voxel_gi_spatial_filter`, and the independent `dynamic_voxel_gi_analytic_lighting`, `dynamic_voxel_gi_environment_lighting`, and `dynamic_voxel_gi_emissive_lighting` controls affect directional diffuse transport. `skybox_visible` changes background visibility independently of environment lighting. Average diffuse reflectance is selectable with `voxel_reflectance_policy=averaged` and its separate V1 budget; owner remains the compatibility default.

`RendererServer::SetVoxelGIMethod` changes the requested method between frame recordings, resets history and retains compatible, budgeted static caches; the grid stays fixed at startup. `--disable-rt` disables native RT features before device creation. The M7 runner uses this flag for all its scene captures. `--gi-method-switching` is a diagnostic for the generated four-mesh lifecycle fixture, used with `--capture-lighting=prefix`; it is not a general animation command. Full directional volume capture supports 64/128 only because a combined 256-cubed readback exceeds the current RHI buffer-size type.

DDA has cell-shaped occlusion and representative-surface error, and Gaussian filtering can leak across surfaces. Current specular IBL remains separate and unoccluded. Hardware triangle queries, multi-bounce/glossy transport and a measured frame-time claim are outside this delivery; M8 profiling and deferred H0-H2 remain.

## Using the demo

Build from an initialized x64 MSVC developer environment, then run from `bin`:

```powershell
cmake --preset x64-windows-msvc-debug
cmake --build build/x64-windows-msvc-debug --target scene_renderer_demo ConfigLoaderTest CommonTest RenderCoreTest VulkanRHITest
cd bin
./scene_renderer_demo.exe --mode=3
```

- `1` / keypad 1: voxel visualization.
- `2` / keypad 2: deferred PBR with environment IBL.
- `3` / keypad 3: deferred PBR with mesh-based direct shadows and the selected voxel GI method (`auto` defaults to cone).
- `R` in mode 1 or 3: rebuild geometry and invalidate dependent lighting.
- `--capture=frame.ppm --frames=3 --mode=3`: save the final framebuffer. Capture waits for completion explicitly; ordinary rendering does not add this wait.

Configure `Data/engine.cfg`. This file is ignored by Git; [Data/engine.example.cfg](../Data/engine.example.cfg) is the tracked complete example. Adjust `model_base_path` to the glTF Sample Assets checkout. The existing local configuration has the GI controls and an animated fifth point light. Optional `default_model_path` and `camera_position` support reproducible custom scenes.

`light_count=0` disables analytic lights. Omitting `light_count` preserves four default point lights. Indexed lights support `type=point|directional|spot`, `position`, `direction`, `color`, `intensity`, `range`, `inner_angle_degrees`, `outer_angle_degrees`, `enabled`, and `casts_shadows`. Vectors are comma separated; colors are linear RGB. Positions, orbit centers, and ranges use the centered/normalized renderer world. Light intensity is a renderer radiance scale; point/spot attenuation includes smooth range falloff.

`dynamic_light.enabled`, `.index`, `.orbit_center`, `.orbit_radius`, and `.angular_speed_degrees` animate the selected point/spot light in seconds. Runtime editing uses `RenderScene::GetLights().Add/Update/Remove/Find` and stable IDs; it is not tied to the demo animation. A UI can use the same API, `RenderScene::SetEnvironmentLighting`, and `RendererServer::RequestVoxelGI()->SetSettings`.

Environment controls are `environment_texture`, `environment_lighting`, `environment_intensity`, `environment_rotation_degrees`, and `skybox_visible`. The supported environment input is an RGBA16F cubemap under `Data/Textures`; a missing/unsupported input logs a warning and supplies black. Background visibility and lighting are independent.

Legacy cone controls and validated ranges are listed in the example and in `VoxelGISettings`: resolution 64/128/256; 4 or 6 cones; 10–90 degree aperture; 8–512 maximum steps; indirect intensity 0–10. The cone default is 256 cubed, six 60-degree cones, 128 steps, and a 1.5-voxel normal bias.

## Delivered rendering path

Both voxelizers elect a deterministic triangle-instance owner, then resolve matching albedo, normal/metallic, and HDR emission. Material factors, UV0/UV1, vertex colors, alpha cutoff, and emissive strength are shared with the G-buffer. Textureless materials use neutral texture factors. Ownership avoids mixed attributes from competing triangle writes.

The GI renderer caches sky irradiance for the geometry/environment revision, injects direct diffuse light plus emission into RGBA16F radiance, and builds coverage and radiance mips with explicit storage-image views. The deferred GI variant traces those mips front to back, includes sky only for escaping visible directions, and retains direct PBR and specular environment IBL. All dependencies go through the existing RDG. Light-only edits rebuild radiance/mips and preserve voxel geometry.

## Verification environment

- Windows, x64 MSVC debug preset; Vulkan SDK 1.4.357.0.
- NVIDIA GeForce RTX 5080; graphics family 0, compute family 2, transfer family 1.
- Synchronization validation enabled with `VK_LAYER_VALIDATE_SYNC=1`; implicit overlays disabled with `VK_LOADER_LAYERS_DISABLE=~implicit~` and `DISABLE_RTSS_LAYER=1`.
- Sponza: 256 cubed, five configured lights with animated removal; 48 frames including all modes, repeated geometry updates, resize, minimize/restore, and returning to GI.
- Generated colored room: 64 cubed, six cones, 128 steps; point/directional/spot, zero light, emissive, sky-only, missing sky, and moving/removed light cases.

## Completed checks

The five targeted unit executables passed: `CommonTest` (30 tests), `InputControllerTest` (3), `ConfigLoaderTest` (12), `VulkanRHITest` (37), and `RenderCoreTest` (458): **540 tests**. Coverage includes light validation/capacity/IDs/snapshots, material emission defaults/strength, reflected vertex stride, descriptor access, lazy GI resources, owner reset/retry, and async ordering.

Run the native validation and image assertions with:

```powershell
python tools/validate_voxel_gi.py
```

The script generates a room fixture, saves PPM/PNG captures and logs, and restores the exact original config bytes. All **28 GPU cases** and image assertions passed. Every run exited successfully with zero application errors, Vulkan VUIDs, or synchronization hazards.

| Voxelizer | RHI | Async | Frames | Graphics / compute / transfer submissions | Result |
| --- | --- | --- | ---: | --- | --- |
| Geometry | Inline | Off | 48 | 98 / 0 / 6 | Pass |
| Geometry | Inline | On | 48 | 129 / 19 / 6 | Pass |
| Geometry | Threaded | Off | 48 | 98 / 0 / 6 | Pass |
| Geometry | Threaded | On | 48 | 129 / 19 / 6 | Pass |
| Compute | Inline | Off | 48 | 98 / 0 / 6 | Pass |
| Compute | Inline | On | 48 | 121 / 23 / 6 | Pass |
| Compute | Threaded | Off | 48 | 98 / 0 / 6 | Pass |
| Compute | Threaded | On | 48 | 121 / 23 / 6 | Pass |

These serial counts establish that opted-in work used the compute queue. They do not establish an async speedup.

Image assertions check both backends:

- Enabling indirect intensity increases illumination on the white receiver. The left white region receives more red; the right region receives more green. Measured mean RGB increases in 8-bit display values: geometry left `(4.752, 3.306, 3.219)`, right `(3.412, 4.285, 3.283)`; compute left `(4.746, 3.308, 3.222)`, right `(3.220, 4.075, 3.100)`.
- With analytic lights, emission, and environment lighting disabled, the tested interior region is exactly black.
- Emission alone and sky alone illuminate the white receiver. Point, directional, and spot inputs produce distinct output.
- Missing sky loads the black fallback and leaves the unlit interior black.
- Removing the animated light at frame 34 makes the tested interior black in that frame's output. Geometry revisions stay unchanged during light-only updates; light revisions stop changing after removal.

Evidence is under `build/voxel-gi-validation/`: `results.json`, `matrix-*.log`, `room-*.log`, and matching PPM/PNG captures. Summary logs are `build/voxel-gi-validation.log` and `build/voxel-gi-unit-final.log`. Captures were inspected directly; desktop automation was unavailable.

The focused native Vulkan binding, async-compute, and upload suites also passed (**45 tests**); see `build/voxel-gi-integration-targeted.log`.

## Skybox orientation correction

The initial GI change removed the source cubemap's required Y flip. `EnvironmentSourceDirection` now applies it consistently to the displayed sky, voxel sky-irradiance injection, and escaping cone samples. Deferred irradiance/specular lookups retain rotation only because `filtercube.vert` already applies the conversion during preprocessing.

Rebuilt the affected shaders and captured modes 2 and 3 with synchronization validation enabled. Both runs exited successfully without application errors, VUIDs, or synchronization hazards; the mode 3 capture was visually checked for upright environment orientation. Follow-up evidence: `build/skybox-orientation-build.log`, `build/skybox-orientation-mode2.log`, `build/skybox-orientation-mode3.log`, and the corresponding PNG captures.

## Sponza material correction

The roof's metallic-looking highlight came from material and reflection handling shared by modes 2 and 3. The main roof material (Sponza material 24) has near-zero metalness. Scene uploads forced its linear metallic/roughness texture to sRGB, turning a roughness texel of `104/255 = 0.408` into approximately `0.138`. The reflection shader also used `roughness * 4` despite preprocessing ten cubemap levels with roughness `mip / 9`.

Corrections:

- Scene uploads preserve the asset texture format, so base color/emission remain sRGB and metallic/roughness, normal, and occlusion data remain linear. Full scene-texture mip chains are generated to reduce distant texture aliasing.
- Reflection LOD uses the sampled cubemap's actual level count, with linear interpolation between prefiltered levels.
- The G-buffer applies tangent-space normal maps, including their UV set and scale. Imported tangent XYZ is preserved independently of its handedness sign. World-space tangent frames account for mirrored transforms; meshes without tangents use a derivative basis, falling back to the mesh normal for degenerate UVs. Normal scale occupies the previously unused third component of the renamed `surfaceProperties` vector, preserving the 96-byte material stride.

Validation: rebuilt `scene_renderer_demo`, `CommonTest`, and `RenderCoreTest`; all **31 Common tests and 458 RenderCore tests** passed. Regression coverage checks uploaded sRGB/linear formats and mip counts, plus imported normal scale, UV1 selection, mirrored tangent handedness, and material stride. All **28 existing GPU cases and image assertions** passed again, including both voxelizers, RHI thread/async combinations, light changes, and mode switching. The broad Vulkan suite limitation below was not retested by this correction.

Before/after Sponza captures use the same configuration and camera, with demo animation temporarily disabled and the original configuration restored afterward. Modes 2 and 3 both completed with synchronization validation enabled and no application errors, VUIDs, or synchronization hazards. Visual inspection confirms the broad mirror-like roof highlight is removed and tile normal detail is visible. Some surface gloss remains as specified by the source texture; no Sponza-specific material overrides were added.

Evidence: `build/sponza-material-build.log`, `build/sponza-material-common-tests.log`, `build/sponza-material-rendercore-tests.log`, `build/sponza-material-gpu-validation.log`, and `build/sponza-material-{before,after}-mode{2,3}.{log,ppm,png}`. GPU matrix evidence remains under `build/voxel-gi-validation/`.

## Implementation review corrections

The three confirmed review defects are fixed:

- The glTF loader preserves vertex color independently of `baseColorFactor`; the shared material shader applies that factor once. This restores both color and alpha-cutoff coverage.
- Vulkan image allocation and view creation propagate failure as null, release partial resources, and permit retry. GI initialization publishes normal/emission volumes together only after all other GI allocations and mip views succeed. Failed initialization preserves the voxel visualization resources and reaches the existing PBR fallback. Borrowed albedo mip views are reused across retries instead of accumulating duplicates.
- Unchanged settings and edits used only during cone tracing preserve cached volumes. Shadow changes invalidate radiance; cone count and normal bias invalidate sky irradiance and its dependent radiance. Geometry, light, and environment revision handling remains in place.

Rebuilt the shaders, demo, and affected tests. All **544 unit tests** passed: Common 31, InputController 3, ConfigLoader 12, VulkanRHI 37, and RenderCore 461. New tests inject failures at every GI texture allocation and mip-view step, verify cleanup and successful retry, and inspect executed RDG pass counts after settings, light, and geometry changes.

All **41 focused native Vulkan attachment/view tests** passed with synchronization validation enabled. Failure injection covers image creation, device-memory allocation, default views, and implicit attachment views for small and larger textures. Failed attempts leave no live images; a subsequent attempt succeeds. No Vulkan VUIDs, synchronization hazards, or resource-reference invariant errors were reported. Allocation-failure logs in this suite are expected.

The expanded `tools/validate_voxel_gi.py` passed all **40 GPU cases** and image assertions with zero application errors, VUIDs, or synchronization hazards. Its 12 new captures compare equivalent material-factor and vertex-color inputs, plus mask alpha 0.7 versus 1.0 at cutoff 0.5. Each pair is pixel-identical in mode 2 and in mode 3 with both voxelizers. The script restored the original configuration.

Evidence: `build/voxel-gi-fix-build.log`, `build/voxel-gi-fix-unit.log`, `build/voxel-gi-fix-native.log`, `build/voxel-gi-fix-gpu.log`, and the captures/results under `build/voxel-gi-validation/`. The complete integration suite was not rerun; its previously observed limitation below remains separate from these focused checks.

## Follow-up material and environment corrections

Vertex-color loading now uses fastgltf's accessor decoder, preserving all four RGBA components and supplying alpha 1 only for RGB colors. Normalized byte/short components, accessor offsets, and vertex-color byte strides are respected. The importer regression fixture covers 12 combinations of component count, component type, and packing, with different colors and alpha values on successive vertices.

`SetEnvironmentLighting` advances the GI environment revision only when intensity, rotation, or lighting enablement changes. Unchanged calls and background visibility toggles preserve the sky/radiance caches while still updating the background visibility uniform. The setter is grouped with the scene lighting implementation; RenderCore tests call that production method and verify actual RDG pass counts, uniform values, and invalid-edit preservation across 24 frames.

Rebuilt the affected targets. All **545 unit tests** passed (Common 32, InputController 3, ConfigLoader 12, VulkanRHI 37, RenderCore 461). The expanded image regression script passed all **49 GPU cases**, with no application errors, Vulkan VUIDs, or synchronization hazards. RGBA gray matches equivalent material-factor gray pixel for pixel; zero vertex alpha matches zero material alpha and leaves the masked test scene black. These comparisons cover mode 2 and mode 3 with both GI voxelizers. The original configuration was restored.

Evidence: `build/voxel-gi-refine2-build.log`, `build/voxel-gi-refine2-unit.log`, `build/voxel-gi-refine2-gpu.log`, and `build/voxel-gi-validation/results.json` with the corresponding captures. The broader suite and measurement limitations below were not changed by these corrections.

## Shadow segment and importer corrections

The five findings from `build/voxel-gi-review3/REVIEW.md` are corrected:

- Deferred shading and radiance injection share `VoxelLightVisibility`. Point/spot visibility now constructs its direction and finite distance from the biased origin to the actual light position. BRDF direction and attenuation remain evaluated at the original surface position; directional visibility still traces to the volume boundary.
- Vertex attributes and indices use fastgltf's accessor decoding, preserving byte strides, offsets, normalized components, and sparse values. The duplicated raw-pointer decoding paths were removed. Invalid triangle indices are rejected before normals or GPU buffers are built.
- The appended default material receives its actual array index and initialized GPU data, including metallic/roughness defaults and texture slots.
- Missing or invalid mesh normals generate flat triangle normals. Shared vertices are split so adjacent faces retain separate normals, and authored tangents are ignored when generating normals. Degenerate normals have a finite fallback; voxel resolve also rejects nonfinite normal lengths before normalization.
- Texture color space follows texture usage rather than a shared image-wide flag. When one texture serves both color and linear-data roles, the importer creates one linear copy and routes metallic/roughness, normal, and occlusion references to it. Base color and emission retain sRGB sampling. Default texture indices follow any added copies.

Rebuilt the affected targets and passed **549 unit tests**: Common 36, InputController 3, ConfigLoader 12, VulkanRHI 37, and RenderCore 461. The four new importer tests cover interleaved/normalized/sparse attributes and unsigned joints, separate flat normals at shared vertices and degenerate triangles, default material initialization with and without authored materials, and shared-image/shared-texture color-space routing. The existing texture test now checks the actual texture usage rather than requiring an unused texture to inherit another texture's sRGB role.

The expanded `tools/validate_voxel_gi.py` passed **86 GPU cases and all image assertions**, with zero application errors, Vulkan VUIDs, or synchronization hazards. Equivalent padded/packed geometry, absent/authored flat normals, omitted/explicit default materials, and shared/separate image or texture representations are pixel-identical in mode 2 and mode 3 with both voxelizers. Point and spot tests verify both sides of the finite shadow segment: a blocker beyond the light preserves the lit receiver, while a blocker before the light makes it black. The full thread/async/backend smoke matrix and prior lighting regressions also pass. The original configuration was restored.

The separate review controls were rerun after the fixes: transformed shared-mesh instances still match equivalent baked geometry pixel for pixel in all three mode/backend combinations. For the original point-light failure, the center-region RGB sum is now **254415** with shadows both enabled and disabled, compared with **0** for the erroneous shadowed result before the fix.

Evidence: `build/voxel-gi-fix5-final-build.log`, `build/voxel-gi-fix5-unit.log`, `build/voxel-gi-fix5-gpu.log`, `build/voxel-gi-validation/results.json` and matching captures, plus `build/voxel-gi-fix5/{results,controls}.json`. Point-shadow receiver captures were inspected directly. These results establish the five corrections; they do not replace raw-volume readbacks, performance measurements, or a clean full native integration run. The limitations below remain in effect.

## Workgroup and light-direction corrections (2026-09-22)

The initial correction used 4x4x4 workgroups (64 invocations) for active voxel volume passes, including owner clear, surface resolve, sky/injection, mip filtering, and visualization preparation. `Graphics/Shared/VoxelGI.h` supplied the same size to GLSL and CPU dispatch calculations. A compiled-SPIR-V regression checked all eight volume programs against the shared size and Vulkan core workgroup limits. The device-aware policy below supersedes this fixed size; no additional physical GPU was tested.

Scene light validation and normalization compute direction lengths in double precision before converting the normalized direction to GPU floats. Large finite directional and spot-light vectors therefore preserve their orientation. The new runtime regression covers add/update, components up to the maximum finite float, immutable snapshots, and rejected zero, near-zero, infinite, and NaN edits. Both new unit regressions failed before the production fixes and passed afterward.

Rebuilt the affected targets and passed **551 unit tests** (Common 36, InputController 3, ConfigLoader 13, VulkanRHI 38, RenderCore 461). The expanded GPU script passed **98 cases and all image assertions**, with zero application errors, Vulkan VUIDs, or synchronization hazards. Unit-length and `1e20`-scaled directions produce pixel-identical images for directional and spot lights in mode 2 and mode 3 with both voxelizers. All **86 existing captures** also remain pixel-identical to the pre-fix review captures. The original configuration was restored. Project formatting and `git diff --check` pass.

Evidence: `build/voxel-gi-fix6-build.log`, `build/voxel-gi-fix6-*Test.log`, `build/voxel-gi-fix6-{direction,workgroups}-before.log`, `build/voxel-gi-fix6-gpu.log`, and `build/voxel-gi-fix6-gpu/{results,baseline-comparison}.json` with matching captures. These checks address the two review findings; the broader native-suite failure and remaining measurements below are unchanged.

## Device-aware volume workgroups (2026-09-22)

The RHI now exposes the physical device's compute invocation and per-axis workgroup limits. RenderCore selects the first supported shape from **8x8x8, 8x8x4, 8x4x4, 4x4x4**, capped at 512 invocations. The shader manager specializes all eight active volume programs with that shape, and CPU dispatch counts use the same selection function with per-axis ceiling division, including small mip levels. Shader defaults remain 4x4x4. The compiled SPIR-V uses specialization of `WorkgroupSize`, validates against Vulkan 1.2, and does not require `LocalSizeId` or maintenance4. The RTX 5080 selects **8x8x8** with a reported invocation limit of 1024.

This is a capability-based default, not a measured performance optimum. Device limits determine legal sizes; choosing the fastest shape for filtering, sky tracing, injection, and other passes still requires separate GPU timings. No speedup is claimed, and larger supported workgroups are not assumed to be faster for every pass.

The build and **553 unit tests** pass (Common 36, InputController 3, ConfigLoader 15, VulkanRHI 38, RenderCore 461). Policy tests cover invocation boundaries, each axis, dispatch coverage, and small mip levels. Reflection checks specialization IDs and safe defaults for all eight production shaders. All **eight native pipeline integration tests** pass with synchronization validation, including a new production owner-clear test that exercises all four shapes on this device and reads back every voxel of a non-aligned 9x9x9 volume. This checks clear coverage and dispatch/specialization agreement; it does not validate voxelized triangle ownership or mip values.

All **98 GPU cases and image assertions** pass, with zero application errors, Vulkan VUIDs, or synchronization hazards. Every capture is pixel-identical to the fixed-4x4x4 baseline. The original configuration was restored. Project formatting, `git diff --check`, and Vulkan 1.2 SPIR-V validation of all eight volume shaders pass.

Evidence: `build/voxel-gi-adaptive-build.log`, `build/voxel-gi-adaptive-*Test.log`, `build/voxel-gi-adaptive-native.log`, `build/voxel-gi-adaptive-gpu.log`, and `build/voxel-gi-adaptive-gpu/{results,baseline-comparison}.json` with matching captures. The broader native-suite failure and remaining measurements below are unchanged.

## Visible light-source boxes (2026-09-22)

PBR and voxel GI modes can draw small, colored 3D boxes at enabled point and spot-light positions. The boxes use the same per-frame light snapshot as shading, so animated sources move with their markers and disabled/removed sources disappear. Directional and zero-intensity lights have no visible marker. The vertex shader generates an instanced unit box; no additional model asset or scene geometry is loaded. These visual markers do not enter voxelization or add a second emissive lighting source.

The deferred pass now preserves the sampled scene depth in the viewport depth attachment. The subsequent `LightMarkers` pass loads color and depth and depth-tests the boxes, so scene walls and nearer markers occlude them. `light_markers.enabled=true` and `light_markers.size=0.03` enable the boxes in the active Sponza configuration and example config; size is the side length in normalized world units. Markers default to disabled when the setting is absent. Settings are read at renderer initialization.

Rebuilt the demo and affected test targets. All **514 affected unit tests** pass (ConfigLoader 15, VulkanRHI 38, RenderCore 461). The expanded GPU script passes **128 cases**, including **30 new marker cases** for visible color, wall occlusion, point/spot sources, zero intensity, disabled lights, directional-light exclusion, animation, and removal. It exercises both voxelizers and both PBR/GI composition paths. The original **90 fixture captures** remain pixel-identical with markers disabled; the eight Sponza smoke cases use the updated interior-light configuration and enabled markers. Separate Sponza interior/overview previews were captured and inspected. All runs reported zero application errors, Vulkan VUIDs, or synchronization hazards, and the active configuration was restored. Project formatting and Vulkan 1.2 SPIR-V validation pass.

Evidence: `build/light-markers-{configure,build}.log`, `build/light-markers-*Test.log`, `build/light-markers-gpu.log`, `build/light-markers-gpu/{results,baseline-comparison}.json`, and the captures under `build/light-markers-gpu/` and `build/light-markers-preview/`.

Visibility follow-up: the original exterior starting camera completely occluded the four static markers. An interior capture did contain two white boxes, but they were only about 13 pixels tall and blended into bright stone. The Sponza preset now starts at `camera_position=0.55,-0.15,0`, uses 0.03-sized boxes, and renders dark, antialiased box edges. Light positions and intensity are unchanged. The final GI capture shows two boxes ahead near the far arch. The executable still defaults to voxel visualization (mode 1), which does not draw markers; select 2/3 or launch with `--mode=3`.

The visibility update passes all **38 VulkanRHI unit tests**, Vulkan 1.2 validation of both marker shaders, and the **128 GPU cases** again, with no application errors, Vulkan VUIDs, or synchronization hazards. A separate capture using the final active Sponza config was inspected. Evidence: `build/light-marker-visibility-build.log`, `build/light-marker-visibility-VulkanRHITest.log`, `build/light-marker-visibility-gpu.log`, and `build/light-marker-visibility-after/sponza-final.{png,log}`. Paired marker-on/off captures under `build/light-marker-visibility/` and `build/light-marker-visibility-after/` establish the occlusion and size observations.

## Box-shaped shadow investigation (2026-09-22)

The Sponza captures reproduce stair-stepped, box-shaped shadows with both geometry and compute voxelization. The artifact remains with `voxel_gi_indirect_intensity=0`; disabling analytic voxel shadows removes the corresponding dark regions. Reducing resolution from 256 to 128 makes the grid-scale outlines coarser. These controls disable animation and visual light markers to isolate the shadow behavior.

The direct cause was the initial shadow representation: `VoxelVisibility` in `Data/Shaders/VoxelGI/gi_common.glsl` traverses base-level grid cells and immediately returns zero visibility for any cell whose occupancy exceeds 0.5. Each occupied cell therefore casts the silhouette of an opaque cube, including the empty portion of a cell intersected by a triangle. It does not intersect the original mesh or filter shadow coverage. At the time of this investigation, both deferred direct lighting and radiance injection used this helper. This is the conservative base-level visibility baseline specified in the implementation plan; correct voxel coverage alone cannot remove its grid-shaped silhouettes. The geometry and compute producers have different boundary coverage, as noted below, but these captures do not establish a new voxelization defect. Light-marker boxes are drawn separately and never enter this occupancy volume.

Six diagnostic captures completed with zero application errors, Vulkan VUIDs, or synchronization hazards. The active configuration was restored byte for byte. Evidence: `build/voxel-shadow-review/{geom-current,comp-current,geom-no-shadow,geom-direct,comp-direct,geom-128}.{png,log}` and `build/voxel-shadow-review.py`. The `*-direct` controls disable the indirect-bounce intensity, but retain the environment terms; they are not isolated direct-light-only renders. No production shader or voxelizer change was made for this investigation.

Smooth mesh-shaped direct shadows require a separate geometry-based visibility path, such as shadow maps. Filtering voxel visibility can soften the grid transitions but cannot recover geometry lost to voxel resolution. Increasing volume resolution only reduces the cell size and substantially increases dense-volume memory. Workgroup shape does not change voxel resolution or shadow quality.

## Mesh-based direct shadows (2026-09-22)

The subsequent requested fix replaces mode 3's direct analytic-light occupancy test with mesh depth maps. Point lights use six perspective faces; spot lights use their outer cone; directional lights use an orthographic projection enclosing the scene bounds. Caster rendering reads the same transformed vertices, both UV sets, vertex alpha, material factors, and alpha-cutoff contract as the scene. Point/spot maps store linear radial depth to avoid perspective-depth precision loss near distant receivers. The shader uses a 4x4 tent PCF filter, with a geometric receiver-plane depth correction at each tap and a small scene-relative bias. This removes voxel-cell silhouettes without increasing voxel resolution. Mode 2 retains the documented unshadowed PBR baseline. Visible light boxes remain visual-only.

`shadow_map_resolution` selects the per-face size, defaults to 1024, and accepts 128 through 2048. `voxel_gi_shadow_enabled=false` or `light.N.casts_shadows=false` disables the corresponding direct shadow as well as the existing injection visibility. The voxel normal-bias setting affects GI traversal only; it no longer moves direct-shadow receivers by whole voxels. Injection and sky visibility still use voxel occupancy, and indirect lighting retains its documented coarse-grid limitations.

The new `SceneShadowRenderer` uses the current immutable light snapshot and declared render-graph depth/copy/sample dependencies. A shared 2D depth target is copied into individual layers of the sampled depth array because the existing graph attachment API selects whole textures. Static faces are retained; moving point lights update their six faces. Geometry changes, light-list/layout changes, and failed graph handoff invalidate the affected cache. The renderer falls back to PBR on allocation failure, and a later GI selection can retry. At 1024, the five-point-light Sponza configuration adds approximately 124 MiB of depth textures (30 sampled faces plus one shared target). This is a storage calculation, not a performance measurement; no GPU timing improvement is claimed.

All **502 affected unit tests** pass (463 RenderCore and 39 VulkanRHI). Regression coverage includes the actual packed scene vertex layout, failed allocation/retry, disabled lights, static-cache reuse, moving-light updates, color-only edits, geometry changes, light type/removal, and failed handoff. All **158 GPU cases and image assertions** pass, with zero application errors, Vulkan VUIDs, or synchronization hazards. The 30 new slanted-triangle controls exercise point, spot, and directional shadows with both voxelizers at 64/128/256, alpha-cutout casters, and per-light shadow disable. Direct-shadow captures are pixel-identical across voxel resolutions and backends. The existing indirect-bounce/color-transfer, emissive, sky, finite-light endpoint, moving-light/removal, marker, RHI-thread, async-queue, and mode-switch tests also pass.

The paired Sponza captures and final active-config view were inspected: the large voxel-shaped steps are gone, while the scene retains mesh-shaped shadows and visible light boxes. Shadow maps still have finite resolution, and PCF is antialiasing rather than a physical area-light simulation. Vulkan 1.2 SPIR-V validation of the two caster shaders and the GI deferred shader passes. The original active config was restored byte for byte. Evidence: `build/mesh-shadows-build4.log`, `build/mesh-shadows-final-{RenderCoreTest,VulkanRHITest,gpu}.log`, `build/mesh-shadows-final-gpu/results.json` and captures, plus `build/mesh-shadows-preview/sponza-final.{png,log}` and the six matching diagnostic captures in that directory. The original coarse-shadow captures remain under `build/voxel-shadow-review/` for comparison.

## Broader Vulkan suite limitation

The complete `VulkanRHIIntegrationTest` executable did not pass. It reached `TimelineAndFence/VulkanPresentationSubmissionTest.RejectedCopyAfterSuccessfulFramesRetainsAcquisition/0`, reported `VUID-vkCreateCommandPool-queueFamilyIndex-01937` (`VK_QUEUE_FAMILY_IGNORED`), raised an access violation, and timed out. The same presentation test passes when run alone; a sequential full-suite rerun reproduced the failure. This is a suite-order-dependent failure in an unchanged test path, not a diagnosed root cause. No presentation/RHI code was changed to mask it.

Evidence: `build/voxel-gi-final-tests.log`, `build/voxel-gi-integration-sequential.log`, and `build/voxel-gi-presentation-isolated.log`. The clean GI smoke results above are separate from this unresolved broad-suite failure.

## Limits and remaining measurements

This is one diffuse bounce in a dense, isotropic volume. Coarse resolution, sparse hemisphere directions, and mip averaging can blur illumination, produce bands, and leak around thin or overlapping surfaces. At the time of the original verification, geometry rasterization was not conservatively expanded and differed from compute coverage. The subsequent [V0 calibration](VoxelizationCalibration.md) corrects that path and records raw-volume evidence; its prerequisite acceptance gate passed for the documented coverage and material-sampling contract. A voxel stores one deterministic surface; features separated by less than a voxel can share an owner and lose the smaller feature. The emissive fixture keeps its panel several voxels below the ceiling for that reason.

The normal volume uses mesh normals; normal maps do not alter GI surface normals. Transparent materials use cutoff coverage, not translucent transport. Specular GI, additional bounces, moving/skinned geometry, clipmaps, and UI controls remain outside this implementation. Existing specular IBL is retained and is not voxel-occluded.

At 256 cubed, the dense GI/geometry textures account for approximately 603 MiB before G-buffer, scene, and visualization buffers. Use 128 or 64 on smaller GPUs. The default GPU was tested; unsupported-hardware fallback is covered by existing capability unit tests, not an additional physical device run.

The smoke loops measured roughly 0.73–0.81 seconds of render-loop wall time for 48 mixed-mode debug/validation frames, including first-use work and resize. This is not GPU frame timing or a steady-state performance comparison. No GPU pass timing, median/p95 benchmark, closed-room sky-leak metric, rotation oracle, or analytical readback of voxelized triangle ownership/mip values was completed. Framebuffer assertions, owner-clear readback, and synchronization validation do not replace those measurements.
