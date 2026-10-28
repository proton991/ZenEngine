# Brixelizer based Compute GI implementation plan

2026-10-26; execution started 2026-10-27. Status, 2026-10-28: the replacement, partial static updates, separate dynamic volume and legacy removal are implemented; final qualification and profiling are in progress. The CPU BVH, brick and retrace paths are deleted. Compute GI traces a GPU-built distance field with local lamp detail. The latest still-camera pilot costs 3.60 ms in GI passes against a corrected, shadow-map-matched 2.97 ms baseline, still above the 10% target. Earlier comparisons mixed direct-shadow providers and are superseded. Old Sponza now constructs with explicit unavailable regions instead of failing the whole field. [Execution evidence](BrixelizerGIExecution.md) and [profiling evidence](BrixelizerGIProfiling.md) record the measured results and open items. The original quality and budget gates below are unchanged.

Remove the CPU triangle BVH from ZenEngine and replace its Compute GI responsibilities with a GPU-built sparse distance-field scene, referring to AMD FidelityFX Brixelizer and Brixelizer GI. Keep the existing screen-probe gather, lighting-cache injection, composition, and hardware ray-query backend where their contracts remain suitable.

The completed renderer must have no `SoftwareSceneRayQuery`, CPU BVH build worker, software triangle-BVH shader, or runtime fallback that recreates them. GPU construction and traversal of a hierarchy over distance-field bricks are part of the replacement. Preparing scene data and converging lighting still take time; warm backend switching must not trigger scene reconstruction.

This plan supersedes the CPU-BVH dependencies and fallback decisions in the [previous Compute GI plan](E:/Dev/ZenEngine/Doc/ComputeGI/ImplementationPlan.md). Its recorded measurements remain historical baselines.

## Reference implementation and integration decision

Use FidelityFX SDK **v1.1.4**, including Brixelizer GI **1.0.1**, as the initial source reference. Record the resolved commit and preserve AMD's license notices for adapted code. AMD publishes Vulkan support for this implementation. [AMD release and platform information](https://gpuopen.com/fidelityfx-brixelizer/)

| Reference | Application in ZenEngine |
| --- | --- |
| [Brixelizer construction source](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/components/brixelizer/ffx_brixelizer_raw.cpp) | Resource layout, job scheduling, indirect dispatch, construction dependencies, and capacity handling. |
| [Brixelizer GPU sources](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4/sdk/include/FidelityFX/gpu/brixelizer) | Distance generation, brick compression, distance propagation, hierarchy construction, and ray traversal. |
| [Brixelizer GI source](https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4/sdk/src/components/brixelizergi/ffx_brixelizergi.cpp) | Cache resources, history resets, screen-probe and specular pass dependencies. |
| [Brixelizer GI algorithm](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/) | Surface screen probes trace the field and shade hits through a radiance cache. |

Implement the required passes through ZenEngine's existing render graph, shader system, and RHI. Adapt the relevant AMD algorithms and kernels; do not introduce a second Vulkan resource or submission manager through the complete FidelityFX backend. Keep an upstream-to-Zen pass mapping with the adapted source so deviations remain reviewable.

The first release is an engine-native implementation informed by Brixelizer GI, not a claim of identical AMD output. These choices bound its scope:

- Replace binary occupancy tracing with actual sparse unsigned distance fields. In Brixelizer, SDF means **sparse distance field**, not signed distance field. Its hierarchy contains bricks rather than scene triangles. [Brixelizer representation](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer/)
- Reuse Zen's persistent screen probes, reconstruction, and temporal filtering. Do not replace these merely to duplicate AMD's pass names.
- Retain Zen's scene-wide radiance injection, including off-screen surfaces. AMD populates its radiance cache from previous lit frames; adopting that alone would change how unseen lamps and emissive surfaces contribute. [Brixelizer GI cache inputs](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/)
- Keep the current bounded radiance-cache volume for the first release. Field visibility coverage must contain both that volume and all committed scene geometry that can occlude GI, independently of `SetVoxelBounds`. Start with one field level covering both; use nested levels where needed to meet the quality and memory gates without dropping outer geometry. Camera-following clipmaps require corresponding lighting-cache coverage and are a follow-up.
- Keep single-bounce lighting as the initial comparison. Multi-bounce feedback, a new world-space irradiance cache, HZB screen traces, and AMD's specular pretrace optimization are follow-ups, not prerequisites for CPU BVH removal.
- Direct shadows use shadow maps without HW RT. Hardware shadow rays and hardware GI remain supported.

