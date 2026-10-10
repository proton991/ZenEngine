# Hybrid Voxel GI Implementation Plan

Date: 2026-10-01. Updated 2026-10-02 with the user-confirmed reproduction. Revised 2026-10-07 against the current code and new Sponza measurements, and widened from environment lighting to the complete hybrid voxel GI. Status: P0–P3 core implementation is present; acceptance and remaining requirements are recorded in [HybridGI/README.md](HybridGI/README.md). The unchecked phase gates below are not claimed complete. The file name is kept because other documents link to it; earlier revisions were titled *Hardware Ray Query Environment Lighting Implementation Plan*.

**Goal:** a hybrid voxel GI in mode 3 (PBR + voxel GI).
- Rasterization draws the visible surfaces and builds the voxel scene.
- Ray queries answer visibility and hit questions: hardware triangle rays where the device supports them, voxel-occupancy rays otherwise.
- The voxel radiance volume is the radiance cache that ray hits read.

Every ray-traced feature has a non-RT path, and the renderer selects the best tier the device can run. The result must converge to independent references, look stable, and meet measured per-tier performance and memory targets.

This plan carries forward the hardware infrastructure requirements of [DynamicVoxelGIImplementationPlan.md](DynamicVoxelGIImplementationPlan.md) (Section 4.3 and H0–H2) for the current cone renderer. That older directional-irradiance pipeline was removed on 2026-09-30 and must not be assumed to exist.

## Definition of done

