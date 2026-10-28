# Brixelizer GI source pin and native construction map

This is the implementation source map for [BrixelizerGIImplementationPlan.md](BrixelizerGIImplementationPlan.md). It separates the pinned AMD contracts from Zen's native adaptation and measured checkpoints. It does not import a second SDK renderer/backend.

## Immutable source pin

- Repository: [GPUOpen FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK).
- Release tag: `v1.1.4`.
- Resolved commit: `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`.
- The tag reference response is saved in [tag-reference.json](E:/Dev/ZenEngine/build/compute-gi/brixelizer-reference/tag-reference.json). The [manifest](E:/Dev/ZenEngine/build/compute-gi/brixelizer-reference/manifest.json) records immutable download URLs, byte lengths, and SHA-256 hashes for 31 files totaling 729,316 source bytes.
- The narrow inspection copy is under [build/compute-gi/brixelizer-reference](E:/Dev/ZenEngine/build/compute-gi/brixelizer-reference), ignored by Git. Root/SDK licenses and embedded AMD notices are preserved. Adapted source retains the applicable MIT notice.

All upstream links below address that commit. The acquisition verified the tag object type and commit before downloading immutable URLs; all saved files were then checked against their recorded lengths and hashes. `git check-ignore` confirmed the inspection directory is ignored. The source-pin subtask itself added no linked SDK target or runtime binding.

## Source entry points and Zen inputs

| Pinned source | Responsibility |
| --- | --- |
| [`ffx_brixelizer_raw.cpp`][raw] | Raw context/cascade resources, scratch layout, pass scheduling, dispatch arguments, capacity checks. |
| [`ffx_brixelizer.cpp`][host-update] | Higher-level static/dynamic cascade update, merge, and context-end completion. |
| [`ffx_brixelizer_cascade_ops.h`][cascade] | Triangle references, scans, allocation, distance emission, compression, AABB hierarchy. |
| [`ffx_brixelizer_context_ops.h`][context] | Pool lists, dirty bricks, merge, Eikonal distance completion. |
| [`ffx_brixelizer_brick_common.h`][brick] | Brick atlas addressing and field gradients. |
| [`ffx_brixelizer_trace_ops.h`][trace] | Hierarchy traversal, brick marching, limits and epsilon rules. |
| [`ffx_brixelizer_host_gpu_shared.h`][shared], [private shared definitions][private-shared] | Dimensions, packing and CPU/GPU ABI. |
| [`ffx_brixelizer_callbacks_glsl.h`][callbacks] | Actual GLSL buffer/image operations and descriptor contracts. |
| [`ffx_brixelizergi.cpp`][gi-host] | GI host responsibilities; not a replacement for Zen's existing lighting/cache ownership. |

Use [VoxelizerBase::BindVoxelScene](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/VoxelizerBase.cpp) as the existing GPU input contract: vertex, index, node-transform, material, UV, and stable triangle-record buffers. Scene geometry helpers load indexed vertices and apply the node transform on the GPU. Instance/triangle ranges do not require another CPU-expanded world-triangle scene. Temporary GPU triangle scratch during bounded work is distinct from a persistent CPU triangle representation.

## Current native construction slice

The [DistanceFieldScene owner](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/DistanceFieldScene.cpp), [shared construction constants](E:/Dev/ZenEngine/ZenCore/Include/Graphics/Shared/DistanceFieldScene.h), [scheduler constants](E:/Dev/ZenEngine/ZenCore/Include/Graphics/Shared/DistanceFieldScheduler.h), and [construction shader](E:/Dev/ZenEngine/Data/Shaders/VoxelGI/distance_field_build.glsl) implement bounded scene-field construction, partial static refresh, and local detail/dynamic volumes. All 16 current entry shaders compile with glslang for SPIR-V 1.3 and pass `spirv-val` for Vulkan 1.1; standalone outputs are retained under `build/compute-gi/brixelizer-execution/`. Compilation is separate from native synchronization, quality, memory and timing acceptance. Native coverage, tracing and lifetime acceptance is recorded in BrixelizerGIExecution.md.