## Current dependencies and their replacements

| Current dependency | Required replacement |
| --- | --- |
| [BrickScene](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/BrickScene.cpp:145) receives world triangles from `SoftwareSceneRayQuery` and waits for its readiness. | Read the committed scene's GPU geometry directly. Generate field data without CPU triangle transformation, sorting, or BVH publication. |
| [BuildRayQueryGraph](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp:705) prepares software queries and falls back to them when bricks fail. | Independently prepare HW acceleration structures and the distance-field scene according to the requested passes. Field failure must not select software triangle tracing. |
| [Sky-cache preparation](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp:865) uses full-scene software triangle visibility, including blockers outside the radiance-cache volume. | Add field-based sky visibility while preserving that coverage, environment importance sampling, and representative receiver surfaces. |
| [Near-lamp retracing](E:/Dev/ZenEngine/Data/Shaders/VoxelGI/hit_radiance.glsl:110) and probe/pixel retrace passes query triangles. | Replace both the bounce hit and hit-to-light visibility with field-compatible solutions, then remove retrace queues and shaders. |
| [Provider bindings](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp:1039) bind software buffers even in mixed HW/Compute paths. | Bind only the resources required by the selected shader variants; HW-only operation needs no dummy software-BVH buffers. |
| [Lighting controls](E:/Dev/ZenEngine/ZenUI/ImGui/Source/LightingBackendControls.cpp:53), config, profiling, and tests expose software BVH modes. | Offer HW RT or Compute for GI, and HW RT or shadow maps for direct shadows. Migrate old settings and remove the retired mode throughout. |

## Geometry ownership and GPU construction

Replace `BrickScene` with a narrowly scoped distance-field scene owner under `RenderCore/V2`, owned by `VoxelGIRenderer`. It owns field resources, construction state, revisions, and retirement. `ScreenProbeGather` owns gather histories; `RenderScene` remains the source of committed geometry. Native Vulkan code continues to implement mechanisms rather than GI policy.