Evaluated on the frozen scenes, presets and limits of [P0](#p0-baseline-references-limits-and-targets):

1. **Diffuse GI.** Sky light and one bounce at opaque and alpha-tested receivers, in deferred and forward rendering, converge on the RT tier to independent CPU triangle references and the full-image [ground truth](#ground-truth). Fully enclosed receivers get no sky light, and no contribution is counted twice. On the compute tier they converge to the voxel reference, with documented voxel-geometry limits.
2. **Image stability.** At the shipping presets, there are no voxel-aligned steps. Settling after cuts is bounded, and camera, receiver, occluder and light motion produce no ghost trails or cross-surface leaks.
3. **Specular.** The scene occludes environment specular. Glossy surfaces above the roughness cutoff reflect scene light from the radiance cache. Behavior below the cutoff is documented.
4. **Radiance cache.** The voxel sky cache and light injection follow the same lighting contract from valid surface points.
5. **Fallbacks.** Every signal has a defined path on every tier. The renderer selects the tier from device capability and budget and reports the selection and reasons. Each tier is tested on its reference platforms, including RT disabled on RT-capable GPUs and macOS.
6. **Performance and memory.** Per-tier targets are met on the reference hardware with the frozen presets. A shortfall is documented with measurements and resolved by an explicit preset change, never by a hidden quality reduction.
7. **One implementation per signal.** The per-cone sky path survives only for translucent and scattering layers. Cone-traced bounce survives only where P8 measures it as the better compute-tier choice.

**Out of scope:**
- Mirror-sharp reflections that need full material shading at hit points.
- More than one diffuse bounce (listed as a later candidate).
- Colored transmission or volume scattering through translucent occluders.
- Ray-generation pipelines.
- Cascaded or clip-mapped voxel volumes for large worlds. One volume is fitted to the scene; on the RT tier voxels only supply radiance, so coarse cells cost less quality there than on the compute tier.

## Target architecture

Principles:
- Raster work produces the visible surfaces (G-buffer, or a receiver prepass in forward mode) and the voxel scene (voxelization, light injection, radiance mips).
- Ray queries return only visibility and hit location. Each query has a hardware and a voxel implementation behind the same shader interface.
- Each signal has one estimator and one reconstruction, shared by all tiers. Tiers differ in ray source, tracing resolution, sample counts and enabled features.
- No fallback may produce unoccluded sky light inside closed geometry.

| Signal | RT tier | Compute tier (no ray queries) | Minimum (voxel GI unavailable) |
| --- | --- | --- | --- |
| Visible surfaces | Raster G-buffer, or forward receiver prepass | Same | Same |
| Radiance cache | Voxelization, injection, radiance mips | Same | — |
| Diffuse sky | Hardware rays + reconstruction | Voxel rays + reconstruction | PBR irradiance map, unoccluded |
| Diffuse bounce | Same rays; hits read the radiance cache (P5) | Cone march, or voxel-ray hits if P8 measures them better | None |
| Specular (environment and glossy) | Lobe rays: misses use the environment, hits read the radiance cache (P6) | Same with voxel rays, or occlusion only if P8 measures reflections too costly | PBR prefiltered environment |
| Analytic shadows (pixels and injection) | Shadow maps; hardware shadow rays if P7 is adopted | Shadow maps | Shadow maps |
| Voxel sky cache | Hardware rays | Voxel rays | — |
| Translucent and scattering layers | Per-surface cone path | Same | PBR |

The minimum tier is the existing `RendererServer` fallback: PBR is rendered and the reason reported when voxel coverage is incomplete, GI initialization fails, or shadow memory preflight fails.

## Baseline before P0–P3 (2026-10-07)

This table describes the cone baseline at the start of P0. For the subsequent hybrid implementation, resource sizes, settings, measurements and outstanding gates, see [the P0–P3 implementation record](HybridGI/README.md) and [VoxelGIVerification.md](VoxelGIVerification.md).

| Area | Current behavior |
| --- | --- |
| Frame order | [RendererServer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RendererServer.cpp) builds the G-buffer, voxelization, mesh shadow maps, `VoxelGIRenderer` (sky cache, radiance injection, radiance mips), then composition. |
| Voxel volume | One cube fitted to the scene bounds at 64³, 128³ or 256³ (Sponza cells are 0.48 m at 64³). Any geometry or surface revision re-voxelizes the whole scene ([VoxelizerBase.cpp](../ZenCore/Source/Graphics/RenderCore/V2/VoxelizerBase.cpp)); this took 4.6 ms for Sponza at 64³ (Debug, RTX 5080). |
| Receiver diffuse | `DiffuseVoxelLighting` in [cone_trace.glsl](../Data/Shaders/VoxelGI/cone_trace.glsl) traces fixed cones (default six; weights 2/7 and 1/7; side cones 60° from the shading normal). Bounce composites voxel radiance front to back with transmittance. Escaped sky per cone is eight-ray occupancy visibility × the GGX-prefiltered environment at a cone-width LOD × intensity; transmittance no longer scales or gates it. The result is cosine-weighted mean radiance (irradiance/π), the convention of the PBR irradiance map ([irradiancecube.frag](../Data/Shaders/Environment/irradiancecube.frag)). |
| Visibility traversal | `VoxelEnvironmentVisibility` ([environment_visibility.glsl](../Data/Shaders/VoxelGI/environment_visibility.glsl)) and `VoxelVisibility` ([gi_common.glsl](../Data/Shaders/VoxelGI/gi_common.glsl)) step one base-level cell at a time with no empty-space skipping; occupancy above 0.5 blocks. Rays start at `position + shading normal × 1.5 voxels`, and the start cell itself is tested. |
| Voxel sky cache | [sky_irradiance.comp](../Data/Shaders/VoxelGI/sky_irradiance.comp) uses the same cones and eight-ray visibility from the voxel center plus normal bias, then multiplies by π and intensity. [inject_radiance.glsl](../Data/Shaders/VoxelGI/inject_radiance.glsl) applies albedo/π. Analytic injection evaluates at the owner-triangle surface point with mesh shadow maps; the sky cache does not use that point. The voxel radiance stores one isotropic value per cell. |
| Analytic shadows | `SceneShadowRenderer` mesh shadow maps: six faces per point light, one projection per spot or directional light; up to 32 lights. |
| Specular IBL | [deferred_lighting.glsl](../Data/Shaders/SceneRenderer/deferred_lighting.glsl) multiplies the prefiltered environment by the split-sum terms and material AO only; the scene does not occlude it. |
| Receiver paths | `DeferredLightingRenderer::UsesForwardMaterials` renders the whole scene forward, opaque surfaces included, when any material uses an extended glTF feature or specular-glossiness (`materialProperties.w`), any material is translucent, or any draw is not triangles. Forward mode declares no G-buffer; `forward_material` and `forward_scatter` voxel variants call `DiffuseVoxelLighting` per fragment. |
| G-buffer | Compact layout since [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) Phase 3: shading normal as RG16 UNORM octahedral, albedo RGBA8, metallic/roughness RGBA8, emissive/occlusion RGBA16F, and the device depth format (D32_SFLOAT on the RTX 5080), 24 bytes per pixel with D32. There is no position target: [gbuffer.glsl](../Data/Shaders/Common/gbuffer.glsl) reconstructs world position from depth with a double-precision inverse projection-view built relative to the near-plane center ([GBuffer.h](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/GBuffer.h)), and holds the shared normal encoding. There is no geometric normal, surface identity or motion. Deferred voxel shading derives a geometric normal from screen-space derivatives of the reconstructed position, which mixes neighboring surfaces at silhouettes. |
| Temporal data | `CameraUniformData` ([Camera.h](../ZenCore/Include/SceneGraph/Camera.h)) holds the current projection-view, projection and view only. No previous camera, node transforms or vertex positions are kept, and the renderer has no history resources. |
| Animation | Node animation, CPU skinning and morph weights ([SceneAnimation.cpp](../ZenCore/Source/SceneGraph/SceneAnimation.cpp)). `RenderScene` tracks geometry and surface revisions for static, dynamic and all instances. |
| Ray tracing | Buffer-device-address, acceleration-structure, ray-tracing-pipeline and ray-query features are detected and enabled ([VulkanExtension.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanExtension.cpp)); `--disable-rt` disables them. There is no acceleration-structure (AS) resource, build command or descriptor, and reflection rejects AS bindings ([RHIShaderUtil.h](../ZenCore/Include/Graphics/RHI/RHIShaderUtil.h)). Access and stage enums have 17 bits each ([RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h)). Ray-query availability on macOS (MoltenVK) has not been checked. |
| Diagnostics | Seven-component lighting capture ([LightingCapture.h](../ZenCore/Include/Graphics/Shared/LightingCapture.h)) through `scene_renderer_demo --capture-lighting`; `--capture-voxels`; per-pass GPU profiles through `--profile`; GPU tests in `ConeVoxelGIIntegrationTest`. The `tools/requirements-gi-quality.txt` environment includes Embree (`embreex`) for CPU references. |

## Problem and evidence

### Original report

The reported Sponza view uses camera position `(-0.053, 0.264, -0.010)`, looking down at the floor, with the AABB +Y light (Light 3) at zero intensity; see [Sponza_abnormal_shadow_debug.jpg](imgs/Sponza_abnormal_shadow_debug.jpg). Repeated dark strips appear on the floor. On 2026-10-02 the user confirmed that disabling **Environment lighting** removes them. Disabling it is a diagnostic, not a fix: it removes useful lighting with the artifact. The 2026-10-02 artifacts under `build/sponza-floor-debug/` no longer exist, and that reproduction recorded only the camera position, not the full matrices.

That investigation also measured a 16-ray-per-cone experiment: median `SceneLighting` rose from 17.326 ms to 215.221 ms at 1920×1080 on an RX 7900 XT.

### Analysis of 2026-10-07

Conditions: environment light only, 64³, 960×540, Debug build, RTX 5080. The reference is the same voxel visibility with 64 rays per cone. Scripts, debug shaders, comparison images and capture metadata are in `build/sponza-environment-analysis-20261007/` (ignored, non-portable; raw HDR files were not kept but can be regenerated with its `probe.py`).

| ID | Defect | Evidence | Status |
| --- | --- | --- | --- |
| D1 | **Angular quantization.** Every pixel traces the same fixed directions and gets binary answers. In Sponza's narrow atrium only 0–3 of the upward cone's eight rays escape, so one ray changes floor brightness by up to about 40%, with voxel-square edges. | Up-cone visibility on the atrium floor takes only the values 0, 1/8, 2/8 and 3/8. With 16 rays per cone the floor is still blocky; 32 is nearly smooth. | Open: P2, P3 |
| D2 | **Voxel geometry error.** Sponza's flagpoles are 0.17 m thick at 6.6–7.1 m height but occupy 0.48 m cells, casting bands. Ray starts land inside column-base and curb cells for 26% of atrium-floor pixels in a top-down view, and those return zero visibility. | A CPU query of the glTF triangles confirmed the poles. The black squares remain with 64 rays. Moving the start out of occupied cells removed them but added bright seams at curtain bases. | Open: RT tier (P4); documented compute-tier limit |
| D3 | **Occlusion counted twice.** Escaped sky was transmittance × visibility; both estimate the same blockers. On open roofs, where at least 97% of rays escape, transmittance was still 0.82. | Before: 18% (top-down) and 60% (hall) darker than the reference; open roofs at 73% of PBR diffuse IBL. After: within 1.3% and 10%; roofs at 86%. | Fixed 2026-10-07 |
| D4 | **Unoccluded specular sky.** Covered areas reflect the full environment. | Code inspection. | Open: P3, P6 |
| D5 | **Sky cache sampling.** Originally eight directions from voxel centers. The hybrid cache now uses 48 environment + 16 cosine directions at owner surfaces; ownerless occupied voxels still use their centers. Diagnostic captures count this fallback: 0 / 22,997 evaluated occupied voxels in 64³ Sponza (2026-10-10), both providers. | Native captures and [execution results](HybridGI/P4Execution.md). | Sampling and measurement implemented; ownerless fallback policy remains |
| D6 | **Cone bounce leaks.** Mip-averaged opacity lets cones see light through thin walls; the cone-bounce limitation is recorded in [VoxelGIVerification.md](VoxelGIVerification.md). | Existing verification record. | Open: P5 on the RT tier; compute-tier limit unless P8 adopts ray hits |

Cost reference: in a static scene only the G-buffer (0.09 ms) and the lighting pass run each frame; voxelization, injection, the sky cache and mips run only on changes. Going from 6 to 48 voxel rays per pixel raised the lighting pass from 0.229 ms to 0.814 ms (960×540, 64³, Debug, RTX 5080), roughly 0.014 ms per additional ray per pixel there. The RX 7900 XT experiment implies a much higher per-ray cost at 1080p. Voxel-ray cost grows with grid resolution.

## Phase overview

D1 requires distributed per-pixel sampling with temporal and spatial reconstruction, whatever traces the rays; one hardware ray in place of each fixed voxel ray would keep the steps. D2 and D6 require triangle visibility. Shared estimators and reconstruction are therefore built first with the existing voxel traversal, which also delivers the compute-tier fallback. Hardware ray queries follow behind the same interface, then the remaining hybrid signals, then tier tuning.

| Phase | Delivers | Depends on |
| --- | --- | --- |
| P0 | Frozen baseline, CPU references, fixtures, limits, performance targets, platform matrix | — |
| P1 | Receiver data, previous-frame data, persistent history, forward receiver prepass | P0 |
| P2 | Sky estimator with the voxel provider; sky-cache update; settings | P1 |
| P3 | Temporal accumulation, spatial filtering, specular occlusion | P2 |
| P4 | Hardware ray-query provider: RHI, render graph, scene acceleration structures | P0; P4a–P4c may run in parallel with P1–P3; P4e after P3 |
| P5 | Diffuse bounce from ray hits | P3; P4 for the RT tier |
| P6 | Glossy reflections from the radiance cache | P3, P5 |
| P7 | Hardware shadow rays (optional; adopted only if P8 measures a benefit). Shadow rays for bright sources extracted from the environment are pulled into P4 (2026-10-10) | P4 |
| P8 | Compute-tier and RT-tier performance, presets, memory, platform validation, automatic selection | P5, P6; P7 if implemented |

Each phase leaves the renderer usable, with focused tests and a short verification record in [VoxelGIVerification.md](VoxelGIVerification.md).

## Lighting contract

### Diffuse signals

For a receiver at `x` with shading normal `n_s` and geometric normal `n_g`, and `V(x, w) = 1` when the ray in direction `w` escapes:

`D_sky(x) = (1 / pi) * integral[L_env(w) * V(x, w) * max(dot(n_s, w), 0) dw]`

`D_bounce(x) = (1 / pi) * integral[L_hit(x, w) * (1 - V(x, w)) * max(dot(n_s, w), 0) dw]`

- Units match the PBR irradiance map and the current `DiffuseVoxelLighting` output: a constant environment `L` with nothing blocking gives `D_sky = L`.
- `L_env(w)` is level 0 of the prefiltered environment map, looked up with `EnvironmentDirection(w)` and multiplied by environment intensity and enablement. Level 0 is generated at roughness 0, and the prefiltered maps share their orientation with specular IBL. `EnvironmentSourceDirection` applies only to the raw skybox texture.
- `L_hit` is the radiance-cache value at the first hit (see [Hit radiance](#hit-radiance)), multiplied by indirect intensity.
- Directions with `dot(n_g, w) <= 0` lie below the actual surface and count as blocked with zero radiance.
- A ray escapes only when no accepted opaque hit occurs before it leaves the scene bounds (triangle provider) or the voxel volume (voxel provider). Alpha-mask candidates that fail the material's cutoff are rejected and traversal continues. Blend and transmissive surfaces count as opaque occluders; this is a documented limitation. Both faces of every surface occlude, whatever its raster sidedness.
- Sky sampling combines environment-luminance samples (three quarters) with cosine-weighted hemisphere samples around `n_s` (one quarter). Sample counts that are multiples of four split deterministically; one and two samples choose each proposal with those probabilities. A miss adds `L_env * max(dot(n_s, w), 0) / (pi * p_mixture(w))`, where `p_mixture = 0.75 * p_env + 0.25 * p_cos`; the explicit mixture PDF preserves the scale. A black environment falls back to cosine sampling. The same estimator is used by the diagnostic reference mode. See the [hotel-room noise investigation](HybridGI/P4.md#hotel-room-noise-2026-10-08), which introduced equal proportions, and the [hall reconstruction follow-up](HybridGI/P4.md#hall-full-image-reconstruction-2026-10-09), which measured the three-to-one split.
- P5 hit radiance must likewise use its actual sample PDF when adding `L_hit` to the bounce channel. Hit-radiance sampling is still deferred; the current cone bounce is unchanged.
- Also accumulate `nu`, the cosine-weighted unblocked fraction, for diagnostics and filter guidance.
- Sky and bounce are reconstructed as separate channels: bounce must respond to light changes that leave sky history valid.

Do not multiply either signal by cone transmittance or by a second visibility term. Until P5 is adopted on a tier, `D_bounce` is the existing cone bounce `B_cone` (transmittance compositing and indirect intensity, unchanged).

### Composition

`diffuse outgoing = kD * albedo * AO * (D_sky + D_bounce)`

Metallic/Fresnel, albedo and material AO are applied exactly once. Material AO represents detail absent from the geometry and stays, as in PBR mode. A receiver without a valid reconstructed value, such as a translucent layer or an unsupported topology, keeps the per-cone path, and capture metadata reports which path each class of receiver used.

### Specular

`specular = (S * prefiltered(R, roughness) + (1 - S) * H) * (F * A + B) * AO`

- `S` is the fraction of the specular lobe `f * cos` whose rays escape, over the upper hemisphere of the shading normal: the domain of the prefiltered map and the split-sum terms. Lobe directions below the shading normal are outside that integral and are excluded, not counted as blocked; otherwise an unoccluded rough surface gets `S < 1` (0.5 at roughness 1 and normal incidence). Directions above the shading normal but below `n_g` are blocked. Visible-normal samples carry the weight `G1(l)` (separable Smith, Fresnel excluded).
- `H` is the mean radiance-cache value of the lobe rays that hit, with indirect intensity applied; it is zero until P6.
- Keeping the environment part on the prefiltered map leaves only the hit part noisy.
- In forward materials with extra lobes (clearcoat, sheen), apply the base-lobe `S` and `H` to every environment-specular lobe and document the approximation.
- Below the roughness cutoff fixed in P0 (initially 0.2), reflected scene detail is limited by voxel resolution; it is still occluded correctly. Analytic specular is unchanged.

### Hit radiance

Shared by P5 and P6:
- **Hit point and normal.** The hardware provider uses the triangle hit and its geometric normal. The voxel provider uses the entry point into the occupied cell and the face it entered through.
- **Lookup.** Read the base-level radiance of the hit cell. Each cell stores one isotropic value for its owner surface. If the cell's stored normal faces away from the ray (`dot(n_voxel, -w) <= 0`), it represents the far side of a thin surface or the receiver's own surface. In that case use the adjacent cell on the ray's side when it is occupied and facing; otherwise return zero, and count rejected lookups in diagnostics. Trilinear lookups must divide by coverage so empty neighbors do not darken the result.
- **Content.** The cache contains injected analytic light, injected sky light and emission, so each path delivers exactly one bounce of each.
- **Thin walls.** Must pass the thin-wall fixture: a wall lit on one side contributes nothing to receivers on its unlit side.

### Ray starts and ranges

- Ray starts use the depth-reconstructed position from `gbuffer.glsl`. Its error grows with the square of view distance for perspective cameras; `CompactGBufferPreservesNormalsPositionsAndPlanarDerivatives` bounds it at 1×10⁻⁴ + 1×10⁻⁴ × distance² renderer units with a 0.001 near plane. Start offsets must exceed that error at the receiver's distance and near plane, and are tested at near, middle and far distances.
- The triangle provider offsets along `n_g` with a floating-point-aware offset, such as the method of Wächter and Binder (Ray Tracing Gems, chapter 6). It must not ignore whole instances to avoid self-hits, because that removes legitimate self-occlusion.
- The voxel provider offsets along `n_g` instead of the shading normal, keeping the current bias in voxels. The D2 start-inside-occupancy defect remains a documented compute-tier limit unless a start policy passes all leak fixtures in P0, including a receiver at the base of a one-voxel wall.
- The triangle provider traces to the scene bounds, including geometry outside the voxel volume. The voxel provider traces to the volume boundary.

### Sampling sequence

Each pixel follows the R2 sequence, rotated by an Owen-scrambled Sobol point in Morton pixel order, so every aligned 2^k × 2^k pixel block holds a stratified set of rotations and the spatial filter averages blue-noise rather than white-noise error. Each proposal and dimension uses its own scramble. Advance the sequence once per successfully executed frame. Captures use a fixed seed and frame index. Diffuse samples per pixel are 1, 2 or 4; specular uses one lobe sample per pixel initially. A reference mode accumulates many frames with reprojection and filtering disabled; it is the converged estimate used by acceptance tests.

### Voxel sky cache

- Evaluate each occupied voxel at the owner-triangle surface point with that triangle's geometric normal, using the reconstruction analytic injection already uses. Define a representative point for the averaged-reflectance policy. Fall back to the voxel center only when no owner exists, and count those voxels.
- Use a deterministic stratified sample set with the active provider: 48 environment-luminance directions and 16 cosine directions, the per-pixel proposal shares, with the same mixture PDF as per-pixel sky sampling. Never use camera-space history for voxels.
- Updates may complete progressively over several frames. Consumers keep using the last complete generation; a partial generation is never published.
- Units are unchanged: irradiance is `pi * mean(L_env * V * max(dot(n, w), 0) / (pi * p_mixture(w))) * intensity`, and injection multiplies by albedo/π.

## P0 Baseline, references, limits and targets

1. **Cameras.** Freeze full camera matrices, not only positions, for:
   - the original report view;
   - a top-down atrium view (camera at (0, 16, 0) in glTF space looking down, screen-up = −X, 60° vertical FOV, as in the archived `probe.py`);
   - the hall view from the 2026-10-07 analysis;
   - a fixture view for each fixture below.

   Store them as glTF camera nodes or capture metadata.

   **Receiver precision amendment, 2026-10-10.** G3 arbitration implicated receiver positions: Vulkan and Embree agree from identical origins, while depth reconstruction changes sharp distant shadows. The rationale was recorded before the FP32 experiment in [P4 gaps](HybridGI/P4Gaps.md#execution-notes-2026-10-10). Hardware receivers now use an RGBA32F raster-position target, adding 16 bytes/pixel to the hybrid G-buffer (64 bytes total). The voxel tier retains its depth reconstruction. Ray count, 32-frame history, five edge-stopping iterations, camera matrices and error limits are unchanged. Seven additional coarse residual passes address filter bias; their memory and GPU cost still need P8 qualification. This amendment does not mark P4 accepted.
2. **Configurations.** Environment light only; analytic lights only; both. Bounce intensity 0 and 1. Voxel resolutions 64, 128 and 256. Viewports of 960×540 and 1920×1080. Record scene and shader hashes, device and driver.
   - **Environments** (added 2026-10-10; see the [decisions of 2026-10-10](HybridGI/P4Gaps.md#decisions-2026-10-10)). The ground truth (rung 3) covers nine: `papermill.ktx` and the eight Poly Haven panoramas in `Data/Textures/Environments`. The shipping-preset limits gate five of them, one per lighting type: Papermill (soft interior daylight), kloppenheim 06 (soft sky), qwantani noon (hard sun), hotel room (interior with small lamps) and carpentry shop 01 (artificial lights with daylight). Studio small 09, small empty room 1, large corridor and kloofendal 48d are tracked and reported but do not gate: they repeat those types or, like the studio's softboxes, are extreme cases kept as stress tests. A wrong converged answer is a defect under any environment, so correctness is checked on all nine; the shipping limits are a noise budget, so they apply to a representative set. File hashes are in `baseline.json`.
3. **CPU references** in `tools/environment_reference.py`, using the Embree binding from `tools/requirements-gi-quality.txt`:
   - **Sky.** Load the glTF scene with the renderer's normalization and take receiver positions and normals from a capture. Trace at least 4096 cosine samples per receiver against all triangles with material alpha cutoffs, and sample the same environment cubemap with the same rotation and orientation. Output `D_sky` and `nu`.
   - **One bounce.** For uniform-albedo fixtures, compute one-bounce diffuse light at the same receivers from analytic lights and sky, with triangle-accurate visibility at both the receiver and the hit point.
   - **Specular.** Sample the GGX lobe and output `S` and the hit fraction.
   - **Full-image sky** (added 2026-10-09) in `tools/ground_truth_mitsuba.py`, using Mitsuba 3 from the same requirements file: `D_sky` and `nu` at every pixel center, with independent primary visibility. See [Ground truth](#ground-truth).

   Validate each reference against analytic fixtures before using it.
4. **Fixtures**, for the GPU tests and the CPU references:
   - open plane under a constant environment (`D_sky = L`);
   - closed box with one-voxel walls (no sky light inside);
   - receiver at the base of an infinitely tall wall (`D_sky = L/2` for a constant environment);
   - narrow slot;
   - thin pole above a plane;
   - alpha-masked blocker;
   - mirrored and two-sided meshes;
   - a closed box of single-sided, outward-facing walls (back faces must occlude; added 2026-10-08);
   - an occluder outside the voxel volume;
   - a uniform-albedo room lit by one point light (bounce);
   - a one-voxel wall lit on one side only (thin-wall leak);
   - a glossy floor beside a lit colored wall (reflections);
   - a moving occluder and a moving light over a static floor.
5. **Capture extensions.** Add raw and reconstructed sky, bounce, `nu`, `S` and `H` as debug outputs. Also add history length, rejection reason, hit-or-miss masks, rejected-lookup counts, receiver position and geometric normal. None of these may be permanent production allocations. Keep the seven existing components compatible and add a format version to the metadata.
6. **Platform matrix.** Windows RTX 5080 and RX 7900 XT, each with RT enabled and with `--disable-rt`; macOS on Apple Silicon through MoltenVK (record ray-query availability); one GPU without ray queries if available. Untested platforms are reported as unverified. Per the [decisions of 2026-10-10](HybridGI/P4Gaps.md#decisions-2026-10-10), P4 can close with the RX 7900 XT and a GPU without ray queries reported as unverified; MoltenVK remains an open P4a item that is recorded but does not block P4.
7. **Frozen limits.** These initial limits are frozen by this revision. Changing one requires a written rationale recorded before any candidate result is evaluated.

| Check | Limit |
| --- | --- |
| Analytic fixtures, converged raw estimate | Within 0.5% of the analytic value; closed box exactly zero sky |
| Closed box after reconstruction | Sky ≤ 10⁻⁶ × environment mean |
| RT tier, converged, versus CPU sky reference over frozen regions | Absolute mean bias ≤ 1%; RMS error ≤ 3% of the region mean |
| Shipping preset after 64 static frames, versus the converged reference of its tier | Absolute mean bias ≤ 2%; RMS error ≤ 8%; 99th-percentile absolute error ≤ 20% of the region mean; no exact zeros where the reference is ≥ 10% of the region mean |
| Compute tier, converged, versus its 1024-sample voxel reference | Same as the RT-tier converged limits; versus the CPU reference, report only |
| One-bounce fixture at 128³, RT tier, versus the CPU bounce reference | Absolute mean bias ≤ 10%; the difference is reported per cause (cache resolution, isotropic cells) |
| Thin-wall fixture | Bounce on the unlit side ≤ 1% of the lit side's bounce |
| Settling after a camera cut or history reset | Within the shipping-preset limits within 32 frames |
| Moving occluder over a static floor | No frame shows the old shadow position; within the shipping-preset limits within 32 frames after it stops |
| Light moved or switched | Bounce channel within the shipping-preset limits within 8 frames; sky channel unaffected |
| Glossy floor fixture, RT tier, converged, versus CPU specular reference | `S` and hit fraction within 2% absolute |

8. **Performance targets** (engineering targets proposed by this revision; confirm before the P0 freeze). All GI passes combined, including reflections and reconstruction, at 1920×1080, Release build, frozen presets:

| Tier and preset | Reference hardware | Static scene | One moving light and one moving object |
| --- | --- | --- | --- |
| RT tier, High | RTX 5080, RX 7900 XT | ≤ 3.0 ms | ≤ 5.0 ms |
| Compute tier, Medium | Same GPUs with `--disable-rt` | ≤ 4.0 ms | ≤ 6.0 ms |
| Compute tier, Low | Apple Silicon (MoltenVK) or the slowest available GPU | Measured and reported | Measured and reported |

The 16.7 ms total mode-3 frame time at 1080p remains the overall target. GI memory (voxel volumes, acceleration structures, scratch, histories and retired generations) is measured per preset in P8 and gets a default budget there.

Exit: references, fixtures, cameras, limits, targets and the platform matrix are archived and reproducible, and each CPU reference passes its analytic fixtures.

## P1 Receiver data and history infrastructure

### Receiver data

- Add a receiver target to the G-buffer pass with the geometric normal and a stable surface identity (node index plus a facing bit). Encode the geometric normal with the existing `EncodeGBufferNormal`. Compute it from `dFdx`/`dFdy` of the interpolated world position in the G-buffer pass, where the 2×2 quad lies on one triangle, and orient it like the shading normal. Judge degeneracy relative to the derivative lengths, never with an absolute threshold: until 2026-10-09 an absolute test rejected nearly every pixel of the unit-normalized scene and silently stored the shading normal instead (the geometric normal matched the true triangle normal on 40% of Sponza hall pixels; 99.96% after the fix). Replace the composition pass's derivative reconstruction with it, which also fixes the silhouette error noted above.
- Review the receiver and motion targets together with the compact layout (24 bytes per pixel today), as [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) Phase 3 requires, and record the new size and G-buffer pass time against the Phase 3 measurements.
- Add motion as the NDC difference between the interpolated previous and current clip positions, plus the previous w (2026-10-08: storing only the previous clip position exceeded the 0.01-pixel limit near the camera on an AMD iGPU, because dividing one interpolated position amplifies interpolation rounding). Keep previous world matrices per node and the previous projection-view. For CPU-skinned or morphed meshes, keep previous deformed positions, or mark those pixels as deforming and reset their history.
- Publish previous camera, node and deformation state only after a frame executes successfully.

### Forward receiver prepass

Forward mode currently has no G-buffer. Add a receiver prepass for opaque and alpha-mask forward draws that writes depth, shading normal, roughness, the receiver target and motion. Forward opaque fragments use the reconstructed signals only when their identity and depth match the prepass pixel. Otherwise they keep the per-surface path, and coverage metadata counts them. Translucent and scattering layers keep the per-surface path.

### History resources

- Keep per-view persistent textures for: sky, bounce and `nu`; `S` and `H`; luminance moments per channel; history length; and previous depth, normals and identity. Size them to the view, recreate them on resize, and give every additional GI view its own set.
- Import history textures into the render graph each frame and order them against readers and writers of previous frames in flight. A two-texture ping-pong alone is insufficient.
- A failed or rejected frame publishes neither history nor previous-frame state.

| Change | Required response |
| --- | --- |
| Camera cut, projection change, resize, scene replacement | Reset all receiver histories; recreate extent-dependent resources |
| Environment texture, rotation, intensity, enablement | Reset sky and specular histories; rebuild the voxel sky cache; bounce follows the radiance-cache update |
| Analytic light change (lighting revision), emissive change | Keep sky history; bounce and `H` use the responsive policy of P3 |
| Geometry, topology, transform, deformation, opacity acceptance | Update acceleration structures first; rebuild affected caches; reset histories globally. While anything animates, the result is spatially filtered only and noisier; P8 may add local invalidation |
| Provider, sample count, tracing resolution, bias, ray-range policy | Reset incompatible history and provider data |
| Albedo-only edit with unchanged opacity and geometry | Recompose material factors; keep unmaterialed history; refresh affected voxel radiance |
| Failed or rejected frame | Publish no history, previous-frame state, acceleration structure or cache generation |

Exit: render-graph tests cover history ordering across frames in flight, resize and failed frames. Captured geometric normals match mesh face normals. Identity is stable across frames. Under camera motion in a static scene, motion reprojects to within 0.01 pixel.

## P2 Sky estimator with the voxel provider

- Add a GI compute pass (provisional name `HybridGIRenderer`) after `VoxelGIRenderer` and before composition. It reads receiver data, the voxel occupancy volume and the prefiltered environment, and writes raw sky and `nu`.
- Define one shader-side provider interface returning visibility, and on request the hit point and normal, for an origin, direction and maximum distance. The voxel provider wraps the existing traversal.
- Deferred and forward opaque receivers read the sky signal from this pass. Their per-cone sky branch is disabled; receivers on the per-surface path keep it. Lighting-capture component 5 reports the sky term actually used.
- The voxel sky cache adopts the owner-surface start, geometric normal and stratified samples defined above, using the same provider.
- Add settings through the existing `VoxelGISettings` load, validate and live-apply path, and its runtime UI (names provisional):

| Setting | Behavior |
| --- | --- |
| `voxel_gi_quality=low\|medium\|high\|ultra` | Preset from P8: voxel resolution, tracing resolution, samples, reflections, bounce source, provider preference |
| `voxel_gi_ray_provider=auto\|voxel\|hardware` | Ray source for every query in this plan; `auto` follows the tier selection |
| `voxel_gi_samples=1\|2\|4` | Diffuse samples per pixel |
| `voxel_gi_bounce_source=cone\|rays` | Diagnostic override of the preset's bounce source |
| `voxel_gi_reflections`, `voxel_gi_specular_occlusion` | Enable `H` and `S` |
| `voxel_gi_temporal`, `voxel_gi_filter` | Disable reprojection or spatial filtering, for tests and reference captures |
| `voxel_gi_history_frames` | Upper bound on accumulated frames (default 32) |
| `voxel_gi_rt_shadows` | P7, only if adopted |
| `voxel_gi_memory_budget_mb` | Pre-allocation cap covering voxel volumes, acceleration structures, scratch, histories and retired generations |

During the transition, a `legacy` value of `voxel_gi_ray_provider` selects the current per-cone sky path for comparison. It is removed when P3 passes.

Exit:
- The analytic fixtures pass the converged raw limits.
- The compute tier's converged Sponza result passes its own-reference limits, and the closed box stays zero.
- Zero-environment, zero-indirect, emission-only and analytic-only cases show no duplicated energy.
- With `legacy`, images match the pre-change baselines.

## P3 Reconstruction and specular occlusion

### Temporal accumulation

- Reproject each pixel with its motion and a 2×2 bilinear history footprint. A tap is valid only if all of these hold:
  - it lies inside the view;
  - its identity matches;
  - its previous position lies within a depth-relative plane distance of the current geometric plane;
  - its normals agree (dot product above 0.9).

  Below a total valid weight of 0.01, treat the pixel as disoccluded with history length zero.
- Accumulate with `alpha = max(1 / historyLength, 1 / historyLimit)`, including first and second luminance moments per channel.
- **Sky channel:** no neighborhood clamping in steady state, because clamping biases the estimate. Resets follow the P1 table.
- **Responsive policy** (bounce channel and `H`): a lower history limit, and clamping to the current neighborhood's variance box for a bounded number of frames after a lighting, emissive or radiance-cache generation change.
- **`S` and `H`:** shorten history at low roughness and reject it when the view direction changes by more than a roughness-dependent angle.

### Spatial filtering

Apply an edge-avoiding à-trous filter (SVGF style, five iterations of a 5×5 kernel) to the unmaterialed diffuse channels and `nu`, and separately to `S` and `H`. Edge-stopping weights use:
- plane distance along `n_g`;
- shading-normal similarity;
- identity match (hard);
- luminance difference scaled by the filtered variance.

When history length is below 4, estimate variance spatially. Thin walls and foreground/background boundaries must not exchange values.

The sky is filtered as a ratio to the receiver's unoccluded environment irradiance, evaluated for its shading normal from an order-2 spherical-harmonic projection of the sampled environment, and the result is remodulated by the center's value. Luminance differences and variances are compared in that ratio. This keeps normal-map shading detail that normal weights alone would blur, while neighbors with different normals still average their visibility. Pixels sharing a normal are filtered exactly as without the guide. The projection is built with the environment sampling distribution; an aliased Riemann-sum irradiance cube was rejected as the guide because small, very bright sources alias in it (see [P4](HybridGI/P4.md#hall-full-image-reconstruction-2026-10-09)).

### Specular occlusion

Trace one visible-normal GGX sample per pixel with the active provider for `S`, reconstruct it, and apply it as defined in the lighting contract with `H = 0`.

Exit:
- Sponza floor regions from all frozen cameras pass the shipping-preset limits on the compute tier against its own reference.
- The voxel-aligned steps are gone; the remaining difference from the CPU reference is due to voxel geometry (D2).
- Camera motion, camera cuts, resize, a moving occluder, moving and deforming receivers, alpha edits and environment rotation pass their settling limits with no ghost trails.
- The closed box stays dark after filtering, and glossy surfaces inside it show no environment specular.

## P4 Hardware ray-query provider

Implementation status, 2026-10-09: the hardware path is implemented and tested on RTX 5080 and an AMD Radeon iGPU; see [P4 implementation and verification](HybridGI/P4.md). The checklist remains open where the full acceptance matrix has not passed. The hall's full-image shipping reconstruction now passes bias and RMS but still misses the 99th-percentile and exact-zero limits at four samples per pixel (21.65% against 20%, ten zeros); eight samples per pixel pass. Against the full-image [ground truth](#ground-truth), the converged tier reference passes on three of four frozen views; the hotel-room hall misses the per-pixel limit on distant lamp-lit receivers. A nine-environment sweep on 2026-10-10 widens both findings: the shipping preset fails in every hall view, and the tier reference misses the ground truth per pixel in four of nine hall views, all with small bright sources. Gaps and fix steps are in [P4 gaps](HybridGI/P4Gaps.md), together with the decisions recorded on 2026-10-10: the environment sets, the ground-truth noise rule, unverified platforms, bright-source handling, deferred preset changes and texture LOD for alpha.

### P4a Capabilities and shader variants

- Expose enabled buffer device address, AS, ray query, build-queue capability, geometry formats and relevant limits through generic RHI capabilities. Audit the extension dependency chain for the configured Vulkan version; do not infer usability from names or support flags alone. Check macOS through MoltenVK explicitly.
- Ray-query compute shaders compile and pass `spirv-val --target-env vulkan1.2` at the existing SPIR-V 1.3 target (checked 2026-10-07 with Vulkan SDK 1.4.357), so no global target change is needed. Build them as separate variants and validate each. Keep RT-free variants; shader discovery must not instantiate query modules on unsupported devices.
- `--disable-rt` stays a hard device-feature override; runtime selection cannot enable a feature omitted at device creation.

Exit: supported, disabled and unsupported devices resolve correctly, and voxel-tier rendering starts without loading query shaders or allocating AS resources when RT is unavailable.

### P4b Resources, commands and descriptors

- Add a stable-identity RHI AS resource with build-size queries, storage allocation, device addresses and explicit creation and build status. Expose vertex, index, instance and scratch buffer requirements.
- Record build and update commands through the current command stream. Copy descriptions, geometry arrays and ranges into command-owned storage so threaded execution never retains caller stack memory.
- Add AS reflection, ordinary descriptor binding, descriptor cache keys, pool sizing and validation. Keep AS descriptors separate from the material texture heap.
- Retain the TLAS, referenced BLASes, storage, geometry and material lookup tables, and other query inputs until completion. An AS descriptor retaining its pool does not retain those resources.
- Validate build and update eligibility, formats, counts, instance fields, device-address ranges and offsets, and scratch alignment before recording. Use fresh resources for incompatible replacements.

Builds, updates and allocation sizes follow the [Vulkan acceleration-structure specification](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html). Compaction is a P8 option; serialization and host builds are out of scope.

### P4c Render-graph synchronization and retirement

- Represent AS build writes, update-source reads, query reads and indirect BLAS references in the render graph and submission history. Track build inputs and scratch in the same graph. Adding flags to the 17-bit access and stage enums requires auditing every fixed-size array, expansion loop, queue check and metric that depends on them.
- Required chains:
  - upload or deformation, then BLAS build;
  - BLAS build, then TLAS build;
  - TLAS build, then query;
  - previous readers, then an update;
  - previous scratch users, then scratch reuse.

  Queries read with the shader stage that uses them and AS-read access; builds use the AS-build stage. Follow the [Vulkan ray-tracing synchronization guidance](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html#synchronization-for-ray-tracing).
- Start builds and queries on the graphics queue. Preserve cross-graph producer provenance and retain resources until every relevant queue serial completes.
- Failed or rejected work publishes no valid AS generation, history or reusable scratch. Native allocation-failure recovery remains out of scope; pre-allocation budget rejection and existing failure contracts remain required.

Exit: graph-driven triangle hit, miss, distance, barycentrics, and instance, primitive and material identity are correct with no validation errors. Cover inline and threaded execution, empty scenes, shared BLAS instances, transforms, masked candidates, scratch reuse, replacement and injected submission failures, in mock and native RHIs.

### P4d Scene query service

- Build one BLAS per mesh from the same vertex and index data and indexing used by rasterization, shared across instances. Provide a full-scene TLAS; build static-only and dynamic-only views only when a consumer or test needs them.
- Track instance, geometry, topology, surface-opacity and material generations separately:
  - rigid motion updates instance data;
  - CPU-skinned and morphed meshes update or rebuild their BLAS from the deformed positions of the raster snapshot;
  - topology changes rebuild.
- Handedness, alpha acceptance and material identity must match rasterization. Sidedness does not: raster back-face culling is view-dependent, and visibility queries treat both faces of single-sided surfaces as occluders, as the voxel provider and the CPU reference do. Otherwise a closed single-sided shell admits sky light (measured 2026-10-08: 61% of the environment inside a closed box). Closest hits report their raster facing for hit-radiance consumers.
- Alpha-mask candidates reconstruct UVs, apply texture transforms and cutoff with an explicit texture LOD (compute shaders have no derivatives), and continue traversal when rejected. Never mark masked geometry opaque. The contract evaluates alpha at texture level zero; rasterization tests it at its own mip level, and the pixels where the two see different surfaces are reported as primary mismatches rather than matched by LOD ([decisions of 2026-10-10](HybridGI/P4Gaps.md#decisions-2026-10-10); revisited only if those pixels break a limit).
- Expose any-hit visibility, and closest-hit queries with versioned hit metadata (instance, primitive, barycentrics, material, geometric normal) for hit radiance and later consumers.

Exit: moving and deforming geometry, opacity edits, instance removal, scene replacement and geometry outside the voxel volume all affect visibility, without stale addresses or mixed generations.

### P4e Provider integration

- Implement the hardware provider behind the P2 interface for the sky, specular occlusion and the voxel sky cache, with the start offsets and ranges defined above.
- Fallback order:
  - `hardware` when ray queries are enabled and the scene's acceleration structures are ready;
  - otherwise `voxel`;
  - the minimum tier when voxel GI itself cannot run.

  A fallback is never reported as hardware RT.
- Explicit `hardware` requests either activate a complete, ready provider or report the reason and the actual fallback. Diagnostics separate the requested provider, enabled device features, ready scene resources, selected provider, receiver coverage and reset reason.
- Never reduce voxel resolution, sample count or unrelated quality settings to fit a budget.
- Switching providers in either direction resets incompatible histories. Retired provider resources are freed after their last readers complete.

Exit:
- The RT tier's converged sky passes the CPU-reference limits.
- The shipping preset passes the shipping-preset limits on every frozen camera.
- D2 is gone: no zero squares at column bases, and the flagpole shadows match the reference.
- Provider switching, unsupported capabilities, `--disable-rt`, budget rejection and frames in flight behave as specified.

## P5 Diffuse bounce from ray hits

- Use the diffuse rays' hits for `D_bounce` through the hit-radiance contract, with both providers. The hardware provider uses closest-hit queries; the voxel provider returns the first occupied cell and entered face.
- Reconstruct bounce as its own channel with the responsive temporal policy.
- Compare against the cone bounce (`voxel_gi_bounce_source=cone`) on every fixture and frozen camera, for quality (bounce fixture, thin-wall fixture, D6 leaks, noise after 64 frames) and cost.
- On the RT tier, ray-hit bounce replaces cone bounce when it passes the limits. On the compute tier, P8 chooses between cone and ray-hit bounce by measurement; the losing path is removed for that tier.

Exit: the one-bounce, thin-wall and light-change limits pass on the RT tier, and both bounce sources are measured on the compute tier.

## P6 Glossy reflections from the radiance cache

- Use the specular lobe rays' hits for `H` through the hit-radiance contract, and compose specular as defined in the lighting contract.
- For low roughness, reproject `H` with hit distance (reflection parallax), not only surface motion. Keep history short below the roughness cutoff.
- Mirror-sharp reflections and full material shading at hits remain out of scope; below the cutoff, reflections show voxel-resolution detail.
- On the compute tier, P8 decides between reflections and occlusion only (`H = 0`) by measurement.

Exit: the glossy-floor fixture passes its limit on the RT tier. Enclosed glossy surfaces reflect scene light instead of sky. Open glossy surfaces keep their PBR specular within the frozen limits. Reconstructed reflections show no ghost trails under camera motion.

## P7 Hardware shadow rays (optional)

- Trace one shadow ray per shadowed light per pixel at receivers, and one per lit voxel per light during injection from the owner-surface point, against the full-scene TLAS with the same alpha rules. Hard shadows first; soft shadows from light radius need their own reconstruction and are a later option.
- Shadow maps remain the fallback and the compute-tier path.
- P8 measures both paths with 1, 4, 16 and 32 lights. P7 is adopted per tier and preset only if it matches or beats shadow-map cost there, or improves quality at an accepted cost.

Exit: hardware shadows match a CPU shadow reference on the fixtures, and the adoption decision is recorded.

Per the [decisions of 2026-10-10](HybridGI/P4Gaps.md#decisions-2026-10-10), shadow rays for the compact bright sources extracted from the environment (sun, lamps, softboxes) are prototyped in P4 as the first fix for the shipping variance in high-contrast views ([G1](HybridGI/P4Gaps.md#g1-shipping-variance-in-high-contrast-views)). If adopted, the lighting contract splits `D_sky` into the extracted sources and the residual environment, and that change is written into the contract before results are evaluated. Analytic-light shadow rays remain optional as above.

## P8 Tiers, performance, memory and automatic selection

### Measurement

- Extend the existing profiling and capture tools. Report:
  - voxelization, injection, sky-cache and mip updates;
  - cold AS construction, rigid updates and deformation updates or rebuilds;
  - sky, bounce and specular tracing, temporal, filter and composition passes;
  - complete GPU frame and CPU submission;
  - peak committed device memory, including scratch and retired generations.

  Report the rays actually issued, not only the configured counts.
- Measure every tier and preset on the P0 platform matrix with the same scenes, camera matrices, viewport, voxel resolution and material settings, using median and tail times, initialization and moving-scene costs. Record AMD and NVIDIA results separately.

### Compute-tier performance

- **Empty-space skipping.** Hierarchical traversal over an occupancy max-mip pyramid. It must return the same first occupied cell as the base traversal for a frozen ray set.
- **Reduced-resolution tracing.** Half-resolution tracing with guided upsampling, using the same guides as the filter and a tested fallback at unresolved edges.
- **Bounce and reflections.** Choose between cone and ray-hit bounce, and between reflections and occlusion only, by measurement.
- **Partial voxel updates.** Keep static voxels and re-voxelize only the regions of dynamic instances. Today any geometry revision re-voxelizes the whole scene.
- **Amortization.** Spread sky-cache and injection updates over frames, never publishing partial generations.

### RT-tier performance

BLAS update-versus-rebuild policy, compaction, scratch pooling, and async-compute placement of builds and GI passes, each validated against the render-graph tests.

### Presets and selection

- Define `low`, `medium`, `high` and `ultra` presets mapping voxel resolution, tracing resolution, samples, reflections, bounce source, provider preference and P7 adoption. Record each preset's measured time and memory on the reference hardware.
- Automatic selection picks the tier from capability (`hardware` only for measured, validated capability and preset combinations) and the default preset from the measured table for the device class. It reports both, with reasons. Runtime dynamic-resolution tuning is a later candidate.
- The editor exposes the preset and provider, and shows the selected tier, fallback reason and GI GPU time in its diagnostics. Raw, reconstructed, history-length, hit-or-miss and rejection views are added to the existing debug output selection.
- Every optimization is revalidated against the P0 limits. Do not raise bias, remove occluders or silently drop features to meet a time target.

Exit: reproducible performance and memory reports per tier, preset and platform; post-optimization correctness results; the P0 targets met or the shortfall documented with the preset decision; and an explicit automatic-selection decision. Missing hardware evidence cannot be marked passed.

## Ground truth

A tier's converged reference is computed by that tier's own estimator, so it can only show that reconstruction converges; it cannot show that the converged answer is right. A defect shared by the estimator and its reference passes every shipping-limit comparison. This happened on 2026-10-08: single-sided back faces let sky into a closed shell, and both the shipping output and the hardware reference agreed; the analytic fixtures and the CPU reference caught it. Shipping-limit results are therefore meaningful only while the tier reference itself matches an independent ground truth.

Two different claims need two kinds of evidence:
- **Verification**: the renderer computes the lighting contract. Checked exactly, against references that implement the same definitions independently.
- **Validation**: the contract is close enough to physical light transport. Checked against a general path tracer and reported per approximation; it never has to match exactly.

| Rung | Reference | Shows | Blind spots |
| --- | --- | --- | --- |
| 1. Analytic fixtures | Closed forms: open plane `D_sky = L`, closed boxes `0`, wall base `L/2`, cube-quadrature of unoccluded bright and rotated environments, open and enclosed `S` | Exact values; environment scale and rotation | Simple geometry |
| 2. CPU triangle reference | `tools/environment_reference.py` (Embree): sky, `S` and one bounce on the real triangles and alpha masks | Ray visibility on production scenes | Sparse receivers taken from the engine's G-buffer; cosine sampling, too noisy for small bright sources |
| 3. Full-image ground truth | `tools/ground_truth_mitsuba.py` (Mitsuba 3; CUDA backend where available, CPU LLVM backend otherwise): `D_sky` at every pixel center | Primary visibility, receiver reconstruction and sky over the whole frame | Normal maps (compared with normal textures stripped); per-pixel noise of indoor receivers that see sky through small openings |
| 4. Tier reference | The tier's reference mode (1024 samples × 64 frames, no reconstruction) | Reconstruction error: the shipping-preset limits | Every defect it shares with the shipping estimator |

Rules:
- Every reference passes the rung-1 fixtures before it is used, and the fixture report is archived with its results.
- References are cross-checked against each other where they overlap, because fixtures do not cover every failure. The rung-3 tool recomputes its visibility with the rung-2 tracer at sampled receivers and reports the difference against its own sampling noise. On 2026-10-09 this exposed a rung-3 defect that the fixtures passed: Mitsuba's default ray-start offset (about 1e-4 of the position) closed centimeter gaps under Sponza's gallery floor and made the engine appear to leak sky. When two references disagree, a third decides; neither is assumed right.
- The rung-3 tool shares only the scene file, the capture's camera matrix and the captured level-zero environment cube with the engine. glTF parsing, primary visibility (raster culling and alpha rules), the BVH, environment importance sampling and the estimator are independent. Both faces of every surface occlude and masked texels are transparent, as in the contract. Radiance is evaluated from the captured cube itself with the engine's bilinear lookup; a resampled copy only guides sampling. Small, very bright sources make this matter: evaluating a 2048×1024 resampled map widened the hotel room's 0.36° lamps by 5% and doubled the per-pixel floor discrepancy.
- Like-for-like comparisons use captures made with `capture_hybrid_gi.py --strip-normal-maps`, so both sides shade with vertex normals. Materials without a normal map must then shade with the vertex normal exactly; until 2026-10-10 the engine's 8-bit default normal texture tilted them 0.32°, which the open-plane fixtures now check. Normal-mapped comparisons are a separate validation measurement.
- Pixels whose primary hit differs from the engine's receiver by more than four times the receiver's depth-quantization error are excluded and counted; a large count is itself a primary-visibility finding. A fixed distance tolerance is not used: it either rejects every pixel of a scene with a coarse depth buffer or accepts a neighboring leaf of alpha-masked foliage as the same surface.
- Each tier reference must match rung 3 within the RT-tier converged limits of P0 (absolute mean bias ≤ 1%, RMS ≤ 3%) on every frozen camera and environment. Indoor receivers that see the sky through small openings stay noisy even at tens of thousands of samples per pixel, so report the ground truth's own noise and judge RMS with that noise removed (`excess_rms`), per pixel and on 8×8 pixel blocks. A per-pixel or 8×8 excess above the limit fails, since the excess is already corrected for the ground truth's noise. Where the per-pixel excess is within the limit but the ground truth's per-pixel noise exceeds it, the per-pixel pass cannot be confirmed; the 8×8-block result then decides and the per-pixel result is reported ([decisions of 2026-10-10](HybridGI/P4Gaps.md#decisions-2026-10-10): blocks still expose systematic error and misplaced shadows at low noise, while resolving the studio hall per pixel would take about 14× the samples). Pixels under diagnosis get targeted high-sample renders instead.
- Rung-3 renders are stored once per frozen view and environment and never replaced, together with what they depend on (scene and environment file hashes, camera matrix, captured environment cube) and their fixture-gate report. Engine captures are regenerated for each build by `tools/ground_truth_sweep.py`, which refuses a capture that no longer matches its render; a changed input needs a new render.
- Run rungs 1–3 before tuning reconstruction against a tier reference, after any change to the estimator, visibility, environment handling or receiver reconstruction, and for every acceptance claim.
- The rung-3 scope grows with the plan: sky now; one bounce with uniform albedo for P5 (a validation comparison of the radiance-cache approximation, with differences reported per cause); and the composed final image with real materials as a report-only validation of the whole model (split-sum specular, specular occlusion, single bounce).

## Validation and acceptance

Tolerances, regions, seeds and comparison inputs are those frozen in P0. Store raw linear HDR data as well as display images.

| Test group | Required evidence |
| --- | --- |
| CPU references | Analytic fixtures pass before each reference is used; reference inputs and outputs archived |
| Ground truth | [Ground truth](#ground-truth) rungs 1–3 for every frozen camera and environment; the tier reference within the RT-tier converged limits of rung 3, with the ground truth's noise reported |
| Triangle-query oracle | Hit, miss, distance, barycentrics, normal and identity match an independent CPU reference; rejected mask candidates continue to later blockers |
| Voxel traversal oracle | Base and hierarchical traversal return identical first occupied cells and entered faces on a frozen ray set |
| Open and enclosed geometry | Constant-environment normalization; no sky in enclosed receivers before or after filtering; no filter leakage through thin walls |
| Sparse and oblique blockers | Thin pole, slanted wall, narrow slot, mirrored and two-sided meshes, alpha-cutout geometry, occluders outside the voxel volume |
| Bounce and hit radiance | One-bounce fixture, thin-wall fixture, emissive surfaces, rejected-lookup counts; cone versus ray-hit comparison on both tiers |
| Reflections | Glossy-floor fixture; enclosed glossy receivers reflect no sky; open glossy receivers keep PBR specular within limits |
| Sponza floors | Every frozen camera. The environment on/off/on sequence of the original report. Sky, bounce and specular separated. Steps removed while legitimate occlusion remains. Disappearance with environment disabled does not pass. |
| Estimator isolation | Converged raw, low-sample raw, temporal-only and temporal-plus-filter outputs; bias and noise quantified separately per channel |
| Composition | No duplicated energy; zero-environment, zero-indirect, emission-only and analytic-only cases; direct, emission and analytic specular unchanged where their inputs are unchanged |
| Animation and history | Camera motion and cuts, moving occluder, moving and switched lights, moving and deforming receivers, alpha edits, disocclusion, environment rotation, resize, scene replacement, failed frames |
| RHI and render graph | Inline and threaded execution, graph boundaries, scratch reuse, frames in flight, queue equivalence, failed submission, retirement, zero validation and synchronization errors |
| Tiers and selection | Every platform-matrix entry; disabled and unsupported features; explicit requests; budget rejection; empty scene; provider switching both ways; reported tier, preset, fallback and coverage |
| Performance and memory | P0 targets per tier and preset on the reference hardware, with the measurement conditions recorded |
| Compatibility | Existing voxelization, reflectance and cone-lighting tests pass; translucent and scattering layers keep their per-surface path |

Use the existing `scene_renderer_demo`, `ConfigLoaderTest`, `CommonTest`, `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest` and `ConeVoxelGIIntegrationTest` targets, and add focused native estimator, reconstruction, hit-radiance and ray-query tests following their conventions. Compile and validate every shader variant, and run with Vulkan synchronization validation. Report skips and unrelated pre-existing failures separately from passes.

Capture metadata must include:
- format version;
- tier, preset and ray provider;
- bounce source and receiver coverage;
- frame, geometry, lighting and environment generations;
- sample seed and count;
- history validity and length;
- filter settings and reset reason.

## Later candidates

These are not required for done and are each assessed against the P0 limits and targets:
- environment and emitter importance sampling;
- a second bounce by gathering into the radiance cache over frames;
- localized history invalidation;
- runtime dynamic resolution;
- soft hardware shadows;
- full material shading at reflection hits;
- cascaded voxel volumes for large worlds.

## Relationship to earlier plans

| Earlier requirement | Disposition |
| --- | --- |
| Old H0 capabilities, acceleration structures, descriptors, commands, synchronization, lifetime | P4a–P4c, in full |
| Old H1 scene views, transforms, deformation, masks, out-of-grid geometry | P4d; the full-scene view serves every query |
| Old H1 triangle hit decoding | P4d versioned hit metadata, used by hit radiance |
| Old H1 directional-GI sender packing and gather | Not carried forward; the radiance cache and ray-hit gather replace it |
| Old H1 provider switching and histories | P1, P2 and P4e |
| Old H2 profiling, optimization, memory, automatic promotion | P8 |
| [VoxelGIImplementationPlan.md](VoxelGIImplementationPlan.md) §6.2 escaped sky and specular IBL | Escaped sky replaced by `D_sky` for covered receivers; the "separately bounded" specular occlusion is delivered as `S`, extended with `H` |

## Delivery checklist

- [ ] P0 cameras, configurations, CPU references, fixtures, limits, performance targets and platform matrix archived.
- [ ] [Ground truth](#ground-truth): each tier reference matches the independent full-image ground truth on every frozen camera and environment.
- [ ] P1 receiver data, motion, previous-frame state, history resources and forward receiver prepass pass.
- [ ] P2 voxel-provider sky estimator, composition, sky-cache update and settings pass.
- [ ] P3 temporal accumulation, spatial filtering and specular occlusion pass on all frozen cameras.
- [ ] P4a capabilities and shader variants pass.
- [ ] P4b acceleration-structure resources, commands, reflection and descriptors pass.
- [ ] P4c render-graph dependencies, lifetime, retirement and failure tests pass.
- [ ] P4d scene query service passes.
- [ ] P4e hardware provider passes the CPU-reference and shipping-preset limits.
- [ ] P5 ray-hit bounce passes on the RT tier; compute-tier bounce source measured.
- [ ] P6 glossy reflections pass on the RT tier; compute-tier choice measured.
- [ ] P7 hardware shadows evaluated and the adoption decision recorded (optional).
- [ ] P8 performance and memory reports per tier, preset and platform; presets and automatic selection documented.
- [ ] Functional report records correctness evidence and limitations per tier.

This document retains the acceptance gates. P0–P3 implementation and partial verification do not imply that their unchecked exit criteria have passed; see [the implementation record](HybridGI/README.md#remaining-acceptance-work).