| Concrete entry shader | Native responsibility and upstream relationship |
| --- | --- |
| `distance_field_snapshot.comp`, `distance_field_snapshot_vertices.comp` | Freeze mutable node/material/UV buffers and compact each source vertex to 16 bytes containing local position and alpha. Preserve arbitrary existing UV stride; use the frozen node transform during construction. Retain immutable index and triangle-record versions. This is Zen ownership work. |
| `distance_field_clear.comp` | Initialize private map, hierarchy, all scratch arrays, counters, control state, and indirect arguments. Full initialization makes inactive zero-work indirect phases safe. The owner clears the atlas to distance 1. Pending data is never exposed to tracing. |
| `distance_field_batch_clear.comp` | Reset per-batch counts, cursors, and job/reference/active counters. Preserve allocated-brick count, failure flags, and first-failure site/triangle/cell/sample diagnostics. |
| `distance_field_jobs.comp` | Transform committed triangle vertices and classify root coverage for the full triangle batch. Opaque/proven-covered roots emit immediately; proven-transparent roots finish without work. Partial roots enter a compact continuation queue. Each 16-byte region job contains triangle ID, packed region coordinates, quadtree node code and depth; a patch spans at most 16³ regions at side 64. |
| `distance_field_coverage.comp` | Process at most 64 queued partial triangles and 32 refinement nodes/triangle per dispatch. Persist the depth-first stack, visited count and status in 41 uints/initial-batch triangle, plus one queue uint. Prepare skips up to 1,024 completed queue entries before dispatching; fully opaque scenes omit the phase. |
| `distance_field_prepare.comp`, `distance_field_advance.comp` | Separate ordered GPU dispatches prepare indirect arguments, then advance phase/cursors after work completes. The GPU controls triangle batches, job slices, active-cell slices and hierarchy levels. Pre-allocation capacity failures halve the triangle quota and retry the same range; a single-triangle failure remains fatal. The former `distance_field_prepare_jobs.comp` wrapper is retained but unused by this scheduler. |
| `distance_field_count.comp` | One 64-thread group tests one region. Count patch/cell references using the engine's triangle-box SAT with a full-brick halo. |
| `distance_field_scan.comp`, `distance_field_scan_sums.comp` | Exclusive scans in 256-cell blocks, then one workgroup scans at most 1,024 block sums. Record total reference count and overflow. |
| `distance_field_allocate.comp` | Add block offsets, allocate atlas bricks for newly referenced cells and append active cells. Atlas, total-reference and per-cell-reference limits are independent failure conditions. |
| `distance_field_compact.comp` | Repeat the same patch/cell test and compact job IDs by cell. The initial clear defines complete scratch contents; later prefix consumers are readonly. |
| `distance_field_emit.comp` | One 8³-thread group evaluates analytic distance to every referenced patch at each sample, using finite segment projections and a normalized plane test. Write R8 distance directly and take the minimum with previous batches. Stage 256 patches at a time, streaming up to 1,024 references/cell; scheduler limits active cells/dispatch. |
| `distance_field_hierarchy.comp` | Build a leaf copy and successive OR-reduced occupancy/availability levels. This differs from AMD's packed AABB tree. Publish only after complete atlas/hierarchy work and successful completion/status observation. |

The direct distance emitter replaces AMD's small-triangle approximation, half-packed edges, squared-distance atomic minima, compression, and Eikonal completion for supported geometry. A brick stores distance clamped to its extent `B`. Its reference list includes triangles intersecting the brick box expanded by `B`, plus a rounding guard. Every triangle within distance `B` of a sample must intersect that box; omitted triangles cannot lower the clamped result. This larger halo costs more references and active bricks than AMD's near-surface emission. It is necessary for the direct formula, not evidence of a speedup. Minimum accumulation across batches preserves the R8 minimum because quantization is monotonic.

