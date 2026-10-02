# Hardware Ray Query Environment Lighting Implementation Plan

Date: 2026-10-01. Updated: 2026-10-02 with the user-confirmed environment-lighting reproduction. Status: accepted design direction; implementation and acceptance checks remain open.

Implement hardware triangle visibility for diffuse environment lighting, using distributed direction samples, temporal accumulation, and edge-aware filtering. Keep voxel cone tracing for bounced diffuse lighting. Complete the RHI, VulkanRHI, and render-graph support needed to build, bind, query, update, and retire acceleration structures safely.

This is the current implementation plan for that work. It carries forward the hardware infrastructure and validation requirements from [DynamicVoxelGIImplementationPlan.md](DynamicVoxelGIImplementationPlan.md), especially Section 4.3 and H0–H2, while targeting the current cone renderer. The older plan describes a directional irradiance pipeline that must not be assumed to exist in the current checkout. Completing this plan does not by itself complete or restore that older algorithm.

## Problem and evidence

The reported Sponza reproduction uses camera position `(-0.053, 0.264, -0.010)`, looking down toward the floor, with the AABB +Y light, Light 3, set to zero. The marked source image is [Sponza_abnormal_shadow_debug.png](imgs/Sponza_abnormal_shadow_debug.png).

The investigation reproduced the stripes with all analytic lights disabled. Linear lighting captures isolated them to escaped environment diffuse lighting. In [cone_trace.glsl](../Data/Shaders/VoxelGI/cone_trace.glsl), each broad cone currently receives a single binary result from [environment_visibility.glsl](../Data/Shaders/VoxelGI/environment_visibility.glsl). An occluder on that ray can suppress the environment contribution of the entire cone. This produces excessive directional contrast even when the occluder is beside the floor rather than directly above it. Voxel geometry approximation adds a separate source of visibility error.

A distributed 16-ray-per-cone experiment removed the large stripes, but the measured median `SceneLighting` GPU time rose from 17.326 ms to 215.221 ms at 1920×1080 on the RX 7900 XT. These are lighting-pass measurements for that experiment, not total frame times or predictions of hardware RT performance. Removing visibility altogether also removed the lines but admitted environment light into enclosed areas. Both experiments were reverted; no production fix remains from the investigation.

Local diagnostic artifacts are under `build/sponza-floor-debug/` and are ignored, non-portable evidence. Before implementation, regenerate and archive a reproducible baseline with full camera matrices, configuration, scene/shader hashes, device/driver, HDR components, and fixed floor regions. Camera position alone is insufficient to reproduce the exact view.

### Bug record and UI reproduction

Status: diagnosed and unresolved. On 2026-10-02, the user confirmed that disabling **Environment lighting** in the UI makes the abnormal floor shadows/black strips disappear.

1. Load Sponza in Voxel GI mode with environment lighting enabled.
2. Set the camera position to `(-0.053, 0.264, -0.010)` and look down at the floor areas marked in the screenshot.
3. Set Light 3 at AABB +Y to intensity zero. Observe the repeated dark strips despite no apparent object directly above those floor locations.
4. Disable **Environment lighting** in the UI. The user observed that the strips disappear.
5. For regression capture, re-enable environment lighting without changing the camera or other settings and verify that the baseline artifact returns. Also repeat with all analytic lights disabled to isolate the environment contribution.

Expected behavior: environment lighting should remain occluded by scene geometry, with the diffuse response accounting for the visible range of incoming directions. A narrow blocker should not remove the light assigned to an entire broad cone. Disabling environment lighting is a diagnostic workaround, not a correction: it removes useful lighting along with the artifact.

### How environment light enters voxel GI

Environment light is directional radiance from the HDR cubemap. It is independent of Light 3 and the other analytic lights. The current renderer consumes it through three paths:

| Path | Implementation | Effect of disabling environment lighting |
| --- | --- | --- |
| Environment to visible diffuse surface | `TraceDiffuseCone` in [cone_trace.glsl](../Data/Shaders/VoxelGI/cone_trace.glsl) adds environment radiance when a cone escapes the volume and its visibility ray is clear | Removes the escaped environment diffuse term responsible for the observed strips |
| Environment to voxel surface to visible surface | [sky_irradiance.comp](../Data/Shaders/VoxelGI/sky_irradiance.comp) estimates incident environment light; [inject_radiance.glsl](../Data/Shaders/VoxelGI/inject_radiance.glsl) converts it to reflected voxel radiance, later gathered by cone tracing | Removes the environment-sourced portion of bounced diffuse lighting |
| Environment to specular reflection | [deferred_lighting.glsl](../Data/Shaders/SceneRenderer/deferred_lighting.glsl) samples the prefiltered environment independently of diffuse cone tracing | Removes specular IBL |

The UI experiment establishes environment dependence but, by itself, does not distinguish these three paths. The earlier linear component captures identify escaped environment diffuse as the source of the strong floor stripes. Analytic direct lighting and non-environment bounce sources remain independent of this switch; skybox visibility has its own control.

### Why the strips appear

For each escaping cone, the current shader effectively adds:

`cone weight * remaining transmittance * binary voxel visibility * environment radiance * environment intensity`.

`VoxelEnvironmentVisibility` traverses base-level voxel occupancy along one ray. Encountering a cell with opacity above `0.5` returns zero, regardless of how much of the cone's angular footprint that cell actually blocks. With the default six cones, [HemisphereCone in gi_common.glsl](../Data/Shaders/VoxelGI/gi_common.glsl) gives the normal-aligned cone weight `2/7` and each of the five tilted cones weight `1/7`. These are integration weights, not fixed fractions of the final pixel brightness; environment radiance and transmittance vary by direction.

A hit therefore removes a substantial directional contribution abruptly. A neighboring floor pixel may miss the same occupied cells and retain that contribution. Sparse fixed directions create coherent strips, while voxel boundaries and shading-normal variation can make their edges jagged. Sampling one unfiltered environment direction per cone also poorly represents an HDR environment with strong directional variation.

The tilted rays can encounter geometry beside the receiver. No object directly overhead is required for environment occlusion. The bug is the excessive darkness and abrupt shape caused by using one binary ray to represent a broad cone, compounded by voxel geometry approximation. The strips represent missing incoming light, not a separate black layer or necessarily a Light 3 shadow-map error. Specific blocker triangles have not been identified by the component captures alone.

The planned fix must address both angular sampling and geometric visibility. Distributed samples, temporal accumulation, and edge-aware filtering reconstruct the environment contribution; hardware triangle queries improve occluder accuracy. Replacing each existing voxel ray with one hardware ray while retaining the same all-or-nothing cone gate would preserve the sampling defect.

## Scope and completion boundary

The delivery includes:

- Usable hardware capabilities, acceleration-structure resources, GPU builds and updates, shader reflection and bindings, synchronization, and deferred lifetime management.
- A reusable triangle-query scene with static, dynamic, and full-scene views; the environment consumer uses full-scene visibility.
- Distributed environment sampling at visible diffuse receivers, reprojection, history rejection, and spatial reconstruction.
- Environment visibility at occupied voxel surfaces for radiance injection, while retaining cone tracing to gather bounced radiance.
- Runtime selection, resource limits, capability fallback, diagnostics, automated correctness tests, and final performance assessment.