The geometry inputs already exist: vertex/index buffers, node transforms, material/UV data, stable triangle records, instance bounds, and static/dynamic revisions. Reuse [VoxelizerBase's bindings](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/VoxelizerBase.cpp:316) and [TriangleVertices](E:/Dev/ZenEngine/Data/Shaders/VoxelGI/voxel_geometry.glsl:77). CPU work may prepare instance jobs and bounds; it must not expand every mesh into a second world-space triangle scene.

The construction sequence will follow the AMD implementation, expressed as RDG passes:

1. Identify invalid regions and cull instance jobs against them.
2. Process geometry on the GPU, with separate work distribution for large triangles.
3. Count, scan, and compact triangle-to-cell references into bounded scratch storage.
4. Allocate or recycle occupied bricks and evaluate distances to their contributing geometry.
5. Complete missing distances, compress the atlas, and build brick bounds in the ordering required by the pinned kernels.
6. Build the fixed spatial hierarchy over the published bricks; merge static and dynamic representations where applicable.

Use the reference's 8-by-8-by-8 distance samples per brick: these span seven intervals per axis, with matching boundary samples between neighboring bricks. Do not reuse the occupancy tracer's eight-subcell size calculation. Establish atlas addressing, distance normalization, quantization, and grid/hierarchy dimensions from the pinned kernels, then port construction and traversal together. The reference uses cascades and GPU-generated AABB trees; neither requires the current CPU triangle BVH. [AMD construction presentation](https://gpuopen.com/download/GDC-2023-Sparse-Distance-Fields-For-Games.pdf)

Persistent storage includes the distance atlas, cell-to-brick maps, hierarchy, allocation metadata, and generation/validity data. Scratch storage includes work queues, triangle references, scans, and indirect arguments. Measure both persistent memory and peak construction memory, including overlapping frames and replacement resources.

Capacity overflow must produce an explicit invalid construction result or incomplete region. Do not publish missing geometry as empty space, convert an arbitrary region into a solid occluder, or silently fall back to a BVH. Use a valid coarser field where available; otherwise use the defined temporary rendering fallback.

## Tracing and lighting contracts

Provide one field-trace implementation for the Compute portions of diffuse GI, specular occlusion, reflections, sky-cache visibility, and local-light visibility. Its result distinguishes a hit, a covered miss, and an unavailable result caused by missing data or an iteration limit. A covered miss requires valid field data along the requested ray segment wherever it intersects committed scene bounds; space beyond those complete bounds is known empty. Sky-cache rays test the full scene along their direction, and local-light visibility tests the segment to the light. Leaving the radiance-cache volume does not end either visibility query. Only a covered miss may contribute unobstructed environment lighting or unoccluded local light.

Trace the brick hierarchy, then march the local distance field using the reference's quantization-aware step and hit rules. Normals come from a valid local distance gradient or the existing compatible surface cache. Preserve geometric normals for visibility offsets and receiver shading normals for BRDF evaluation. Validate both sides of thin surfaces, grazing rays, origins near a surface, and transitions between field levels.

The field hit is approximate and has no intrinsic material record. Adapt it to Zen's existing radiance and surface-class cache without inventing a triangle ID. Keep validity and facing tests so the back of a wall cannot borrow its illuminated front. A hit outside the radiance-cache volume still blocks visibility but contributes no cached hit radiance; do not turn it into a miss or clamp its lookup to a cache boundary cell. Do not add the environment, cached direct lighting, and analytic local lights twice. Keep the existing `hybridSky`, `hybridBounce`, and `hybridSpecular` composition contracts, including forward/translucent consumers.

Sky-cache rays use the field when Compute is selected. Keep their current sampling distribution during the first comparison so visibility changes can be isolated. Direct-light injection uses the selected shadow maps or HW shadow queries; near-light visibility needed during Compute hit shading must not call `SoftwareTraceOpaqueRay`.

Both probe gather and per-pixel diagnostic/reference accumulation must support field tracing. Ray-count instrumentation must count the selected field path and report distance samples/iteration limits separately from ray counts. Mixed HW/Compute feature masks must continue to select the requested backend for each feature.

## Small geometry and material quality

New Sponza is the required acceptance scene. Existing tests showed that approximately 7 cm occupancy voxels closed small lantern openings. For probes with all lights, triangle retracing improved floor bounce bias against the CUDA reference from about -41% to -5%; lamps-only floor bias improved from about -80% to -14%. Removing that correction without replacing its accuracy is unacceptable. [Recorded lantern experiment](E:/Dev/ZenEngine/Doc/ComputeGI/Verification.md:8)

Run a small field-tracing experiment on a lantern and thin-wall fixture before integrating the whole renderer. Sweep field resolution, hit tolerance, and ray bias; visualize the reconstructed field and the light paths that remain open. Distance samples should improve surface localization over occupancy bits, but cannot be assumed to preserve sub-cell openings. Fine geometry can also make tracing and construction expensive. [AMD performance guidance](https://gpuopen.com/learn/getting-the-most-out-of-fidelityfx-brixelizer/)

Alpha masks need an explicit representation. The stock geometry-only field can treat foliage cards as opaque and over-darken GI. Preserve Zen's material-aware coverage during GPU field generation, or adapt a separate coverage cache with validity handling. Test obscured and off-screen cards; dropping masked geometry from the field is not an acceptable general solution. [AMD foliage analysis](https://gpuopen.com/learn/brix-foliage-gi/brix-foliage-gi-foliage_in_brixelizer_gi/), [AMD coverage-cache extension](https://gpuopen.com/learn/brix-foliage-gi/brix-foliage-gi-extending_the_gi/)

Use these proposed rollout gates at the existing CUDA comparison settings: uniform 0.5 albedo, `PhysCamera001`, 480-by-270, matched sampling and exposure, and separate sun-only, lamps-only, and all-light captures. Preserve the masks and normalized mean-error definition in [ground_truth_bounce.py](E:/Dev/ZenEngine/tools/ground_truth_bounce.py:115):

- All-light bounce: absolute mean bias at most 15% overall and 10% on the floor.
- Lamps-only bounce: absolute mean bias at most 22% overall and 20% on the floor.
- No new opaque-wall leaks, fully black curved receivers, or missing off-screen emissive contributions in the targeted fixtures.
- Inspect error distributions and images as well as means; opposite errors must not cancel into a passing result.

These are acceptance targets derived from the existing corrected renderer, not measured field results. If they cannot be met within the construction and memory budgets, resolve local field detail or report the remaining quality/cost tradeoff before default rollout. Do not reintroduce the CPU BVH or silently weaken the gates. A separate local triangle-intersection technique is outside the initial Brixelizer-based path and would require an explicit design amendment.

## Scene changes and switching

Use committed scene identity plus geometry, surface, and lighting generations. Reuse the existing per-instance bounds and revisions; add only the dirty-region information needed by the field owner. Rebuild affected static regions, update dynamic geometry separately, and relight caches when only lighting changes. An alpha-cutoff or coverage-changing material edit invalidates the field as well as lighting; an albedo-only edit invalidates lighting.

On movement, removal, disabling, or class changes, invalidate both previous and new bounds as applicable. Vertex edits invalidate every instance using the edited geometry. Retain the last submitted bounds until the replacement submission is accepted. New scenes never reuse old scene field or lighting generations. [RenderScene update contracts](E:/Dev/ZenEngine/ZenCore/Include/Graphics/RenderCore/V2/RenderScene.h:82)

Track field visibility bounds with the committed geometry generation. Geometry moving beyond the published coverage makes the affected visibility region unavailable immediately, even when the radiance-cache bounds stay fixed. Expand or replace the field within the construction budget, and refresh affected sky/radiance caches and gather histories when the replacement becomes usable. A radiance-cache bounds change must not discard field coverage of scene occluders.

Schedule construction in bounded jobs with explicit scratch and reference-count limits. Split oversized work before dispatch, including large-triangle workloads; GPU timestamps alone cannot interrupt a long dispatch. Dirty regions may accumulate, but continuous animation must not restart a full-scene build indefinitely. Invalidate stale regions immediately and publish only completed, internally consistent field data.

Every producer and consumer declares its RDG accesses. Retire superseded atlases, scratch storage, and scene references after their actual GPU users complete. Do not flip history indices or commit a completed generation when a frame is declined. No new device-idle wait, synchronous GPU readback, or wait for a CPU future may appear in the backend-switch path.

For an editor scene with GI enabled, prepare the Compute field during scene activation even when HW RT is selected, then keep it current within its budget. This spends GPU time and memory to support fast switching. Preserve it across backend toggles; changing the selected backend invalidates affected lighting histories, not geometry. An explicit resource eviction or quality/resolution change can require preparation again and must be reported as a cold transition.

When the requested field is not ready, keep an already active HW GI path if available; otherwise show a voxel-cone GI preview with shadow maps from the first rendered frame while GPU preparation progresses. The preview uses the current scene's radiance voxels, is explicitly labelled as a preview, and does not sample an unpublished field. Field publication starts ray/probe gathering with fresh history and recomputes sky visibility. A permanently failed allocation or a scene coverage requirement that cannot fit the qualified field budget reports failure instead of displaying preparation indefinitely. Missing regions must not appear as valid stale field GI or unoccluded sky.

The steady-state target is an effective warm switch within two submitted frames, with no geometry rebuild. Initial preparation favors throughput: the user accepted a few seconds of stalls or stutter on 2026-10-29 to get the full distance field ready sooner. Later geometry edits retain the smaller background budget. These cases must have distinct status and profiling metadata.

## Implementation milestones

| Milestone | Work | Exit condition |
| --- | --- | --- |
| 0. Baselines and source pin | Resolve the AMD tag to a commit; map required kernels and capabilities. Capture current HW and Compute output, frame time, preparation time, and memory on old/new Sponza. Retain the baseline revision, build inputs, and configuration for the final Nsight comparison. | Reproducible baseline and a bounded native-port scope. |
| 1. GPU geometry input | Remove the brick builder's dependency on software-query triangle ownership. Reuse committed GPU scene geometry and implement instance/dirty-region jobs. | Existing brick output and geometry-edit tests remain valid without invoking a CPU BVH for construction. Other legacy queries may still exist temporarily. |
| 2. Distance-field scene | Implement bounded construction, atlas, hierarchy, traversal, and debug views. Validate complete scene visibility coverage independently of radiance-cache bounds, then partial static and dynamic updates. | Analytic trace tests pass, including blockers outside the cache volume; allocation overflow and invalid regions are distinguishable; lantern and alpha experiments establish a viable quality/budget range. |
| 3. GI integration | Route sky cache, probe/pixel GI, reflections, and local-light visibility through fields. Adapt hit-cache lookup and mixed-provider bindings. | A complete Compute frame runs with CPU BVH preparation disabled, including diagnostic modes and fallback handling; the outside-volume sky-cache and local-light fixtures pass. |
| 4. Switching and lifetime | Prepare fields on scene activation, retain across toggles, budget cold work, and publish consistent revisions. | Old Sponza to New Sponza, HW to Compute, edits between toggles, repeated switches, and shutdown during preparation pass in Debug and Release. |
| 5. Remove legacy implementation | Delete CPU BVH code, workers, shader traversal, retrace passes, settings, bindings, and obsolete tests. Migrate configuration and capture metadata. | Engine, editor, and sample targets build and run without any CPU-BVH implementation or fallback. |
| 6. Qualify defaults | Run full scene quality, motion, frame-time, memory, and GPU validation checks; set measured field presets. | Quality and responsiveness gates pass, costs are documented, and remaining limitations are explicit. |
| 7. Nsight Graphics performance profiling | Profile the implemented Compute GI with GPU Trace and supported shader/hardware metrics; compare against the corrected Compute baseline under matched conditions. | Reproducible captures and a report identify dominant costs, queue dependencies, budget results, and prioritized optimization opportunities; uninstrumented runs confirm whole-frame performance. |

Milestones are sequential implementation slices. The old path may remain available for baseline comparison during migration; it is deleted before completion. No migration feature flag is allowed to preserve a hidden CPU BVH in the final renderer.

## Removal and configuration checklist

- Delete `SoftwareSceneRayQuery.h/.cpp`, `SoftwareRayNode`, `SoftwareRayTriangle`, software-query uniform/buffer bindings, and `software_ray_query.glsl`.
- Remove software preparation futures, cancellation, snapshot publication, pending flags/reasons, dummy software buffers, and their CMake entries. Keep any progress UI that is genuinely needed for GPU field preparation, with accurate state names.
- Remove `probe_retrace_bricks.comp`, pixel/fallback/inline/counting triangle retrace variants, their queues, shader registrations, and RDG passes after their responsibilities have replacements.
- Remove `ComputeTraversal::Triangles`, Software BVH UI entries, and software-BVH direct shadows. Consolidate the final Compute tracer as distance fields rather than maintaining parallel occupancy and field backends.
- Migrate `voxel_gi_compute_traversal=triangles` and `bricks` to the new field backend. Migrate an explicit software shadow selection to shadow maps. Record a one-time migration reason; new captures and saved settings use canonical names.
- Replace occupancy-specific brick-resolution controls with field resolution/quality and memory controls. Do not reinterpret old numeric resolutions as physically equivalent distance-field quality. Choose the corresponding measured preset during migration.
- Preserve global Auto/HW/Compute selection, per-feature HW/Compute choices, GI contribution toggles, and explicit shadow-map selection. An unavailable HW provider resolves to Compute GI plus maps.
- Update `VoxelGISettings`, shared lighting controls/tests, demo profiling and lighting captures, shader registration, and documentation. Replace obsolete BVH tests with geometry-generation, field-trace, and lifecycle tests; retain the independent HW and offline lighting references.
- Search source, shaders, build definitions, and active tests for retired symbols. Historical verification documents may retain their names and measurements.

## Verification and performance targets

Use the existing RenderCore, RayQuery, BrickScene, ScreenProbeGather, ConeVoxelGI, editor-rendering, shader-reflection, and lighting-control suites as starting points. Extend tests around behavior and ownership rather than duplicating the new implementation.

| Area | Required coverage |
| --- | --- |
| Geometry and tracing | Analytic planes/boxes, empty scenes, rotated/scaled meshes, shared meshes, large triangles, thin walls, lantern gaps, masked cards, grazing rays, hierarchy/level boundaries, out-of-volume rays, and iteration-limit handling. |
| Updates | Transform and vertex edits, removal/disable/reenable, static/dynamic changes, material coverage changes, lighting-only edits, and edits while HW RT is selected. |
| Lifetime | All existing frame-submission modes, declined frames, frames in flight, resize, resource eviction, memory pressure, scene replacement, and shutdown during preparation. |
| Rendering | Sky, bounce, specular visibility, reflections, off-screen lighting, translucent receivers, per-pixel captures, mixed providers, and direct shadows with HW RT disabled. |
| Responsiveness | Repeated warm toggles, cold HW-to-Compute, and old-to-new Sponza while Compute is selected. Count effective-backend frames and separate asset loading, GPU preparation, and temporal convergence. |

Add a fixed-small-cache fixture with a receiver inside the cache and an opaque blocker outside it. Compare Compute sky-cache irradiance and resulting bounce with HW or the independent offline reference; the blocker must suppress the sky contribution and removal must restore it. Also test a hit-to-light segment crossing an external blocker, an out-of-cache hit with no cached radiance, and an unobstructed segment extending beyond scene bounds. Move, disable, and reenable the blocker across published field bounds while keeping cache bounds fixed; verify unavailable-region fallback during preparation and correct lighting after publication, with no stale sky cache. Extend the existing [outside-voxel-bounds query fixture](E:/Dev/ZenEngine/ZenSamples/CommonTest/RayQueryIntegrationTests.cpp:402) for field tracing and exercise the sky-cache consumer explicitly.

Profile textured New Sponza at **1600-by-900 on the RTX 5080**, using the same camera, lights, shadow maps, gather settings, and warmed shader cache for paired runs. Use Release for GPU cost comparisons and both Debug and Release for preparation/UI responsiveness. Run on a second GPU architecture or a device without HW RT before making a broad portability claim; explicitly forcing Compute on the RTX remains a required test.

Proposed initial budgets:

- No CPU triangle BVH allocation, build, traversal, or wait in any renderer configuration.
- Warm backend switching takes at most two submitted frames once current field data exists.
- Target at most 2 ms of additional GPU construction work per frame for background geometry updates. Initial scene and local-field preparation may exceed that target under the 2026-10-29 throughput requirement; size work from GPU timestamps, respect storage and device limits, and measure time until all fields are ready as well as stalls.
- Target steady-state total GI time within 10% of the current corrected Compute baseline under matched settings. Report whole-frame and update costs separately; a faster tracer cannot hide a more expensive builder.
- Start with a 256 MiB persistent field budget and 128 MiB construction-scratch budget on the reference configuration, including all levels needed for complete scene visibility coverage. Account for retirement and frames in flight when reporting peak memory; compare against current occupancy plus BVH memory. These starting budgets must be validated before becoming presets.

Record per-pass GPU medians and p95, worst transition frame, CPU preparation time, time until the selected backend becomes effective, brick/reference counts, overflows, unavailable rays, and resident/peak bytes. Include still-camera, camera-motion, and dynamic-geometry runs. Capture field distance, hierarchy occupancy, normals, cache validity, dirty regions, and trace-iteration debug views for failures.

## Final phase: Nsight Graphics performance profiling

Run milestone 7 after the implementation and default qualification are complete. Use [Nsight Graphics GPU Trace](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html) to inspect GPU execution, synchronization between queues, utilization, and dominant dispatches. Use its Shader Profiler and available hardware counters to investigate expensive shaders; record unsupported metrics explicitly.

1. **Reproducible setup.** Use the Release RTX 5080, 1600-by-900 configuration above, with Compute GI explicitly selected, HW ray queries disabled, and direct shadows using maps. Record the effective providers, scene/camera, quality and sampling settings, submission mode, executable and shader hashes, driver/Nsight versions, GPU clock policy, warmup, and capture length. Enable RDG GPU markers and disable validation for performance runs; keep correctness validation separate. Adapt [profile_hybrid_gi.py](E:/Dev/ZenEngine/tools/profile_hybrid_gi.py) for the implemented field settings and capture cases rather than inheriting its older frozen-view defaults. Run config-owning capture helpers serially and restore the configuration afterward.
2. **Capture cases.** Capture settled still-camera, reproducible camera-motion, and dynamic-geometry workloads on New Sponza, with old Sponza as a secondary comparison. Capture cold scene preparation from the first relevant submission through field readiness, coverage expansion after an edit, and warm HW/Compute switches separately. Switching runs may enable HW RT; identify fallback and effective-provider frames rather than counting them as Compute GI. Use at least three repeated runs per comparison, record the sampled frame counts, and retain complete transition sequences. Use repeated-frame counter collection only for reproducible steady workloads.
3. **Cost attribution.** Map markers to field construction and updates, hierarchy work, sky-cache generation, radiance injection, probe tracing, specular tracing, reconstruction/filtering, and composition. Report GPU frame median/p95, pass distributions, worst dispatch and transition frame, preparation-to-ready time, and memory using the accounting boundaries above. Inspect queue overlap, waits, barriers, idle gaps, and the work that determines frame completion. For the dominant dispatches, inspect supported occupancy, warp-stall, memory-bandwidth, and cache metrics; distinguish measured bottlenecks from hypotheses. Use the GPU frame timeline for whole-frame cost: overlapping pass durations must not be summed into frame time, and CPU submission or replay duration is not GPU execution time.
4. **Matched comparisons.** Recapture the retained milestone 0 baseline under the same Nsight clock policy, camera, lights, shadow maps, submission mode, and gather settings, with the agreed quality gates passing. Compare total frame time and construction/update cost as well as tracing cost. Historical normal-clock measurements are context, not a directly comparable baseline for clock-locked captures. Confirm throughput in separate uninstrumented runs and compare the results with the existing time, memory, and switching budgets. A HW RT run may provide additional context but does not replace the corrected Compute baseline.
5. **Evidence and report.** Save raw `.ngfx-gputrace` captures, timing/counter exports, launch logs, configuration, and reproduction commands under `build/compute-gi/brixelizer-nsight/<run-id>/`. Reject incomplete or overflowed traces and verify marker attribution before using their timings; the existing helper records hardware-event overflow and attribution problems, so select and record a validated timing mode. Write `Doc/ComputeGI/BrixelizerGIProfiling.md` with matched result tables, budget outcomes, dominant bottlenecks, and prioritized optimization opportunities. Link every finding to its capture, frame, queue, and marker range. When a `.ngfx-capture` is needed to inspect commands/resources, retain its exports and include exact Nsight event IDs and dispatch anchors. Report missing evidence explicitly. If profiling leads to code or preset changes, repeat the affected quality checks and matched measurements.

Completion requires deletion of the CPU BVH, a qualified replacement, and the final Nsight profiling report. Unresolved performance-budget failures remain open findings. Changing the worker thread, substituting a CPU-built distance structure, or retaining a software triangle-BVH fallback does not satisfy this plan.