The field-specific [distance helper](E:/Dev/ZenEngine/Data/Shaders/VoxelGI/distance_field_distance.glsl) replaces the region-determinant formula after a full-scene diagnostic reported a nonfinite distance at triangle 892812, cell 71024, sample 120. CPU reconstruction gives duplicate second and third vertices. Exact-zero-area faces remain excluded using explicit non-contracted cross products; no area-size threshold discards valid slivers. Nonzero normals are scaled before their squared norm, and zero-length edge projections remain finite. Disassembly confirms `NoContraction` on the cross arithmetic. The native checkpoint passed 24 tests including duplicate-face exclusion and a finite face with an aspect ratio above one million; the subsequent empty-scene checkpoint passed 28 cases.

Complete New Sponza construction then succeeded with 60,419 bricks in a 512³ pool: 136,464,688 bytes of native persistent allocation and 108,912,464 bytes of scratch. That older CPU-gated scheduler required 73,194 submitted graphs and 25.7805 seconds of headless preparation. These are coarse single-level construction baseline measurements, not lantern quality or GI frame-time acceptance. They do not measure the new scheduler/alpha path.

The native alpha extension is separate from AMD's geometry-only kernels. It combines existing material factor, interpolated vertex alpha, authored UV set/transform, optional spatial-atlas mapping and bound-sampler base-level coverage. A compact metadata buffer supplies sampler slot count, subtexel precision and supported LOD-zero/magnification filter flags. Texture gather preserves actual wrap addressing. Clipped bilinear-cell bounds or nearest-texel bounds classify patches as covered, transparent or uncertain, including arithmetic/filter precision guards. An uncertain retained patch needs a covered witness on the original triangle and a bound of one quarter sample pitch from every retained point to that witness. Nearby witnesses outside the patch can resolve fixed arithmetic fringes only when patch radius plus witness distance satisfies that same world-distance bound. Unresolved patches subdivide to bounded depth/work limits; unresolved regions at those limits publish unavailable cells. This bounds extra retained geometry relative to actual coverage; R8/interpolation/tracing errors still require quality tests. No nearest-transparent-point rejection is used. Native vertex-alpha and textured-card acceptance remain required.

The initial 8,192-node monolithic refinement failed the transformed 8×8 texture-card test at its work limit. The continuation path permits 65,536 cumulative nodes/triangle at depth at most 12 while submitting at most 2,048 node evaluations/dispatch. This separates cumulative completion work from a dispatch's work bound. It does not establish a 2 ms frame limit without native timings and an aggregate per-frame dispatch budget. The per-cell reference cap remains 1,024. The [CPU arithmetic regressions](E:/Dev/ZenEngine/tools/test_distance_field_coverage_arithmetic.py) reproduce unsafe UV-cancellation and constant-alpha cutoff shortcuts, then verify corrected enclosure over 80,020 separate/fused float32 evaluations. They complement, not replace, native coverage fixtures.

The first continuation run completed refinement but exceeded per-cell references for a single source triangle. Accepted patches now stream through Count/Allocate/Emit in windows of at most `min(referenceCapacity/64, referencesPerCell, jobCapacity)` region jobs. A 4³-cell region contributes at most 64 references and at most one to any cell, so both reference limits hold by construction. A patch that spans multiple windows keeps its next region index with its DFS state and resumes without another refinement visit. Streaming clears reference scratch while preserving that state and queue; atlas distance minima accumulate across windows. Mixed opaque work flushes before partial coverage, and original-triangle progress advances only after the final window. The native mixed opaque/partial-coverage streaming fixture passes in all four execution modes.

The control buffer has 32 words, separate from 19 counter/diagnostic words. Only job/reference/cell-reference capacity failures before allocation are retryable. Atlas overflow, invalid geometry and unsupported coverage remain explicit failures. Unresolved native-sampler arithmetic or exhausted refinement marks the intersected cells unavailable; it never emits a fabricated surface or certifies them as empty. Counter 18 records those cells. Each prepare/work/advance chain declares storage and indirect-read dependencies. CPU status polling observes progress/completion instead of selecting the next phase.