Use `VK_KHR_ray_query` in compute shaders for the main receiver pass. Ray queries may also serve the explicit forward-surface fallback described below. A ray-generation pipeline, shader binding table, `TraceRays` commands, recursive path tracing, RT reflections, and replacement of analytic-light shadow maps are outside this delivery. Query support must not depend on enabling `VK_KHR_ray_tracing_pipeline`; the APIs are separate paths with shared acceleration structures. See the [Vulkan ray-tracing guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html) and [ray-query sample](https://docs.vulkan.org/samples/latest/samples/extensions/ray_queries/README.html).

The compatibility cone path remains available with RT disabled. Hardware RT is not a guarantee of speed or a substitute for adequate angular sampling. A single hardware ray controlling an entire cone would retain the sampling defect.

## Current code and ownership

| Area | Observed state | Planned responsibility |
| --- | --- | --- |
| [VulkanExtension.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanExtension.cpp) | Device-address, acceleration-structure, and ray-query feature plumbing exists | Distinguish support, requested enablement, successful device enablement, and renderer readiness |
| [RHIShaderUtil.h](../ZenCore/Include/Graphics/RHI/RHIShaderUtil.h) | Acceleration-structure reflection is explicitly unsupported | Add AS resource type, reflection, binding validation, and backend descriptors |
| [RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h) and [RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h) | No complete public AS build/query contract; access/stage enums have no AS operations | Add backend-neutral resources, limits, build descriptions, and accesses |
| [RenderSubmissionHistory.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderSubmissionHistory.cpp) and RDG | Existing transactional resource/submission tracking | Extend it to AS dependencies and all referenced resources |
| [VoxelGIRenderer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp) | Owns voxel radiance, sky irradiance, revision tracking, and lighting bindings | Keep bounce transport; integrate the selected environment provider |
| [offscreen_surface.glsl](../Data/Shaders/SceneRenderer/offscreen_surface.glsl) | Writes position, shading normal, materials, and depth-related receiver data; no temporal receiver identity or velocity output here | Supply stable receiver identity, geometric normal, and previous position/motion for reconstruction |
| [deferred_lighting.glsl](../Data/Shaders/SceneRenderer/deferred_lighting.glsl) and [forward_material.glsl](../Data/Shaders/SceneRenderer/forward_material.glsl) | Both consume cone diffuse lighting | Replace only the environment term for eligible receivers, preserving material composition |
| [LightingCapture.h](../ZenCore/Include/Graphics/Shared/LightingCapture.h) | Seven components include escaped environment and bounced diffuse | Preserve component meaning and add versioned diagnostic metadata |

Proposed RenderCore components are `RayQueryScene` for scene acceleration structures and `EnvironmentVisibilityRenderer` for sampling/history/filtering. These names are provisional. Share scene geometry and material infrastructure rather than creating a second importer or native Vulkan renderer. Backend handles and Vulkan synchronization translation stay in VulkanRHI.

Follow the current [RHI lifetime and submission contracts](../ZenCore/Include/Graphics/RHI/README.md). Use engine containers, explicit C++ types without `auto`, one terminal return for value-returning functions, small necessary lambdas, and repository formatting. No renderer-owned native queue submissions or per-frame device-idle waits.

## Lighting contract

### Separate direct environment from bounced radiance

For hardware mode, define the unmaterialed diffuse environment signal as:

`D_env(x, n) = (1 / pi) * integral[L_env(w) * V_triangle(x, w) * max(dot(n, w), 0) dw]`.

For samples with solid-angle density `p(w)`, estimate it with `sum(L_env * V * cosine / (pi * p)) / sampleCount`. Begin with cosine-weighted hemisphere samples, for which the weight simplifies to `L_env * V`. Keep the PDF explicit in the interface so later environment importance sampling cannot silently change the energy scale.

A confirmed opaque hit contributes zero environment radiance. A miss samples the rotated HDR environment with the existing intensity convention. Trace against all eligible scene geometry, including objects outside the voxel volume. Derive the finite ray limit from scene bounds and receiver position so it cannot omit a registered occluder; document floating-point margins and test out-of-bounds receivers.

In hardware mode, composition uses:

`diffuse outgoing = receiver diffuse factor * (D_env_filtered + D_bounced_cones)`.

Apply existing metallic/Fresnel, albedo, AO, and material-layer factors exactly once at composition. Accumulate and filter the linear, unmaterialed environment signal. Disable the old escaped-environment branch of cone tracing for that receiver. Do not multiply the RT environment estimate by cone transmittance or a second voxel visibility term. Cone transmittance still applies while gathering bounced voxel radiance.

Preserve the current controls: indirect intensity scales the bounced term as it does today; environment intensity and environment enablement govern environment contributions. Analytic direct lighting, visible emission, specular IBL, skybox visibility, and their captures retain their existing contracts.

### Sampling and ray origins

Use a deterministic, spatially decorrelated sequence that advances with successful rendered frames. Distribute samples across the hemisphere rather than assigning a binary result to a broad cone. Supply a fixed seed/frame sequence for reference captures. Start with full receiver resolution and explicit 1, 2, and 4 sample presets for functional testing; choose production defaults only in H2.

Use a geometric normal and scale-aware origin offset for intersection robustness, with the shading normal used for the lighting estimator. Handle shading-normal directions below the geometric surface consistently and test the chosen correction against a reference. Document the offset and `tMin` policy for small, large, mirrored, and nonuniformly scaled geometry. Avoid ignoring an entire source instance to suppress self-intersections: that would remove legitimate self-occlusion.

For alpha masks, inspect candidate intersections, reconstruct the appropriate UVs and vertex alpha, apply material UV transforms and alpha cutoff, and continue traversal after rejected candidates. Do not mark masked geometry opaque. Match raster material and sidedness policy, including mirrored transforms and two-sided surfaces. Define texture LOD explicitly because compute queries have no implicit screen derivatives.

Blend and transmissive surfaces require an explicit occluder policy. Initially treat solid surfaces as opaque unless their supported mask rejects the hit; any conservative treatment of blend/transmission must be reported in diagnostics and tested. Physically correct colored transmission and volumetric shadow transport are deferred, not implied by enabling RT.

### Environment lighting for voxel bounce sources

Retain radiance injection and radiance mip filtering. In hardware mode, replace the voxel sky-irradiance visibility provider with the same triangle-query/material acceptance rules and distributed sampling. Evaluate from a valid represented surface position and normal, not an arbitrary voxel center inside a wall. Reuse the existing owner/triangle data and define a valid representative point for both owner and averaged reflectance policies.

Compute this cache when its geometry, surface-opacity, environment, or sampling inputs change. Use a deterministic sufficient-sample calculation for initial correctness; camera-space history must not be reused as voxel history. Preserve irradiance units, including the `pi` conversion required by injection. This remains a voxel approximation for material representation and bounce gathering; no triangle-accurate bounced GI claim is made.

## H0 Complete hardware query infrastructure

### H0a Capabilities and shader variants

- Expose enabled buffer device address, AS, ray-query support, queue build capability, geometry-format support, and relevant limits through generic RHI capabilities.
- Audit the complete extension dependency chain for the configured Vulkan API version. Do not infer usability from extension names or support flags alone, and enable only required features.
- Audit the current SPIR-V 1.3 build target. Compile query shaders for a supported ray-query target and validate them separately; retain RT-free variants for unsupported devices. Shader-program discovery must not instantiate unsupported modules.
- Preserve `--disable-rt` as a hard device-feature override. Runtime selection cannot enable a feature omitted at device creation.

Exit: supported, disabled, and unsupported paths resolve correctly; ordinary cone rendering starts without loading query shaders or allocating AS resources when RT is disabled.

### H0b Resources commands and descriptors

- Add a stable-identity RHI AS resource, build-size queries, storage allocation, device addresses, and explicit creation/build status. Expose vertex/index/instance build-input and scratch buffer requirements.
- Record build and update commands through the current command stream. Copy descriptions, geometry arrays, and ranges into command-owned storage so threaded execution cannot retain caller stack memory.
- Add AS reflection and ordinary descriptor binding, descriptor cache keys, pool sizing, and validation. Keep AS descriptors separate from the material texture heap.
- Retain TLAS, referenced BLAS, storage, geometry/material lookup tables, and other query dependencies through completion. An AS descriptor retaining its pool does not retain those resources.
- Validate build/update eligibility, formats, counts, instance fields, device-address range/offsets, and scratch alignment before recording. Use fresh resources for incompatible replacements.

GPU build/update rules and queried allocation sizes must follow the [Vulkan acceleration-structure specification](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html). AS compaction is an optional H2 optimization; serialization and host builds are outside scope.

### H0c RDG synchronization and retirement

Represent AS build writes, update source reads, query reads, and indirect BLAS references in RDG and submission history. Track build inputs and scratch as resources in the same graph. Audit all access/stage bit counts, fixed-size arrays, expansion loops, queue checks, and metrics when adding flags; the current enums contain 17 ordinary access/stage bits.

Required dependency chains are upload or deformation to BLAS build, BLAS build to TLAS build, TLAS build to query, previous readers to an update, and previous scratch users to scratch reuse. Query reads use the actual shader stage with AS-read access; builds use the AS-build stage and appropriate resource accesses. Follow the [Vulkan synchronization rules](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html#synchronization-for-ray-tracing).

Start builds and queries on the graphics queue. Preserve exact cross-graph producer provenance and retain resources until every relevant queue serial completes. Failed or rejected work must not publish valid AS generations, history, or reusable scratch. Native GPU allocation-failure recovery remains outside scope, as in the old H0–H2 plan; pre-allocation budget/limit rejection and existing failure contracts remain required.

Exit for H0: graph-driven triangle hit/miss/distance/barycentrics/instance/primitive/material identity is correct with no validation errors. Cover inline and threaded execution, empty scenes, shared BLAS instances, transforms, masked candidates, scratch reuse, replacement, and injected submission failures. Extend affected mock RHIs as well as native tests.

## H1 Integrate environment lighting and reconstruction

### H1a Scene geometry and query service

Build per-mesh BLAS resources from the same effective geometry and indexing used by rasterization. Share immutable BLAS across instances. Provide static-only, dynamic-only, and full-scene TLAS views without triplicating BLAS; build additional views lazily when requested by a consumer or test.

Track instance, geometry, topology, surface-opacity, and material generations separately. Rigid motion updates instance data; compatible deformations use eligible updates or rebuilds; topology changes rebuild. Position, skin/morph deformation, handedness, alpha acceptance, and material identity must match the raster frame snapshot. Keep query resources coherent with that snapshot across frames in flight.

Expose both any-accepted-hit visibility and closest-accepted-hit queries with versioned hit metadata. The environment estimator needs visibility; closest-hit identity and barycentrics preserve the useful H1 decoding contract for later consumers. Do not restore the old directional-GI sender cache solely to exercise decoding.

Exit: moving/deforming geometry, material-opacity edits, instance removal, scene replacement, and geometry outside the voxel volume all affect visibility correctly without stale addresses or mixed generations.

### H1b Unfiltered environment estimator and composition

Implement a full-resolution compute pass using the lighting contract above. Initially capture raw estimates at high sample counts with accumulation/filtering disabled to verify the estimator independently. Add deterministic low-sample sequences only after energy and material acceptance tests pass.

Split bounced and escaped environment terms at the common cone-lighting interface. Select exactly one environment provider for each receiver and capture the actual provider. Connect the hardware provider to voxel sky injection and preserve the existing radiance gather. Verify isolated direct, environment, emissive, bounced, and specular components, including zero-intensity cases.

Exit: high-sample results converge to a triangle visibility reference, the Sponza stripes disappear without opening closed geometry to sky light, and no environment contribution is counted twice.

### H1c Temporal accumulation and spatial filtering

Add the receiver data required for correct reprojection. Camera-only reprojection may begin with world position and the previous view/projection matrix, but moving receivers require previous transforms or previous deformed positions. Do not assume an existing usable velocity buffer. Preserve stable surface identity across frames and reset it on incompatible geometry changes.

Store previous receiver depth/position, normal, identity, environment estimate, luminance moments, and history length/confidence. Reproject in explicit pixel/UV conventions, accounting for viewport and projection changes. Reject history for off-screen coordinates, disocclusion, depth/normal/identity mismatch, camera cuts, and incompatible generations. Bound history length and use variance-aware neighborhood clamping to limit stale bright samples.

Moving occluders can invalidate lighting while the receiver remains stationary. Start with conservative history reset on any relevant occluder or opacity-generation change. This is a correctness baseline with a known convergence cost during animation. Local reactive invalidation and history reuse are H2 options only after ghosting tests pass.

Use depth-, geometric-normal-, and surface-identity-aware spatial filtering of linear environment radiance, guided by variance. Keep thin walls and foreground/background boundaries separate. Disoccluded pixels receive current valid samples rather than invented history. If half-resolution tracing is introduced in H2, require the same guides for upsampling and a tested fallback at unresolved edges.

| Change | Required response |
| --- | --- |
| Camera cut, projection change, resize, scene replacement | Reset receiver histories and recreate extent-dependent resources as needed |
| Environment texture, rotation, intensity, enablement | Invalidate environment history and affected voxel sky cache |
| Geometry, topology, transform/deformation, opacity acceptance | Update AS first; invalidate affected caches and conservatively reset environment history |
| Backend, sample distribution, resolution, bias, ray-range policy | Reset incompatible history and provider data |
| Albedo-only edit with unchanged opacity and receiver geometry | Recompose material factors; preserve unmaterialed receiver history where valid; refresh affected voxel radiance |
| Rejected/failed frame | Publish neither its history nor successful AS/cache generations |

History read/write resources must be ordered through RDG across frames in flight. A two-image ping-pong scheme alone is insufficient without dependencies on prior readers and writers. Publish previous camera/geometry/history state only for successful frames.

Exit: static convergence, camera motion, moving receivers and occluders, disocclusion, resizing, pauses, cuts, and failed submissions pass without ghost trails or cross-surface filtering leaks.

### H1d Receiver coverage controls and fallback

Cover deferred opaque and alpha-tested receivers first. Forward opaque material variants also call `DiffuseVoxelLighting`; add a compatible receiver prepass for those surfaces, preserving their final depth, normals, identity, and material-side convention. They must consume their own reconstructed environment result, not an unrelated opaque G-buffer pixel.

Transparent layers cannot generally reuse a single opaque-screen history. Provide an explicit per-surface query variant without temporal reconstruction for supported forward layers, or retain their current cone environment path with clear coverage metadata. The initial required reconstruction scope is opaque and alpha-tested diffuse receivers; transparent transmission/scattering remains a documented limitation. Never suppress their old environment term unless a valid replacement exists.

Proposed settings, to be integrated with existing runtime settings and tests rather than treated as already available:

| Setting | Proposed behavior |
| --- | --- |
| `environment_visibility_backend=auto\|voxel_dda\|hardware_rt` | Select only the environment provider; cone bounce transport stays selected independently |
| `environment_rt_samples_per_pixel` | Explicit validated sample count; initial test presets 1, 2, 4 |
| `environment_rt_temporal_enabled` | Disable for independent raw-estimator/reference tests |
| `environment_rt_filter_enabled` | Disable to measure temporal and spatial contributions separately |
| `environment_rt_history_limit` | Bounded accumulation length, with documented clamp/reset policy |
| `environment_rt_budget_mb` | Pre-allocation cap covering AS, scratch, lookup tables, reconstruction, and overlapping retired generations |

During H0–H1, `auto` keeps the existing provider. Explicit hardware requests either activate a usable complete provider or report the reason and actual fallback. Diagnostics must separate requested backend, enabled device features, ready scene resources, selected provider, receiver coverage, and reset reason. Do not silently reduce voxel resolution, sample count, or unrelated quality settings to fit a budget.

Use the existing cone/PBR availability policy if the overall GI path cannot run. Do not describe fallback as hardware RT. Free retired provider resources after their actual readers complete. Switching back to an existing provider restores its environment branch and invalidates incompatible histories.

Exit for H1: all functional and image-quality checks below pass with explicit hardware selection, provider switching in both directions, unsupported capabilities, RT disabled, budget rejection, and frames in flight. Publish scope and limitations before H2. Performance does not gate H1.

## H2 Measure optimize and decide automatic selection

Begin new GPU profiling work and optimization after H1 correctness acceptance, following the original H0–H2 ordering. The prior debugging measurements motivate this plan but do not establish a new backend performance result.

Extend the existing profiling/capture infrastructure rather than adding a separate timing system. Report cold AS construction, rigid updates, deformation updates/rebuilds, voxel sky updates, receiver query, temporal, filter/upsample, composition, complete GPU frame, CPU submission cost, and peak committed device memory. Include scratch and old generations retained during transitions. Report sample counts and actual rays issued, not only configuration values.

Measure the legacy cone path and hardware environment path on the same scenes, camera matrices, viewport, G-buffer extent, voxel resolution, material settings, and hardware. Include the Sponza reproduction at 256 cubed to match the investigation and 64/128 cubed for scaling. Verify RT-disabled compatibility on a supported device and an unavailable-capability path. Record AMD and NVIDIA measurements separately when those devices are available; untested vendors remain unverified.

Use 1920×1080 as the primary viewport. Carry forward 16.7 ms total mode-3 GPU frame time as an engineering target, not a guaranteed outcome or functional gate. Freeze the tested preset and exact hardware before assessing it. Report median and tail times, initialization costs, and moving-scene costs; a faster isolated query pass does not prove a faster frame.

Candidate optimizations, each requiring correctness revalidation, are environment importance sampling with correct PDFs, adaptive sample allocation, reduced-resolution tracing with guided reconstruction, spatially localized history invalidation, sparse voxel-sky updates, eligible BLAS updates/compaction, scratch pooling, and validated async queue placement. Do not raise bias or remove occluders to meet a time target.

Promote `auto` to hardware only for measured, validated capability/preset combinations with acceptable complete-frame cost and memory. Otherwise retain explicit opt-in and publish non-promotion. Preserve the compatibility provider and report the exact selection reason.

Exit for H2: reproducible performance and memory reports, post-optimization correctness results, documented presets, and an explicit automatic-selection decision. Missing hardware evidence cannot be marked passed.

## Validation and acceptance evidence

Freeze numerical tolerances, convergence windows, image regions, seeds, and performance comparison inputs before evaluating optimized results. H1 begins by checking the reference against analytic open/closed fixtures; do not derive acceptance limits from a candidate implementation's error. Store raw linear HDR data as well as display images.

| Test group | Required evidence |
| --- | --- |
| Triangle-query oracle | Hit/miss, distance, barycentrics, instance/primitive/material identity against an independent CPU or trusted triangle reference; masked rejection continues to later blockers |
| Open and enclosed geometry | Constant-environment normalization; fully enclosed opaque receiver has zero raw environment contribution within frozen numeric tolerance; no filter leakage through thin walls |
| Sparse and oblique blockers | Thin pole, slanted wall, small aperture, mirrored/two-sided mesh, alpha-cutout geometry, and occluders outside the voxel grid |
| Sponza floor | Capture the documented environment on/off/on UI sequence and zero-analytic-light isolation at fixed camera matrices. After the fix, environment-on floor-region HDR comparisons must show removal of false hard stripes while preserving legitimate occlusion; disappearance only with environment disabled does not pass. Separate escaped environment, bounced diffuse, and specular components. |
| Estimator isolation | Raw high-sample reference, low-sample raw, temporal-only, and temporal-plus-filter outputs; demonstrate convergence and quantify bias/noise separately |
| Lighting composition | No duplicated environment energy; zero environment, zero indirect, emission-only, and analytic-only cases; direct/specular/emission invariants where their inputs are unchanged |
| Animation and history | Camera motion/cut, moving occluder over a static floor, moving/deforming receiver, alpha edits, disocclusion, environment rotation, and scene replacement; bounded settling time and no stale history |
| RHI and RDG | Inline/threaded execution, graph boundaries, scratch reuse, multiple frames in flight, queue equivalence, failed submission, object retirement, and zero validation/synchronization errors |
| Selection and limits | Disabled/unsupported features, explicit requests, budget/limit rejection, empty scene, switching both ways, reported fallback and coverage |
| Compatibility | Existing voxel occupancy/reflectance and cone lighting tests remain valid; RT-off image baselines remain unchanged within existing tolerances |

Use the existing `scene_renderer_demo`, `ConfigLoaderTest`, `CommonTest`, `RenderCoreTest`, `VulkanRHITest`, and `ConeVoxelGIIntegrationTest` targets where applicable. Add focused native ray-query and reconstruction integration tests following current target conventions. Run shader compilation and SPIR-V validation for each variant, plus Vulkan synchronization validation. Distinguish skips and unrelated pre-existing failures from passes.

Capture metadata must include selected environment provider, cone bounce provider, receiver coverage, frame/geometry/environment generations, sample seed/count, history validity/length, filter settings, and reset reason. Keep the existing seven lighting components compatible where their semantics remain valid; version the format if changing them. Add raw environment, reconstructed environment, variance/history, and rejection debug outputs without making them permanent production allocations.

## Relationship to the older H0 H1 H2 plan

| Earlier requirement | Disposition in this plan |
| --- | --- |
| H0 capabilities, AS, descriptors, commands, synchronization, lifetime | Required in full for hardware queries |
| H1 static/dynamic/full-scene views, transforms, deformation, masks, out-of-grid geometry | Required in the reusable query service; full-scene view drives environment visibility |
| H1 triangle hit decoding | Required as versioned instance/primitive/barycentric/material metadata |
| H1 directional-GI sender packing and shared directional gather | Deferred with that older algorithm; cone transport remains the accepted bounce method |
| H1 provider switching and histories | Required for the new environment provider and its reconstruction resources |
| H2 profiling, optimization, memory and automatic promotion | Required after functional acceptance, with independent hardware measurements |

This mapping deliberately does not mark the old paper-based Stage B checklist complete. It completes the reusable hardware foundation and the newly accepted environment-lighting consumer. A future directional-GI revival can reuse the query service but needs its own sender-cache/gather integration and acceptance.

## Delivery checklist

- [ ] Archive the baseline, reference fixtures, limits, and HDR capture contract.
- [ ] H0a enabled capabilities and RT-free/query shader selection pass.
- [ ] H0b AS resources, recorded commands, reflection, and descriptors pass.
- [ ] H0c graph dependencies, lifetime, retirement, and failure tests pass.
- [ ] H1a scene updates and material-aware query service pass.
- [ ] H1b distributed environment estimation, voxel sky injection, and composition pass.
- [ ] H1c motion/reprojection, accumulation, and edge-aware filtering pass.
- [ ] H1d receiver coverage, runtime settings, fallback, and switching pass.
- [ ] H1 functional report records correctness evidence and limitations.
- [ ] H2 profiling and optimization report records full-frame and memory results.
- [ ] Automatic-selection promotion or non-promotion is documented.

Each milestone should leave the legacy renderer usable and produce reviewable code, focused tests, and a short verification record. This document records the plan only; none of its unchecked implementation items is claimed complete.
