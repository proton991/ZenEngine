# RDG renderer verification

Baseline verified on 2026-09-07 before the implementation phases below. Both renderer modes execute and can be switched in either direction, but the rendering pipeline is not synchronization-clean. The PBR graph's G-buffer producer/consumer setup appears correct in the tested frames. The voxel path has missing initialization and unused GI work, and the shared RDG compiler still has a reader-visibility gap.

Implementation status: phase 1 passed the user's verification after rebuilding. After phase 2, the user reported no Vulkan validation errors but continued RDG `missing_barrier` diagnostics, then authorized phase 3. After phase 3, the user supplied compute-mode diagnostics for redundant initial buffer barriers and the known emissive-input gap, then authorized fixes. The user reported **all ok** after phase 4 and the initial-buffer-barrier correction. The user reported **all ok** after phase 5 and the staging-source correction, then authorized phase 6. Phase 6 is implemented and awaiting the user's build/runtime verification and refreshed live baseline. Geometry-specific runtime coverage has not been separately reported. Historical observations and counts below describe the baseline, not the modified code.

## Coverage and evidence

- Built `scene_renderer_demo` in `build/arm64-apple-clang-debug`.
- Ran the real renderers with Vulkan synchronization validation enabled on Apple M3 Pro / MoltenVK. This device selects `ComputeVoxelizer`; it does not support the geometry-shader voxelizer or dedicated compute/transfer queues.
- Used a temporary copy of `SceneRendererDemo.cpp` with a bounded `Run()` loop. It called the same `RendererServer::SetRenderOption()` used by keys **1 = voxel** and **2 = PBR**, before dispatching each frame. Keyboard event delivery itself was not tested.
- Ran two fresh processes, 12 frames each: voxel → PBR → voxel and PBR → voxel → PBR, with four frames per segment. Captured every frame, including transfer-node detail. Both processes exited normally and reported no tracked memory leaks.
- Ran `RenderCoreTest.MetricsExposeReaderVisibilityGapInCurrentRDG` and all six `RDGBarrierValidator` tests: **7/7 passed**. The former deliberately expects the existing compiler defect to be detected; its passing result does not mean that defect is fixed.
- Review harness and full logs are local artifacts in `/tmp/zen-rdg-review/`: `SceneRendererReview.cpp`, `renderer_review`, `voxel-pbr-voxel.log`, and `pbr-voxel-pbr.log`.

The harness was linked against the normal ZenCore library. It changed only loop duration, mode selection, and metrics capture settings. The validation environment was:

```sh
VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT /tmp/zen-rdg-review/renderer_review
ZEN_RDG_REVIEW_PBR_FIRST=1 VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT /tmp/zen-rdg-review/renderer_review
```