The native map uses `0` empty, `0xffffffff` unavailable and `atlasIndex + 1` occupied. An occupied entry's high bit marks bricks whose stored samples cannot meet the hit band; traversal skips them, while gradients can still read their atlas samples. Its bit hierarchy includes a full leaf copy, approaching `8/7` of map storage; map plus hierarchy approaches `15/7` of map storage. These differ from AMD's encodings and tree sizes below. Partial static rebuilds retain unaffected atlas/map data and recycle dirty bricks. A separate mandatory local volume holds dynamic geometry. Local lamp volumes and the dynamic volume share a pool, with a hierarchy over their authority boxes; this is not a triangle BVH. Published and retired resources count against the field budgets until their GPU users complete.

## Pinned construction sequence and mapping

The authoritative sequence is [raw scheduling][raw] around lines 1292–1325, 1512–1535 and 1582–1619, together with [high-level update][host-update]. Compression precedes merge/tree construction; context-end Eikonal follows them. Consumers wait for complete atlas and hierarchy, not merely brick allocation.

| Upstream work | Zen responsibility |
| --- | --- |
| Instance/job invalidation and voxel clearing | Snapshot committed geometry versions, select bounded jobs and maintain private pending/unavailable data. Never clear a published generation in place. |
| `Voxelize` | Fetch/transform GPU scene triangles, emit bounded region work and references; preserve failures on exhaustion. |
| `ScanReferences` | Allocate bricks, scan references and active work; distinguish completed empty cells from failed/unavailable cells. |
| `CompactReferences`, `ClearBrickStorage` | Compact cell references and initialize scratch before any consumer. |
| `EmitSDF` | Emit geometry distances with material-aware native adaptation. The current direct emitter deliberately differs as described above. |
| `CompressBrick` | Convert temporary distances to normalized R8 and compute/free brick bounds in AMD. Native direct emission removes this separate pass; never free failed/partial coverage as empty. |
| `MergeCascades`, `PrepareMergeBricksArgs`, `MergeBricks` | Optional static/dynamic maps must share grid state; union bounds and minimum distances. Do not accumulate dynamic geometry into retained static data. |
| `BuildTreeAABB`, iterations 0, 1, 2 | Ordered hierarchy reduction. Unknown regions must remain discoverable. Native OR hierarchy is a deliberate replacement. |
| `CollectDirtyBricks`, `PrepareEikonalArgs`, `Eikonal` | Complete missing reference-atlas distances before publishing. Current direct full-halo distances need no missing-seed propagation. |

Indirect producers require storage-write to indirect-read dependencies. Scratch/image producers need accurate following read/write declarations. Adjacent host calls are not a barrier. Upstream `maxBricksPerBake`, `triangleSwapSize` and `maxReferences` bound storage, not dispatch duration. Native phase/work limits still need responsiveness measurements; no region is complete before all its contributing work finishes. Rejected submission never advances publication or releases its owners.

## Resource layout and memory

These are pinned AMD dimensions, not accepted Zen presets. Atlas shape changes require matching allocation, addressing, capacity and tracing changes.