The normal demo enables `VK_LAYER_KHRONOS_validation`, but its `VkValidationFeaturesEXT` list in `VulkanContext.cpp` enables only debug printf. Synchronization validation is disabled by default in the installed SDK; enabling the validation layer alone does not enable this check. See the [validation layer settings](https://vulkan.lunarg.com/doc/view/latest/linux/khronos_validation_layer.html).

After the initial review, the **unchanged `bin/scene_renderer_demo` executable** was run twice for ten seconds each, using the default voxel mode. The runs used the same environment with synchronization-enabling environment variables removed; the second run added only `VK_LAYER_VALIDATE_SYNC=1`:

| Stock executable run | Vulkan error messages | Synchronization hazard messages |
| --- | --- | --- |
| Default validation settings | 0 | 0 |
| `VK_LAYER_VALIDATE_SYNC=1` | 10 | 10, capped by validation's duplicate limit |

Both runs reached frame execution and were terminated by the test supervisor after ten seconds. Their logs are `/tmp/zen-rdg-review/stock-demo-default.log` and `/tmp/zen-rdg-review/stock-demo-sync-enabled.log`. This reproduces the user's observation of no validation error during a normal launch, and confirms that the reported presentation hazard depends on explicitly enabling synchronization validation, not on the temporary renderer-switching harness. Reproduce the additional check from the repository root with:

```sh
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

This spelling was verified with the installed Vulkan SDK 1.4.341.1. It avoids the deprecated `VK_LAYER_ENABLES` setting used in the original harness runs. Validation is an opt-in diagnostic for these runs; no application defaults were changed.

This is a short correctness run, not a performance benchmark or image-quality comparison. Resize, scene replacement, long-running resource reuse, geometry voxelization, and multiple queue families were not exercised.

## Observed graphs

| Frame type | Nodes: graphics / compute / transfer | Resources: imported / transient | Barrier calls | Buffer / texture transitions | Internal texture transitions |
| --- | --- | --- | --- | --- | --- |
| Cold voxel startup | 106 / 8 / 104 | 103 / 0 | 230 | 23 / 401 | 12 |
| Cold PBR startup | 106 / 0 / 102 | 88 / 6 | 208 | 6 / 401 | 0 |
| Steady PBR, including switch back | 3 / 0 / 0 | 86 / 6 | 3 | 0 / 16 | 0 |
| First voxel frame after PBR startup | 3 / 8 / 2 | 98 / 0 | 25 | 17 / 19 | 12 |
| Steady voxel, including switch back | 3 / 2 / 2 | 95 / 0 | 19 | 1 / 13 | 12 |

Internal texture transitions are a separate counter; barrier calls include internal calls. Startup includes environment preprocessing and one-time voxelization. The first PBR frame after voxel startup used 19 texture transitions, then settled to 16.

PBR executes `SkyboxDraw → OffScreen → SceneLighting`. `OffScreen` declares five transient color outputs and one transient depth output; `SceneLighting` resolves and samples all six by their output tags. Producer registration, attachment-to-sampled transitions, and reuse on return to PBR were present in the captured graphs. See [DeferredLightingRenderer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp).

Steady voxel executes `SkyboxDraw → evsm → shadowmap_copy → shadowmap_mipmaps → VoxelDraw2 → ResetVoxelTextureComp → VoxelInjectRadianceComp`. The reset here clears radiance. The extra voxelization/reset/indirect-preparation passes appear only when voxelization is needed. Both switch sequences retained these expected schedules; no ordering diagnostic was emitted.

## Findings

### P1: Both modes have an acquire-to-layout-transition synchronization error

**Runtime confirmed with synchronization validation explicitly enabled.** Each original harness process logged `SYNC-HAZARD-WRITE-AFTER-READ` ten times, reaching validation's duplicate-message limit. Reports occurred in both renderer modes. The unchanged stock executable also reproduced the error with this check enabled, while its default validation run reported no errors. The hazard is a swapchain image layout transition writing an image previously read by `vkAcquireNextImageKHR`.

`VulkanViewport::PrepareForPresent()` waits on the acquire semaphore at `TRANSFER`. `CopyBackBufferToSwapchainImage()` batches the acquired image's `UNDEFINED → TRANSFER_DST` transition with the backbuffer's `COLOR_ATTACHMENT → TRANSFER_SRC` transition. `ExecuteImageBarriersOnly()` infers a source mask of `TOP_OF_PIPE | COLOR_ATTACHMENT_OUTPUT` from the old layouts. That transition is not properly chained after the transfer-stage semaphore wait.

Use explicit acquire-transition synchronization whose source scope chains with the wait, or move the wait early enough to cover the transition. Vulkan's [semaphore/barrier examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html) describe the required relationship. This work happens after RDG execution, so the RDG snapshot cannot detect it.

Sources: [VulkanViewport.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), `CopyBackBufferToSwapchainImage` and `PrepareForPresent`; [VulkanSynchronization.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSynchronization.cpp), `ExecuteImageBarriersOnly`.

### P1: Compute voxelization does not produce two GI inputs

**Confirmed by resource and shader inspection.** `ComputeVoxelizer::UpdatePassResources()` binds only the albedo volume to its reset and voxelization passes. The base class also allocates normal and emissive volumes, but this path never initializes or voxelizes them.

`VoxelGIRenderer` nevertheless binds both volumes to `VoxelInjectRadianceComp`. The shader reads their contents for occupied albedo voxels. A layout transition cannot supply missing contents. The first voxel frame reports `unknown_import_state` for `voxel_emissive`; normal's read/write declaration masks its initial read from this diagnostic.

Implement the missing outputs and their reset passes, or gate GI until the selected voxelizer provides valid inputs. Importing a texture into RDG currently does not assert that its contents have been initialized.

Sources: [ComputeVoxelizer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/ComputeVoxelizer.cpp), `UpdatePassResources`; [VoxelGIRenderer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp), `UpdatePassResources`; [inject_radiance.comp](../Data/Shaders/VoxelGI/inject_radiance.comp), the normal/emissive `imageLoad` calls.

### P1: The shared RDG compiler loses writer visibility when reader usage expands

**Confirmed in the existing mock-RHI regression and compiler inspection.** For `transfer write → transfer read → vertex-buffer read`, the first barrier targets transfer reads. The later read/read pair skips a barrier even though vertex-attribute access has not been made visible to that writer.

`NeedsBarrier()` considers read/read accesses barrier-free when texture layout is unchanged. `AttachGraphBarriers()` merges subsequent reader stages/usages only into the state used by a future barrier; it does not widen the already-created writer-to-reader barrier. Execution also skips read/read buffer transitions. The tracker retains the latest access rather than the last writer and its visibility coverage, including across graph executions.

Track writer visibility until all subsequent reader access types are covered, or precompute the complete reader group and target it from the writer's barrier. Avoid solving this with an unconditional barrier for every read. Vulkan's [synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) defines the execution and memory scopes that must cover the consumers.

The voxel-first startup reported missing-barrier candidates when uploaded resources reached compute consumers after graphics consumers. Those reports warrant investigation, but the exact count is inflated by the metrics issue below. The live Vulkan logs did not separately confirm these candidates.

Sources: [RenderGraph.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp), `NeedsBarrier`, `AttachGraphBarriers`, and `EmitCompiledNodeBarriers`; [RenderCoreTests.cpp](../ZenSamples/RenderCoreTest/RenderCoreTests.cpp), `WriterVisibilityReachesLaterReaderUsagesInOneGraph` (formerly `MetricsExposeReaderVisibilityGapInCurrentRDG`).

### P2: The new metrics validator overstates some missing barriers

**Confirmed by diagnostic and stage-mask inspection.** `RDGMetrics::BeginNode()` assigns `node->selfStages` to every resource access. This asks vertex buffers, indices, and sampled images to satisfy depth-test stages merely because the same node uses depth testing. An indirect compute node likewise asks all its textures to satisfy `DRAW_INDIRECT`.

Cold PBR startup reports three missing barriers at `SkyboxDraw` for this reason. Cold voxel startup reports 157: three at skybox and 77 at each voxelization dispatch. These are candidates, not 157 proven GPU hazards. Conversely, the validator resets writer history at graph boundaries, so zero reports after switching do not prove complete visibility.

Use per-resource stages derived from the actual binding/access, and account for Vulkan execution-scope rules when comparing masks. Preserve enough writer history to validate across graphs. Until then, use Vulkan synchronization validation alongside RDG metrics.

Source: [RDGMetrics.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RDGMetrics.cpp), assignment to `next.stages` in `BeginNode`.

### P2: Voxel rendering declares an unnecessary storage-buffer write

`voxel_vis.frag` declares `InstanceColorBuffer` without `readonly`, although its only use reads a color. RDG uses reflection's `writable` flag and therefore treats the draw as a writer. The steady voxel graph emits a buffer transition on `VoxelDraw2` every frame for this otherwise unchanged buffer.

Mark the shader buffer `readonly`, retain the initialization-to-draw dependency, and verify that subsequent draw-to-draw transitions disappear. Other shared compute declarations should be audited with the same distinction between actual writes and conservative shader declarations.

Sources: [voxel_vis.frag](../Data/Shaders/VoxelGI/voxel_vis.frag); [RenderGraph.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp), storage-buffer binding access declaration.

### P2: GI radiance has no rendering consumer

`RendererServer` appends voxel drawing before GI. Compute voxel drawing uses instance colors derived from albedo; the geometry draw also binds albedo. The GI radiance texture is only allocated, reset, written, and destroyed. Its six mipmap volumes are allocated but unused, and GI has no graphics pass.

Consequently, the per-frame radiance reset and injection do not contribute to the displayed image. The shadow render/copy/mipmap chain currently feeds this unused GI result. If key 1 is intended as albedo visualization, gate that work; if it is intended to show GI, add an actual radiance consumer and its dependencies. Reordering the existing draw alone is insufficient because it does not read radiance.

Sources: [RendererServer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RendererServer.cpp), `DispatchRenderWorkloads`; [VoxelGIRenderer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp), texture allocation and graph construction.

### Geometry voxelization also needs an initialization audit

**Source-only finding; unsupported on the test GPU.** `GeometryVoxelizer::BuildRenderGraph()` appends voxelization without first clearing its volumes. The fragment shader's atomic averaging reads existing albedo, normal, and emissive values, which require a known starting value. Base texture creation supplies storage but no clear. Add explicit initialization before first voxelization and reset before rebuilding the volume, then validate this branch on a geometry-shader-capable device.

Sources: [GeometryVoxelizer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/GeometryVoxelizer.cpp), `BuildRenderGraph`; [VoxelizerBase.cpp](../ZenCore/Source/Graphics/RenderCore/V2/VoxelizerBase.cpp), `PrepareTextures`; [voxelization.frag](../Data/Shaders/VoxelGI/voxelization.frag), atomic averaging functions.

The original verification added this report only. Follow-up implementation and verification status are tracked below.

## Evaluation and implementation phases

The findings are actionable, but the original report does not define implementation phases; `P1` and `P2` indicate severity. Source review confirms the reported acquire wait/barrier mismatch, reader-visibility gap, whole-node metric stages, missing voxel inputs, conservative shader access, and unused radiance work. The baseline Vulkan results remain historical evidence until the modified executable is built and rerun.

Fix presentation first because it affects both modes and can be isolated from RDG changes. Correct the validator before using its counts to judge the compiler fix. Treat volume initialization and writer visibility as separate requirements: valid layouts and barriers do not initialize data. Preserve the distinction between compute-path runtime coverage and geometry-path source review.

Complete one phase at a time, then stop for the user's compile/build/runtime verification before starting the next phase.

| Phase | Scope | Acceptance checkpoint | Status |
| --- | --- | --- | --- |
| 1 | Chain the acquired swapchain image's layout transition after the acquire semaphore wait. | Build the demo; run both modes and switch in both directions with synchronization validation enabled. The reported acquire-related `SYNC-HAZARD-WRITE-AFTER-READ` must disappear. | User reported no errors after rebuilding; passed |
| 2 | Correct per-resource stages in RDG metrics and preserve writer history across graph executions, accounting for resource lifetime and external uploads. | Add validator regressions for mixed bindings, indirect dispatch, and cross-graph reads. Eliminate false stage requirements while retaining detection of the existing reader-visibility defect. | Mock-RHI tests passed; user reported no Vulkan validation errors, with RDG missing-barrier diagnostics remaining; authorized phase 3 |
| 3 | Track the last writer and reader visibility coverage in RDG compilation/execution and persistent resource state. | Change the known-defect regression to require correct emitted synchronization. Cover new reader usages/stages within and across graphs, unchanged readers, and a subsequent writer; avoid unconditional read/read barriers. | 45 mock tests passed; user supplied compute-mode checkpoint and authorized phase 4 |
| 4 | Initialize/reset all voxel volumes before first use and revoxelization; produce compute normal/emissive inputs or explicitly gate injection when the voxelizer cannot provide them. Audit geometry atomic accumulation against the same initialization contract. | Verify cold startup, mode switches, and forced revoxelization. Every GI input read must have a valid producer. Geometry runtime verification requires a capable GPU and remains pending until exercised there. | 48 mock tests passed; user reported all ok; geometry-specific runtime coverage remains unreported |
| 5 | Mark `InstanceColorBuffer` readonly and audit other shader declarations against actual accesses. Preserve actual writes, including injection's normal-alpha update. | Rebuild shaders; confirm reflection reports the color buffer as read-only, its initialization dependency remains, and steady draw-to-draw buffer transitions disappear. | 51 RenderCore tests and 7 reflection/Vulkan utility tests passed after the staging-source correction; user reported all ok |
| 6 | Resolve the unused GI work. The default scope preserves the current albedo visualization and gates unused radiance/shadow work. A displayed radiance consumer is a separate rendering feature to agree on before implementing. | For albedo visualization, preserve the displayed result and remove passes/allocations that have no remaining consumer. If radiance rendering is selected, verify a real graphics consumer and its dependencies. Refresh the baseline measurements afterward. | Implemented; 52 RenderCore tests and 7 reflection/Vulkan utility tests passed; user runtime verification and refreshed live baseline pending |

### Phase 1 implementation

`VulkanViewport::CopyBackBufferToSwapchainImage()` now uses the existing explicit-stage `VulkanPipelineBarrier::Execute()` for its initial barrier batch:

- Source stages: `COLOR_ATTACHMENT_OUTPUT | TRANSFER`.
- Destination stages: `TRANSFER`.
- The backbuffer barrier retains color-attachment-write → transfer-read access.
- The acquired image retains `UNDEFINED → TRANSFER_DST_OPTIMAL` and zero source access → transfer-write access.

The source `TRANSFER` scope chains the layout transition after the `TRANSFER` semaphore wait in `PrepareForPresent()`. `COLOR_ATTACHMENT_OUTPUT` retains the dependency on the rendered backbuffer in the same batch. This follows the [Vulkan synchronization dependency-chain rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-dependencies). The shared layout-inference helper, acquire wait stage, copy/blit commands, and final present transition are unchanged.

Validation: source review and whitespace/diff checks passed. The user's first retry ran an executable older than the patch; Ninja's dry run confirmed that the viewport object, library, and executable still required rebuilding. After rebuilding, the user reported **no errors** and authorized phase 2. Detailed mode/resize coverage was not separately reported, and this does not establish that the complete renderers are synchronization-clean.

From the repository root, using the existing configured build:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

Let the default voxel mode render, press **2** for PBR, then **1** to return to voxel; repeat the switches after several steady frames in each mode. Confirm the expected image in both modes and the absence of the specific acquire-related hazard. Also resize in both modes to exercise swapchain recreation; this extends the original coverage and any new diagnostics should be recorded separately. A clean default-validation run alone is insufficient. RDG missing-barrier candidates and the known-defect unit test remain expected until their later phases.

### Phase 2 implementation

Implemented on 2026-09-08. Access declarations now carry resource-specific pipeline stages. Shader bindings use reflection's stage flags; vertex/index buffers, indirect arguments, attachments, and transfer operations retain their own usage stages. Multiple bindings of one resource merge this diagnostic metadata. Compiler scheduling and emitted barrier policy remain unchanged.

The validator distinguishes Vulkan execution ordering from memory access scope: logically earlier/later stages can satisfy execution dependencies, while availability and visibility require explicit memory-stage coverage. It checks merged access types at their supported stages and combines compatible barriers targeting separate reader stages. See the [Vulkan pipeline-stage scope rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-stages).

Writer visibility now persists across frame and transfer graphs. With logging and validation enabled, all executions update validation history, including unsampled uploads and readers; only sampled graphs publish diagnostics, counters, labels, and timings. Unsampled execution leaves the last snapshot unchanged. An earlier graph's node ID is reported as `previous_node=-1` rather than being confused with an ID in the current graph.

External tracker updates, invalidation, and resource retirement discard the affected validation history. Staging-buffer eviction and shutdown now notify the device's tracker as well. Disabling validation or collection clears history; re-enabling seeds from current import state and cannot recover unobserved writers. Tracking adds CPU work between reports while validation is enabled; `validate=false` retains sampled counters without continuous replay.

Validation: built `RenderCoreTest` and ran all **43 tests**, with **43 passed** and no tracked memory leaks. The suite includes mixed depth/geometry bindings, indirect compute, separate graphics/compute barriers, unsampled uploads, cross-graph reads, external state replacement/invalidation, and a validation-disablement gap. The original known-defect regression still passes by detecting the compiler defect; a new cross-graph regression also expects that defect to be detected. Full test output: `/tmp/zen-rdg-phase2-tests.log`.

User checkpoint, from the repository root:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo RenderCoreTest
./bin/RenderCoreTest
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

Verify the displayed image and switch **1 → 2 → 1** after several frames in each mode. Keep synchronization validation enabled. The phase 1 acquire hazard should remain absent. The old whole-node stage false positives should disappear; genuine reader-visibility candidates remain until phase 3, and tracking across graphs may expose additional candidates. Reports still follow the existing cadence, so allow a later sample to appear when inspecting mode switches. The compiler defect and missing voxel inputs have not been fixed by this phase.

User result: no Vulkan validation errors were reported, but `diagnostic=missing_barrier` remained. The full diagnostic was not supplied, so that particular runtime report has not been tied to a resource/pass. The known compiler regressions still reproduced the gap at this checkpoint. The user subsequently authorized phase 3.


### Phase 3 implementation

Implemented on 2026-09-08. Buffer and texture state now retain the last writer's write access/stages and visibility per destination access/stage. Compatible readers accumulate their execution scopes for a later write. A later reader receives a barrier when its memory scope has not acquired the writer's contents; repeated covered readers remain barrier-free. A new write replaces the writer and clears coverage. External state replacement and resource invalidation replace/remove the same persistent state.

Compilation starts from persistent resource state and simulates each node's accesses. Both initial and intra-graph barriers use resource-specific stages. Coverage reflects the destination mask of the actual barrier batch, including stages contributed by its other resources. Execution emits that compiled plan directly and advances persistent state with the same state-transition rules. Every execution refreshes the plan, including re-execution of an already compiled graph. Transfer-queue eligibility includes the retained writer's stages through those compiled barriers.

`RHITextureTransition` and `RHIBufferTransition` now carry an optional `additionalSrcAccess` mask. The Vulkan backend and metrics normalization combine it with the latest usage's source access. This preserves earlier writes without inventing an old image layout or replacing the latest reader's usage. Texture barriers always use the current tracked layout; layout changes reset reader visibility because the transition itself can write image memory. This follows the [Vulkan image-layout transition rules](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions). Tracking remains conservative at whole-resource granularity.

Validation: all **45 tests** in `RenderCoreTest` passed, with no tracked memory leaks. The Vulkan `VulkanCommandList.cpp` object also compiled successfully. The former same-graph and unsampled-upload regressions now require zero missing/stage/access diagnostics and assert the emitted writer access and destination stage. Added regressions cover fragment/compute storage readers, vertex and index readers, repeated covered reads, new writes, compiled-graph re-execution, reader ordering before a new write, graphics-queue fallback, texture layout changes, and synchronization with metrics disabled. The independent validator tests still inject and detect missing/corrupt barriers. Full output: `/tmp/zen-rdg-phase3-tests.log`.

User checkpoint, from the repository root:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo RenderCoreTest
./bin/RenderCoreTest
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

Allow several steady frames in voxel and PBR, switch **1 → 2 → 1** repeatedly, and resize in both modes. Confirm the image remains correct and the acquire-related hazard stays absent. Inspect subsequent sampled RDG reports for `missing_barrier`, `stage_coverage`, `access_coverage`, and `layout_mismatch`. The reader-visibility cases fixed here should disappear; preserve the full diagnostic line for any remaining report so its resource and pass can be identified. The voxel initialization and shader-declaration findings belong to later phases and are still open.

User compute-mode checkpoint: the supplied excerpt contains three `redundant_barrier_candidate` reports for `large_triangle_buffer` at `VoxelizationComp` and `instance_position_buffer` / `instance_color_buffer` at `VoxelPreDrawComp`, plus `unknown_import_state` for `voxel_emissive` at `VoxelInjectRadianceComp`. It contains no missing-barrier or stage/access/layout-coverage report.

Source review explains the three buffer candidates: these buffers are allocated without initial data and their first graph uses write the output entries. The compiler conservatively emits a barrier from the empty initial buffer state; the validator finds no prior access requiring it. These are first-use optimization candidates, not evidence of missing synchronization. The emissive report matches the still-open phase 4 producer/initialization defect: the compute voxelizer writes albedo but supplies no emissive or normal producer before radiance injection reads those volumes. No phase 4 changes were made at this checkpoint.

The user subsequently authorized phase 4 and correction of the redundant initial-buffer barriers.


### Phase 4 implementation and initial-buffer-barrier correction

Implemented on 2026-09-08. Both voxelizers use `VoxelizerBase::BeginVoxelization()` to schedule `ResetVoxelVolumes` before initial voxelization and every requested revoxelization. The pass clears the entire albedo, normal, emissive, and static-flag volumes. Steady frames do not clear the stored voxelization. The compute path's old albedo-only reset is replaced by this shared reset; geometry's packed atomic accumulation now also starts from zero instead of uninitialized or stale contents.

Every clear explicitly uses `Color(0.0f)`, including zero alpha. The default `Color()` has alpha one, which would incorrectly mark empty compute voxels occupied. Zero has the required representation for both compute's normalized color volumes and geometry's packed unsigned-integer accumulators. Volume allocations enable copy usage for the transfer clears. RDG now routes image clears through the graphics queue because [`vkCmdClearColorImage`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdClearColorImage.html) requires graphics or compute capability and transfer-destination image usage.

Radiance injection now requires `ProducesRadianceInputs()`. Geometry opts in because its voxelization shader produces normal and emissive data. Compute remains albedo-only, so its GI reset/injection passes are not recorded. Clearing unused normal/emissive volumes does not turn them into valid surface data. This uses the gating option in the agreed phase 4 scope and preserves the displayed albedo visualization. Producing meaningful compute normals/emissive remains a separate shader feature; the remaining shader-declaration and unused-work phases have not been executed.

The three reported buffer candidates are addressed in the compiler, without suppressing diagnostics: an empty tracked buffer state no longer generates an initial barrier because there is no prior access or image layout to synchronize. The first write still establishes writer history, so later consumers and overwrites retain their required barriers. Unknown external reads remain independently diagnosed by metrics.

The demo now accepts **R** to call `RequestVoxelization()` and request a metrics capture. This exercises the same reset path used when assigning a scene and allows repeatable runtime verification without editing source.

Validation: all **48 tests** in `RenderCoreTest` passed, with no tracked memory leaks. New regressions verify removal of empty initial buffer barriers while retaining writer-to-reader synchronization; graphics routing for clear-only graphs; and shared volume resets in both normalized and packed-integer formats. The volume test checks transfer-destination usage, all four full-volume zero clears, clear-before-consumer ordering, no duplicate clears on steady frames, and repeated clears plus barriers after an explicit revoxelization request. Compute voxelizer, geometry voxelizer, GI renderer, and demo source objects compiled successfully. Full output: `/tmp/zen-rdg-phase4-tests.log`. These mock tests do not execute the voxel shaders; GPU verification remains the user checkpoint.

User checkpoint, from the repository root:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo RenderCoreTest
./bin/RenderCoreTest
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

In compute mode, confirm that the voxel image is unchanged, then press **R** several times. The initial/requested voxelization should contain `ResetVoxelVolumes`; `VoxelInjectRadianceComp` and its radiance reset should be absent. The reported initial-buffer candidates and `voxel_emissive` unknown-state diagnostic should disappear. Switch **1 → 2 → 1**, resize in both modes, and verify that synchronization validation stays clean. On a GPU selecting the geometry voxelizer, injection remains active: verify startup and repeated **R** resets there as well. Geometry runtime verification remains pending until exercised on suitable hardware.

User result: **all ok**. Phase 4 was accepted and work advanced to phase 5; geometry-specific GPU coverage has not been separately reported.


### Phase 5 implementation

Implemented on 2026-09-08. `voxel_vis.frag` now declares `InstanceColorBuffer` readonly. Reflection therefore records voxel drawing as a reader of both instance buffers. Initialization still synchronizes compute writes with their vertex/fragment consumers; repeated draws no longer invent a color-buffer write or require a draw-to-draw buffer barrier. Revoxelization still orders its buffer overwrites after those readers.

The related shader-access audit resulted in these declarations:

| Shader / binding | Access contract |
| --- | --- |
| Voxel drawing: `InstanceBuffer`, `InstanceColorBuffer` | Readonly positions and colors. |
| Both compute voxelization passes: `VertexBuffer`, `IndexBuffer`, `NodeBuffer`, `TriangleMap` | Readonly scene data; triangle-map declaration corrected. |
| `LargeTriangleArray` | Writeonly in the producing voxelization pass; readonly in the large-triangle consumer. Declarations moved from the shared include to their respective shaders. |
| Compute `IndirectBuffer` | Atomic read/write in the producer; writeonly in its reset shader. The large-triangle shader does not access it, so its unused shader declaration/binding was removed. The pass retains `UseIndirectBuffer()` and `DispatchIndirect()`. |
| Compute voxel image and reset image | Writeonly storage images; size queries remain available. |
| Pre-draw instance outputs | Writeonly position/color buffers; the indirect counter remains read/write. |
| Geometry accumulation | Albedo, normal, and emissive images remain writable for atomic averaging. |
| Radiance injection | Normal remains read/write because injection updates its alpha; radiance stays writeonly and emissive readonly. |
| PBR and shadow node/material inputs | Existing readonly declarations match their reads. |

The unused geometry static-flag binding remains conservative and is a resource-cleanup candidate for phase 6. This phase does not enable compute radiance injection or implement the unused GI cleanup. RHI still represents writable shader bindings conservatively as `eReadWrite`; adding `writeonly` does not introduce a new write-only access mode.

Shader compilation now produces dependency files consumed by CMake, so modifying `compute_voxelizer_common.glsl` rebuilds both including compute shaders. The generated dependency files were inspected and a subsequent `SpvShaders` dry run reported no pending work. RenderCore tests now depend on rebuilt shaders and can use the actual SPIR-V reflection in the mock backend.

Validation: rebuilt shaders, `ZenCore`, `RenderCoreTest`, and `VulkanRHITest`. All **49 RenderCore tests** and **7 shader-reflection/Vulkan utility tests** passed, with no tracked memory leaks. Reflection tests check the corrected readers, retained writers, and the consumer's sparse set-4/binding-1 layout after removing its unused indirect binding. The new graph regression uses compiled pre-draw and draw shaders' reflection, verifies compute-to-fragment color visibility, three consecutive draws without buffer barriers, and the reader-to-writer dependency before revoxelization. Test outputs: `/tmp/zen-rdg-phase5-RenderCoreTest.log` and `/tmp/zen-rdg-phase5-VulkanRHITest.log`. Build output: `/tmp/zen-rdg-phase5-build.log`.

User checkpoint, from the repository root:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo RenderCoreTest VulkanRHITest
./bin/RenderCoreTest
./bin/VulkanRHITest
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

Rebuild the executable as well as shaders because the large-triangle pass's descriptor bindings changed. Confirm the image remains correct during steady voxel rendering, repeated **R** voxelizations, mode switches, and resizing. In sampled RDG details, `VoxelDraw2` should no longer emit steady instance-color buffer transitions; first/repeated voxelization still needs its producer-to-draw synchronization. Keep synchronization validation enabled. Geometry runtime checks still require a suitable GPU.

Stop here for the user's build/runtime verification before phase 6.

### Phase 5 follow-up: staging-source initialization

On 2026-09-09, addressed the user's `staging_upload` sample with two copy nodes,
three resources, zero barriers, and `unknown_import_state=1`. A mock-RHI regression
reproduced the same counters before the fix. Both copies read one staging buffer
already filled by `StageBytes()` on the CPU, but its initialization was undeclared.

The upload graph now uses `ImportHostWrittenBuffer()` for those source buffers.
This explicit graph-local contract records initialization without inventing a GPU
writer, clearing synchronization history, or adding a host-to-transfer barrier.
The backend requires coherent CPU-write memory, queue submission makes the host
writes visible, and the staging allocator protects in-flight ranges. Ordinary
unknown imports, uninitialized textures/transients, and missing GPU barriers remain
diagnosed. The declaration covers staging sources for both buffer and texture uploads.

This is a phase 5 checkpoint correction; phase 6 has not started. Rebuild and rerun
the demo with synchronization validation enabled. The reported startup upload sample
should have zero diagnostics while retaining zero barrier calls.

Validation: rebuilt `RenderCoreTest`, `ZenCore`, and `VulkanRHITest`; all **51 RenderCore
tests** and **7 shader-reflection/Vulkan utility tests** passed. The regression checks
the exact two-copy startup graph, completed staging-block reuse, validation-history
reset, copied bytes, and continued transfer-queue execution. It reports zero
diagnostics and zero barriers. Texture staging also has no unknown-state diagnostic.
Independent tests retain unknown-import detection and deliberately verify that the
host-written declaration does not hide a missing dependency from a GPU writer.
Logs: `/tmp/zen-rdg-staging-build.log`, `/tmp/zen-rdg-staging-RenderCoreTest.log`, and
`/tmp/zen-rdg-staging-VulkanRHITest.log`. GPU runtime verification is left to the user.


### Phase 6 implementation

Implemented on 2026-09-09 after the user accepted phase 5 and the staging-source
correction. This uses the plan's default scope: both voxelizer paths display albedo.
A radiance display remains a separate rendering feature.

`RendererServer` now creates and dispatches only skybox, voxelization, and PBR
renderers. It no longer creates, initializes, binds a scene to, or dispatches the
shadow and GI renderers. This removes `evsm`, `shadowmap_copy`, `shadowmap_mipmaps`,
the radiance reset, and radiance injection from the voxel frame path. PBR's
`SkyboxDraw → OffScreen → SceneLighting` path is unchanged. Skybox and voxel draw
order and viewport/resize bindings are preserved.

The current compute and geometry voxelizers allocate only albedo. The unused static
flag volume was removed. Geometry voxelization no longer samples emissive data or
atomically accumulates normal/emissive outputs; those image declarations and CPU
bindings were removed together. The albedo atomic accumulation, alpha clipping,
material layout, push-constant layout, and displayed albedo binding remain unchanged.
`ResetVoxelVolumes` clears albedo before first and requested voxelizations, including
zero alpha, and still synchronizes the clear with its producer. The base class retains
optional normal/emissive allocation and reset for a future producer explicitly
implementing `ProducesRadianceInputs()`; neither current voxelizer claims that capability.

The standalone shadow/GI implementation remains outside the server's active paths.
GI now takes its voxelizer and shadow renderer dependencies explicitly, stays inactive
when valid radiance inputs or a shadow renderer are absent, and safely supports
teardown without initialization. Its six unused mip-volume allocations were removed.
Adding GI back requires a valid producer and an actual rendering consumer; it is not
an option exposed by this phase.

Allocation and schedule expectations from the source changes:

| Item | Before phase 6 | After phase 6 |
| --- | --- | --- |
| Base voxel textures | 4: albedo, normal, emissive, static flag | 1: albedo |
| GI textures allocated by the server | 7: radiance plus six mip volumes | 0 |
| Shadow textures allocated by the server | 3: sampled mip chain, color target, depth target | 0 |
| Steady voxel frame after startup | Compute: skybox, shadow render/copy/mips, voxel draw; geometry also reset/injects radiance | `SkyboxDraw → VoxelDraw2` (compute) or `SkyboxDraw → VoxelDraw` (geometry) |
| Steady PBR frame | Three graphics passes | Same three graphics passes |

These changes remove 13 texture allocations, excluding views. The base allocation
count is checked with the mock RHI; the server allocation/schedule reductions are
confirmed by source inspection. Expected steady voxel pass counts are now
**2 graphics / 0 compute / 0 transfer**. Initialization and **R** revoxelization still
add their required producer/reset passes.

Validation: rebuilt the changed geometry shader, `ZenCore`, `RenderCoreTest`, and
`VulkanRHITest`. All **52 RenderCore tests** and **7 reflection/Vulkan utility tests**
passed. The volume lifecycle regression covers single-albedo and optional three-volume
allocation for both packed UINT and normalized RGBA formats, initial/repeated clears,
and clear-to-producer barriers. The new inactive-GI regression checks that albedo
creates exactly one texture and GI adds no texture or graph node. Shader reflection
requires writable geometry albedo and no normal/emissive/static-flag binding; the
standalone injection shader retains its real read/write contracts. Existing staging,
writer-visibility, and steady voxel-draw regressions also pass.

Logs: `/tmp/zen-rdg-phase6-build.log`, `/tmp/zen-rdg-phase6-RenderCoreTest.log`, and
`/tmp/zen-rdg-phase6-VulkanRHITest.log`. The phase-only patch is
`/tmp/zen-rdg-phase6.patch`. A pre-existing whitespace issue elsewhere in
`RenderGraph.cpp` was left untouched; this phase adds no trailing whitespace.

User checkpoint:

```sh
cmake --build build/arm64-apple-clang-debug --target scene_renderer_demo
VK_LAYER_VALIDATE_SYNC=1 ./bin/scene_renderer_demo
```

Check the displayed voxel image, repeated **R**, **1 → 2 → 1** switches, and resizing
in both modes. Let each mode run long enough for its automatic steady-frame metrics
sample (at least five seconds). The old shadow/GI node names should be absent from
voxel samples; the albedo-only steady frame should have two graphics passes.
Synchronization validation and RDG diagnostics should remain clean. Geometry runtime
coverage still requires a GPU supporting that path.

**Refreshed live baseline pending:** preserve the historical table above. No GPU demo
was run during this phase; fresh startup, steady voxel, steady PBR, and switch-back
snapshots are needed at this user checkpoint before replacing measured resource,
barrier, or CPU-time counts. The table here records expected structural changes,
not new performance measurements. Stop here for user verification.


### Follow-up: rebuild renderer declarations from current data

Implemented on 2026-09-09 at the user's request. The server already began and ended a
new frame graph each frame, but renderer-owned pass descriptions cached resource
handles and attachment configuration. Skybox, PBR, compute voxelization, geometry
voxelization, shadow rendering, and standalone GI now construct fresh local pass
descriptions inside `BuildRenderGraph()`, alongside their current resource bindings,
uniform values, attachments, render areas, and command registration.

Removed renderer pass-description members and the separate `BuildGraphicsPasses`,
`BuildComputePasses`, resource-update, and uniform-update methods. `BuildRenderGraph`
is the public frame entry point called by `RendererServer`; the workload wrappers
and resize callbacks that patched cached descriptions are gone. Device/graph resize
invalidation remains in place. Resource preparation, including samplers, buffers,
textures, environment outputs, and the existing macOS allocation warmup, retains
its existing lifetime. Pipeline and transient-resource caches remain in the device/RDG.

PBR, shadow, and geometry draw callbacks now own a snapshot of mesh ranges and
node/material indices. Mutating scene metadata after a graph has been built cannot
make its commands disagree with that graph's bindings. Skybox environment preparation
still runs only when requested; its cubemap and LUT pass declarations are built in
that frame's graph. Voxel generation still uses the existing dirty flag and **R** /
`RequestVoxelization()` trigger. Changes to voxelized scene contents must request
regeneration; this refactor refreshes graph declarations and does not introduce
continuous voxelization or automatic scene-edit detection.

Validation: rebuilt `ZenCore`, `RenderCoreTest`, and `VulkanRHITest`; all **53 RenderCore
tests** and **7 reflection/Vulkan utility tests** passed. A new regression links the
real skybox/PBR builders and compiled shader reflection against a mock asset facade
and RHI. It changes buffers in the same scene, shrinks the scene texture array,
replaces all environment textures and viewport attachments, changes viewport and
G-buffer sizes, and changes uniform data without calling renderer update callbacks.
It verifies current bindings/outputs, absence of old bindings, immutable recorded
uniforms and draw metadata, and pipeline reuse when the next graph's pipeline state
is unchanged. Existing voxel reset, steady draw, GI gating, and synchronization
regressions also pass.

Logs: `/tmp/zen-rdg-dynamic-build.log`, `/tmp/zen-rdg-dynamic-RenderCoreTest.log`, and
`/tmp/zen-rdg-dynamic-VulkanRHITest.log`. Patch: `/tmp/zen-rdg-dynamic-renderers.patch`.
The GPU demo was not run during this refactor. Rebuild `scene_renderer_demo`, then
verify steady rendering, **1 → 2 → 1**, resizing, and repeated **R** with
`VK_LAYER_VALIDATE_SYNC=1`. Stop here for the user's build/runtime verification.