| Resource | Upstream layout | Native constraint |
| --- | --- | --- |
| Distance atlas | 512³ R8_UNORM, 128 MiB; 8³ samples/brick and 64³ brick slots. | Storage writes and filtered sampled reads. Logical brick allocation does not require Vulkan sparse residency. |
| Cascade map | 64³ uint IDs, 1 MiB/cascade. Empty `0x00ffffff`, unavailable `0xffffffff`. | Keep empty and unavailable distinct; native ID encoding differs above. |
| Cascade AABB tree | 16³ packed uint bottom nodes plus `(4³ + 1)` pairs of float3, 17,944 bytes/cascade. | Fixed dimensions/packing are coupled to the 64³ map. |
| Brick bounds | One packed uint/brick, 1 MiB at full capacity. | Three 3-bit minima plus three 3-bit maxima packed at bit 9; traversal expands inclusive maximum. |
| Brick ownership | One uint voxel/cascade ID per brick, 1 MiB. | Low 24 bits cell, high 8 bits cascade; native generations additionally prevent stale references. |
| Pool lists/counters | Free, clear, dirty/Eikonal lists and Eikonal counters each one uint/brick; merge list two/brick; six context counters. | Budget all coexisting/retired representations, beyond atlas/maps/cache. |
| Indirect arguments | Eleven 16-byte slots, including reserved first slot. | Exact offsets and indirect usage; native layout may differ consistently. |
| Temporary samples | 512 uint samples = 2,048 bytes/baked brick. | Include batch limits and in-flight ownership. |
| References | Uncompacted voxel/triangle/local-index record 12 bytes, compact ID 4 bytes. | At least 16 bytes/reference coexist upstream, before scans/scratch. |
| Triangle scratch | Small triangle center float3; large triangle float3 base and half-packed edges, 24 bytes. | Native retains full precision; approximations must pass lantern/scale fixtures. |
| Build arrays | Failure, reference-count/scan, stamp/job scans, brick clear/compression/offset arrays. | Use [scratch partition calculation][scratch], including alignment and optional debug storage. |

The four upstream cell-sized failure/count/scan arrays alone cost 4 MiB; three brick-sized scratch arrays add 3 MiB. Triangle data, references, 2,048-byte/baked-brick storage, jobs, scans and alignment must fit the native starting 128 MiB scratch budget. Static/dynamic/merged and replacement generations can coexist. A 128 MiB atlas is not a complete 128 MiB implementation.

## Distance, addressing and traversal contracts

Let `B` be world brick extent, and `i` a sample index 0…7.

- Sample spacing is `B/7`. Temporary brick minimum is half a sample before the cell minimum; evaluation adds half a sample, giving `cellMin + i*B/7`. Neighbors share boundary positions. Reference geometry bounds inflate by `B/7`; native direct distance uses the larger full-`B` halo above. [Emission/bounds][cascade]
- Atlas brick index is `brickID & 0x00ffffff`. Unflatten over `atlasDimension/8`, then multiply by 8. Local `[0,1]` interpolates between texel centers `offset+0.5` and `offset+7.5`; avoid filtering adjacent bricks. [Addressing][brick]
- Reference emission stores squared distance in sample-coordinate units scaled by `1/16`. Compression applies `sqrt(abs(value))*4/7`, clamped to `[0,1]`: unsigned world distance divided by `B`. Sample times `B` advances the world ray. [Compression][compression]
- R8 quantization step is `1/255`. Reference traversal subtracts `FFX_BRIXELIZER_TRAVERSAL_EPS` before stepping and accepts a hit when adjusted value is below the same epsilon. This is not `sample==0`. The including shader supplies epsilon; `trace_ops.h` defines no universal value. [March][trace]
- Hierarchy packing uses eighth-cell coordinates and Eikonal propagation uses `1/8`, despite seven atlas intervals. Preserve paired behavior initially; do not globally replace every 8 with 7. [Hierarchy][hierarchy], [Eikonal][context]
- Unsigned-distance gradients are not stored oriented triangle normals. Require usable gradients or compatible surface data. AMD's NaN fallback points away from brick center; this does not establish Zen facing/material validity. [Gradient][brick]

Native tracing must return hit, covered miss or unavailable with a reason. AMD's public boolean alone does not satisfy that contract:

1. Outer traversal limits hierarchy work to 32 iterations/cascade and retries coarser cascades on unavailable data or limits, eventually returning false. Native miss requires proving the remaining segment covered, including any coarser replacement.
2. Brick marching has a separate eight-sample limit. Exhaustion inside a brick must continue under a bounded policy or report unavailable, never skip the unresolved remainder as empty.
3. Reference traversal skips a cascade unless the current cursor is strictly inside it. Native scene-wide rays must intersect/clamp field entry bounds, including outside origins, boundary/parallel/grazing cases.
4. Unknown cells contribute full-cell bounds. Preserve discoverable availability; this does not authorize reporting unknown cells as solid hits.
5. Coverage contains all committed occluders independently of radiance-cache coverage. An external hit blocks sky/light even without cache radiance. Beyond complete scene bounds may be a covered miss; missing internal regions may not.

The distance atlas contains no triangle/material/UV/alpha identity. Stock geometry-only kernels do not meet masked-material coverage requirements. Keep off-screen/occluded alpha cards in the acceptance fixtures.

## Capabilities and lighting integration

SDK documentation advertises HLSL CS 6.6, but pinned code also has GLSL callbacks and Vulkan entries. Qualify actual compiled SPIR-V and enabled features, not a shader-model label.

| Observed requirement | Native qualification |
| --- | --- |
| Compression/Eikonal use 512 invocations; others 32/64/256. Eikonal has 512 shared floats. | Query workgroup limits and compiled shared-memory usage. 512 threads are not guaranteed by conservative engine defaults. Smaller groups require correct sample loops/barriers. |
| R8 storage image3D plus filtered sampled texture3D. | Query storage/sampling/linear-filter support, dimensions and required enabled features, including storage extended formats where applicable. [Vulkan formats](https://docs.vulkan.org/spec/latest/chapters/formats.html), [feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html). |
| Raw SDK requires nonuniform storage-buffer-array indexing for external geometry arrays. | Zen's combined geometry buffers avoid that backend design. Qualify descriptors actually used; material texture requirements remain. |
| Traversal subgroup minimum and broadcast-first. | Check operations or adapt traversal; do not assume wave64, which pinned host scheduling deliberately does not force. |
| 32-bit atomics for counters, lists and packed distance minima; packed half edges. | No 64-bit atomic requirement is established. Packed-half storage is separate from FP16 arithmetic permutations. |
| GPU indirect dispatch, storage buffers, sampled/storage images. | Verify device limits and all RDG access/barrier declarations; compilation does not prove synchronization. |

Optional `FFX_HALF`, `FFX_LONG` and `FFX_WAVE` branches in upstream core helpers can introduce unrelated capabilities if imported wholesale. Retain needed operations and validate modules. Device/quality/performance qualification remains milestone 2 work.

| Upstream GI responsibility | Zen destination |
| --- | --- |
| Probe rays and hit-radiance lookup | Existing ScreenProbeGather probe/per-pixel variants use common field tracing and compatible cache. |
| Screen-derived radiance update | Preserve scene-wide direct/emissive injection for unseen surfaces. Avoid double-counting analytic local light, cached direct or environment terms. |
| History and filtering | Existing gather/pixel history owners and accepted-submission publication; invalidate when effective provider or generation changes. |
| Specular/pretrace/world irradiance cache | Integrate common tracer with existing specular/reflections. AMD replacement irradiance/pretrace systems remain deferred. |
| Debug outputs | Expose distance, normals, unavailable data, sample/iteration counts and hierarchy in existing diagnostics/captures. |

Prove the static field on analytic fixtures before incremental updates and GI consumers. The [reference-math experiment](BrixelizerGIReferenceMathExperiment.md) measures sampling, phase, tolerance and thin-slot tradeoffs and identifies a possible local-detail range. It does not qualify alpha, native GPU cost or complete New Sponza image quality.

[shared]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_host_gpu_shared.h
[private-shared]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_host_gpu_shared_private.h
[raw]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/brixelizer/ffx_brixelizer_raw.cpp
[host-update]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/brixelizer/ffx_brixelizer.cpp#L338
[cascade]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_cascade_ops.h
[context]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_context_ops.h
[brick]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_brick_common.h
[trace]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_trace_ops.h
[callbacks]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_callbacks_glsl.h
[gi-host]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/brixelizergi/ffx_brixelizergi.cpp
[scratch]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/src/components/brixelizer/ffx_brixelizer_raw.cpp#L757
[compression]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_cascade_ops.h#L1434
[hierarchy]: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_cascade_ops.h#L1327
