# RenderCore production-readiness fix plan

Status: implementation in progress; evidence and remaining gates are recorded in [RenderCoreProductionFixVerification.md](RenderCoreProductionFixVerification.md). The plan was proposed 2026-10-18, based on `d8a74b2e` (branch `hybrid-gi`). Phases 7–8 (native submission count and async-compute overlap) were added the same day after a follow-up analysis of the schedule. Findings and line numbers below describe that baseline commit.

This plan addresses the findings of the 2026-10-18 production-readiness review of `RenderCore/V2`. That review found the correctness hardening thorough: transactional recording with rollback, submission-failure propagation, serial-gated retirement, validated uploads and copies. The release `RenderCoreTest` passed 588 of 588 tests with no leaks. What remains is one cross-layer behavior defect and several costs that grow with content size rather than with the frame's working set. They are invisible on Sponza-class scenes and will matter with production content. A follow-up analysis added two schedule findings. Native queue submissions equal RDG submission groups, and the grouping rule over-splits same-queue work. Async compute is wired correctly but gives no measured overlap in the current GI frame.

**Device-loss recovery is deferred** (user decision, 2026-10-18). Every phase below keeps today's behavior once submissions are blocked: rendering stops and the applications exit.

Scope: `RenderCore/V2` (`RenderDevice`, the render graph executor and resource manager, `StagingUploadQueue`, `RenderSubmissionHistory`, `TextureManager`, `RenderScene`, `RendererServer` and the renderers' frame-result handling), the editor and scene-renderer-demo main loops, and their tests. RHI contracts are unchanged except where Phase 3 adds a queued, non-presenting batch submission and Phase 7 packs several submission groups into one native queue submission.

## 1. Relationship to other plans

| Plan | Relationship |
| --- | --- |
| [RDGRenderCoreAnalysisAndPlan.md](RDGRenderCoreAnalysisAndPlan.md) | Correctness follow-up 4 introduced the private-tracker transaction and submission-failure propagation that Phases 1–3 build on. Its deferred items (Phase 4 range-aware barriers, render-pass merging, immutable pass templates, backend memory aliasing) stay deferred. |
| [RenderCoreRHIThreadingPlan.md](RenderCoreRHIThreadingPlan.md) | Defines the threaded frame handoff: one pending frame, confirmation through its ticket. Phase 1 changes how a declined handoff is reported; Phase 3 extends the handoff model to upload batches. |
| [AsyncComputeImplementationPlan.md](AsyncComputeImplementationPlan.md) | Scheduled submission groups and cross-queue history. Phase 2 changes how that history is resolved, not what it orders. Phases 7 and 8 change how groups are formed, ordered and submitted. |
| [AsyncComputeNsightVerification.md](AsyncComputeNsightVerification.md) | Verified queue routing and dependencies but measured zero graphics/compute overlap. It is the baseline Phase 8 must beat. |
| [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) | Steady pool retention for the frame graph. Unchanged. |
| [RHIPipelineCompilationFollowup.md](RHIPipelineCompilationFollowup.md) | Owns asynchronous pipeline compilation (T1 below). Not repeated here. |

## 2. Findings

| ID | Severity | Problem | Evidence |
| --- | --- | --- | --- |
| F1 | High | **A declined frame is reported as a failure, and both applications quit on it.** In threaded mode (the default, `RenderConfig.h:125`) a present that needs swapchain recreation (`VK_SUBOPTIMAL_KHR`, `VK_ERROR_OUT_OF_DATE_KHR`) is reported in the batch result one frame later. `CompleteFrame` then sets `m_pRecreateViewport`, and the next frame is deliberately declined: `ExecuteFrameGraph` skips execution and `SubmitRecordedGroups` rejects the handoff. Both return `false`, and `DispatchRenderWorkloads` passes it on. The editor and the demo end their main loop on the first `false`. They are safe only when the application notices a size change before dispatch, because `ResizeViewport` clears the flag. Renderers also treat the decline as a failed frame: `HybridGIRenderer` destroys its history, `SceneRayQuery` releases its active generation, `VoxelGIRenderer` resets its revisions (full re-injection). The decline itself is intended and tested. The application exit has not been reproduced interactively. | [RenderDevice.cpp:363](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp), `:458`, `:742`; [RHICommandListExecutor.cpp:927](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp); [EditorApplication.cpp:577, :682](../ZenEditor/App/EditorApplication.cpp); [SceneRendererDemo.cpp:756, :884](../ZenSamples/VulkanRHIDemo/SceneRenderer/SceneRendererDemo.cpp); [HybridGIRenderer.cpp:729](../ZenCore/Source/Graphics/RenderCore/V2/HybridGIRenderer.cpp); [SceneRayQuery.cpp:513](../ZenCore/Source/Graphics/RenderCore/V2/SceneRayQuery.cpp); [VoxelGIRenderer.cpp:816](../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp); test `DeclinedHandoffRetainsPriorHistoryAndAllowsRetry` ([RDGSubmissionFailureTests.inl:158](../ZenSamples/RenderCoreTest/RDGSubmissionFailureTests.inl)) |
| F2 | High (scale) | **Every graph execution deep-copies all tracked resource state.** `ExecuteTransaction` copies the whole persistent `ResourceStateTracker` and the whole `RDGMetrics` for rollback. The tracker is `std::unordered_map` per resource. Texture contents keep a nested map with one entry per (aspect, mip, layer), and every scene texture is tracked because uploads go through RDG. The cost is paid by the frame graph, every upload graph and every standalone graph, and grows with total content, not with the graph. | [RenderGraph.cpp:974, :976](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp), `:3160–3172` |
| F3 | Medium (scale) | **Submission history is scanned in full on every execution.** `ResolveAccepted` walks every entry; entries are removed only when their resource is destroyed. It runs from `PrepareScheduledSubmissionHistory` (every graph execution), `CompleteFrame` and the non-pending submission path. | [RenderSubmissionHistory.cpp:317](../ZenCore/Source/Graphics/RenderCore/V2/RenderSubmissionHistory.cpp); [RenderDevice.cpp:895](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) |
| F4 | Medium | **Any upload in a frame blocks the render thread on the RHI thread.** `StagingUploadQueue::Flush` first drains pending frames (`PollFrameSubmissions(true)`) to take a serial baseline. It then submits through `ExecuteRenderGraph(RenderGraph&)`, which drains again and submits with `SubmitGroups`, a queued batch followed by `ticket.Wait()`. Batches run in FIFO order, so the render thread waits for the previous frame's batch, including acquire and present, before recording its own commands. `DestroyBuffer`/`DestroyTexture` flush on every call even though pending uploads already hold a destination reference. The editor uploads a bounds buffer every frame; animated scenes upload every frame (F5). | [StagingUploadQueue.cpp:490–508, :321, :413](../ZenCore/Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp); [RenderDevice.cpp:625, :1196, :1294](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp); [RHICommandListExecutor.cpp:567](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp); [EditorViewport.cpp:856](../ZenEditor/Rendering/Source/EditorViewport.cpp) |
| F5 | Medium | **Animated scenes regenerate scene buffers every animated frame.** Node, vertex and UV buffers (materials when dirty) are created anew and fully re-uploaded, and the old ones are destroyed. Each create and destroy is a blocking RHI-thread invoke; each destroy flushes uploads (F4). New stable IDs also miss the descriptor cache. | [RenderSceneGeometry.cpp:489–545](../ZenCore/Source/Graphics/RenderCore/V2/RenderSceneGeometry.cpp) |
| F6 | Medium (scale) | **Diagnostic strings are built on success paths.** `Check(bool, code, const std::string&)` takes its message by `const std::string&`, so every call constructs the string before the condition is tested. In the encoder that is about 4–5 heap allocations per `SetPushConstants` + `DrawIndexed` (all messages exceed MSVC's 15-character small-string buffer), repeated for every pass that draws the scene. Compile paths build concatenated messages per pass and per resource every frame. | [RDGDefs.h:137](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGDefs.h); [RDGPassCompiler.h:885](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGPassCompiler.h); [RDGPassCompiler.cpp:585, :664, :708, :739, :771, :812](../ZenCore/Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp); [RenderGraph.cpp:1220](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp) |
| F7 | Low | **The RDG texture-view cache is never pruned** before manager destruction. Entries for destroyed textures stay, and the cache is searched linearly. Current renderers use pre-created raw views, so the impact today is small. | [RDGResourceManager.cpp:481–511, :1543](../ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp) |
| F8 | Low | **Scene mip chains are copied byte by byte** into the upload payload at load. | [TextureManager.cpp:357–378](../ZenCore/Source/Graphics/RenderCore/V2/TextureManager.cpp) |
| F9 | Note | **`VERIFY_EXPR*` aborts in Release** at API entry points (for example the `EnqueueBuffer` bounds check and `CreateTextureView` ranges). That is acceptable while callers validate content first, as they do today (editor bindless-capacity precheck, glTF deformation validation), but the contract is not written down. | [StagingUploadQueue.cpp:296–298](../ZenCore/Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp); [RenderDevice.cpp:1187](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp); [EditorViewport.cpp:214](../ZenEditor/Rendering/Source/EditorViewport.cpp); [FastGLTFLoader.cpp:1286](../ZenCore/Source/AssetLib/FastGLTFLoader.cpp) |
| F10 | Medium | **Native submissions equal submission groups, and grouping over-splits same-queue work.** `ExecuteGroups` submits each group through its own `ExecuteBatch` → `FlushAllGPUCommands`, which is one `vkQueueSubmit` on that group's queue. Groups go one at a time because a waiting group needs its producer's timeline serial, and serials are assigned at submission (`ResolvePredecessors`). Consecutive groups on the same queue therefore cost separate calls even with no cross-queue wait between them. `BuildSubmissionGroups` appends a pass to the open group only if its set of first other-queue consumers equals the previous pass's, and seals a group once another group consumes it. In the 2026-10-17 release GI smoke run (mode 3, RHI thread on, async on), steady frames have 37 passes in 6 groups. The first frame has 271 passes in **176 groups**: its environment preprocessing passes (one cubemap face or mip each) land in separate graphics groups linked only by same-queue order. | [RHICommandListExecutor.cpp:690, :727, :789](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp); [VulkanQueue.cpp:378, :587](../ZenCore/Source/Graphics/VulkanRHI/VulkanQueue.cpp); [RenderGraph.cpp:4578, :4616, :4701](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp); [smoke-m3-t1-a1.log](../build/rhi-validation-defaults/release/acceptance/smoke-m3-t1-a1.log) |
| F11 | Medium | **Async compute is wired correctly but gives no overlap in the current GI frame.** Routing to the dedicated compute family, per-resource eligibility, timeline waits at cross-queue edges and cross-frame history are in place, and the feature is on by default (`async_compute=auto`). Nsight (Debug, 2026-09-20, an earlier voxel workload) verified routing and dependencies but measured zero overlap; voxel-update frames got slower (warmed GPU work span 1.40 → 2.33 ms). The steady GI frame alternates queues along one dependency chain: G-buffer (graphics) → voxelization and opacity mips (compute) → sky irradiance and radiance injection (graphics) → radiance mips (compute) → Hybrid GI trace, temporal, filters and lighting (graphics). Only the G-buffer and one copy run alongside compute work. Causes: **(a)** only voxel passes opt in; Hybrid GI's compute passes do not, and hardware-RT sky and injection are pinned to graphics. That is a renderer choice, not an RDG limit, since RDG tracks acceleration-structure storage buffers; the code does not record why. **(b)** The pass order is a FIFO topological sort that ignores queues, so nothing moves independent graphics work into a fork/join window. **(c)** Hybrid GI reads this frame's voxel radiance, so the chain has no slack. **(d)** Any multi-queue frame disables within-frame transient reuse. | [RenderGraph.cpp:4057, :3357, :4731](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp); [VoxelGIRenderer.cpp:552, :643](../ZenCore/Source/Graphics/RenderCore/V2/VoxelGIRenderer.cpp); [Data/engine.cfg:6](../Data/engine.cfg); [AsyncComputeNsightVerification.md](AsyncComputeNsightVerification.md); [smoke-m3-t1-a1.log](../build/rhi-validation-defaults/release/acceptance/smoke-m3-t1-a1.log) |

Tracked elsewhere and not planned here:

| ID | Item | Owner |
| --- | --- | --- |
| T1 | First-use pipeline creation is synchronous and can hitch (about 20 ms on a delayed first GI entry). | [RHIPipelineCompilationFollowup.md](RHIPipelineCompilationFollowup.md) |
| T2 | The device `VkPipelineCache` is not persisted to disk. | [RHI README](../ZenCore/Include/Graphics/RHI/README.md) (persistence and versioning policy still to be measured) |
| T3 | No memory aliasing between different transient resources; only identical descriptors share a native object within a frame. | [RDGRenderCoreAnalysisAndPlan.md](RDGRenderCoreAnalysisAndPlan.md) (deferred) |
| — | Device-loss recovery. | Deferred 2026-10-18 |

## 3. Invariants to preserve

1. **RDG transaction.** Recording uses private state; persistent state, contents and history are committed only after the submission is accepted (or handed off). Failure or rejection rolls everything back. Extractions publish only after acceptance.
2. **Retirement.** No resource, staging block or pooled allocation is reused or destroyed before every queue that used it has completed. The pool still skips in-flight entries.
3. **Ordering.** Cross-queue dependencies, layout transitions and serialized-read behavior of `RenderSubmissionHistory` are unchanged.
4. **Modes.** Inline and threaded execution, and async compute on and off, produce the same images and the same failure semantics.
5. **RHI ownership.** Native object creation and destruction stay on the RHI thread.
6. **Blocked submissions.** Once `AreSubmissionsBlocked()` is true, behavior is unchanged: no further GPU work, owners retained until teardown, applications exit.
7. **Schedule changes.** Regrouping, reordering and packing (Phases 7–8) keep every version dependency and produce byte-identical captures. Rejection stays retryable: no group may wait on work whose submission has not been accepted. The one exception to byte-identical captures is Phase 8's opt-in previous-frame radiance, which changes images by design.
8. **Repository rules.** The global C++ rules (one return at the end, explicit types, `HeapVector`, no exceptions), blank lines between statements, whole-file clang-format 19.1.5. Commits carry no AI attribution trailers.

## 4. Implementation phases

Each phase builds, passes its tests and can be reviewed alone. Phase 1 does not need the baseline; Phases 2–5, 7 and 8 do. Add a native-submissions-per-frame counter to the Phase 0 profile for Phases 7–8.

### Phase 0 — Baseline and instrumentation (S)

1. **Scaling benchmark** (RenderCoreTest, Release, mock RHI). Track N imported textures (N = 100, 1 000, 5 000; 12 mips each, uploaded through `StagingUploadQueue`), then run repeated small frame graphs touching a few resources. Record CPU time per `ExecuteTransaction` and per `ResolveAccepted` against N.
2. **Encoder benchmark.** 10 000 `SetPushConstants` + `DrawIndexed` through `RDGPassCmdEncoder` on the mock RHI: ns per draw, and heap allocations per draw. The engine allocator tracks only `ZEN_NEW`-style sites, so count `std::string` allocations with a counting global `operator new` in a small dedicated test executable.
3. **Render-thread blocking counters.** Add, if missing, the cumulative render-thread time spent in `PollFrameSubmissions(true)` and in `SubmitGroups`' wait, plus per-frame counts of buffers created and destroyed and bytes uploaded. Report them in the demo profile.
4. **Profiles.** The demo with Sponza (static) and with an animated glTF from the smoke set, modes 2 and 3, RHI thread 0 and 1, vsync on and off. Use the existing protocol: unhooked `7zFM.exe` copy, interleaved A/B, `--fixed-step`, 60 warm-up and 600 measured frames.

Exit: numbers recorded in `Doc/RenderCoreProductionFixVerification.md`.

### Phase 1 — Declined frames are not failures (F1) (M, first)

1. Add `enum class RenderFrameResult : uint8_t` to `RenderCoreDefs.h`:
   - `eSubmitted`: accepted (inline) or handed off (threaded).
   - `eDeclined`: nothing was submitted and persistent state is unchanged; retry after `NextFrame`. Covers a pending viewport recreation, and a submission rejected synchronously before any work was accepted (`RHISubmissionResult::eRejected` on the inline path or at handoff, which RDG already rolls back as retryable). A threaded batch rejected later on the RHI thread already blocks submissions in `CompleteFrame` (`RenderDevice.cpp:463`) and stays `eBlocked`.
   - `eFailed`: graph declaration, compilation, callback or recording error. The graph is rebuilt next frame.
   - `eBlocked`: `AreSubmissionsBlocked()`. Behavior unchanged (invariant 6).
2. `RenderDevice::ExecuteRenderGraph(RHIViewport*)` and `RendererServer::DispatchRenderWorkloads` return `RenderFrameResult`. Classify at the existing decision points: the `m_pRecreateViewport` gate in `ExecuteFrameGraph` and in `SubmitRecordedGroups`, the rejection path, blocked state, and everything else that fails. Keep `ExecuteRenderGraph(RenderGraph&)` boolean; it is not a presented frame.
3. Reduce declines. At the start of `DispatchRenderWorkloads`, before `RenderScene::Update` and graph building, poll ready tickets and process a pending recreation (a `RenderDevice` method wrapping `PollFrameSubmissions(false)` and `ProcessDeferredViewportResize`). A result that is already available then costs no discarded frame. A result arriving during graph building is still declined.
4. Renderers receive the result (`OnRenderGraphExecuted(RenderFrameResult)`). On `eDeclined`, discard recorded but unsubmitted state and keep committed history:

   | Renderer | `eDeclined` | `eFailed` (unchanged) |
   | --- | --- | --- |
   | `HybridGIRenderer` | Drop the recording; keep history, `m_valid` and the frame counter; no `Destroy()`. | `Destroy()`, reset reason `failed_frame` |
   | `SceneRayQuery` | `Release(m_pending)` only; keep `m_active`. | Release both |
   | `VoxelGIRenderer` | Clear `m_recordedRadiance`; keep revisions. | Reset revisions |
   | `SceneShadowRenderer` | Keep `m_validContents`, `m_geometryRevision`, committed faces. | Invalidate |
   | `VoxelizerBase`, `ComputeVoxelizer`, `SkyboxRenderer` | Re-request pending work, as today. | As today |

   `RendererServer` reports the decline in the debug description ("Waiting for swapchain recreation."), not "Frame submission failed."
5. Applications:
   - Editor: continue on `eDeclined` and `eFailed`. Failures already surface through the renderer status and fallback reason. Exit only on `eBlocked`.
   - Demo: continue on `eDeclined`. Keep ending the run on `eFailed` and `eBlocked`, so smoke and profiling runs still catch failures.
   - Both: treat more than a small number of consecutive declines (for example 8) as a failure with a log line, so a persistent rejection cannot loop silently.
6. Add a debug-only option that marks the swapchain for recreation every N frames without a size change, for live smoke tests.

Tests:
- RenderCoreTest, inline and threaded: `recreationRequested` on frame N gives `eDeclined` for N+1 in threaded mode (current behavior); `NextFrame` resizes once; N+2 gives `eSubmitted`. Step 3 handles a ready result before building, with no decline. Rejection with no accepted work gives `eDeclined`; blocked gives `eBlocked`.
- Renderer tests (`HybridGIRendererTests.inl` and peers): history, the ray-query active generation and voxel revisions survive a decline; `eFailed` keeps today's resets.
- `EditorRenderingTests.cpp`: the editor loop keeps running across a decline.

Exit: tests pass in Debug and Release. The smoke matrix with the forced-recreation option passes in threaded mode, vsync on and off, with no exit and no `VUID-` or `SYNC-HAZARD` lines. Interactive check: resize and move the editor window between monitors for several minutes in threaded mode without exit.

### Phase 2 — Execution cost proportional to the graph (F2, F3) (M–L)

1. **Overlay tracker.** Give `ResourceStateTracker` an optional read-only base. Reads look in the overlay first, then the base. Writes go to the overlay; `RemoveResourceState` records a tombstone. `ExecuteTransaction` constructs an overlay on `m_resourceStateTracker` instead of copying it. Commit merges overlay entries and tombstones into the base and bumps its revision once; rollback drops the overlay. The base does not change during recording, so the existing `plan.stateRevision` check stays valid. Production code reaches the tracker only through its methods; only the test helper `RDGSubmissionTestAccess::HasHistory` (`RenderCoreTests.cpp:1669`) reads its maps and must be updated.
2. **Metrics rollback.** Replace the full `RDGMetrics` copy (`RenderGraph.cpp:976`) with a checkpoint of what recording actually mutates: counters, the current snapshot and, only when validation or capture is active, validator state. Do this only if the Phase 0 benchmark shows the copy matters.
3. **Content compaction (optional).** A missing subresource key already reads as the aggregate `status` (`RenderGraph.cpp:3092`). After verifying how `status` is maintained under partial writes, drop entries equal to `status` when contents are committed. A fully uploaded texture then carries no per-subresource nodes.
4. **History resolution.** `Commit` records IDs whose uses still reference an `RHISubmissionState`; `ResolveAccepted` walks only those and drops IDs once they are fully resolved to serials. Optionally, at frame boundaries, prune entries whose every use has completed at the device's cached completed serials. A completed access needs no dependency, and a dropped `layoutUsage` only makes the next texture access a conservative transition.

Tests: the existing transaction, rollback, failure, async-compute and history suites pass unchanged. New: overlay read-through, a tombstone followed by rollback restores the base, commit touches only overlay entries, the revision advances once per commit, the unresolved list empties as tickets resolve.

Exit: Phase 0 per-execution time is flat within noise from 100 to 5 000 textures; captures byte-identical; smoke matrix clean.

### Phase 3 — Uploads do not stall the render thread (F4) (M–L)

1. **No flush on destroy.** Remove the upload flush from `RenderDevice::DestroyBuffer` and `DestroyTexture`. Pending uploads already hold a destination reference (`StagingUploadQueue.cpp:321, :413`) and retire it through their own serials. Test: destroying a resource with a pending upload, then flushing, executes the upload and retires the reference without an early free.
2. **Own serials instead of a baseline.** Return a graph's accepted serials from its submission (`SubmitRecordedGroups` already has `native.groups[].accepted`), so `Flush` no longer drains pending frames to diff global serials. Remove the `PollFrameSubmissions(true)` at `StagingUploadQueue.cpp:492`.
3. **Queued upload batches.** Pick one design after a short spike and record the decision:
   - **3a. Fold into the frame batch.** Submit the upload graph's recorded commands as leading groups of the frame's `SubmitFrame` batch, so the existing frame handoff, history commit and ticket confirmation cover them.
   - **3b. Separate queued batches.** Add a queued, non-presenting batch submission and track upload tickets alongside the pending frame. This generalizes the single-pending-frame invariant in `CaptureResourceRetirement` (`RenderDevice.cpp:1876–1883`) to an ordered list. Staging blocks and `m_retainedResources` retire through `ResourceRetirement` (which already models a pending ticket plus serials).

   Either way, keep the synchronous CPU completion wait where the backend lacks timeline dependencies and the transfer queue is separate (`RenderDevice.cpp:816–828`), and keep failure propagation: a rejected upload retains its payload for retry, and a fatal one blocks submissions.
4. **Standalone-graph drain.** Determine whether the entry `PollFrameSubmissions(true)` in `ExecuteRenderGraph(RenderGraph&)` (`RenderDevice.cpp:625`) is still needed. Queued submission points are already accepted as external predecessors (`RenderSubmissionHistory.cpp:88–97`). Remove it if it is not needed.

Tests: in threaded mode with the RHI thread gated on frame N's batch, recording and handoff of frame N+1 containing an `UpdateBuffer` complete without waiting. Staging reuse still waits for completion (`AsyncUploadTests.inl` stays green). Failed and rejected upload batches follow the existing retry and blocking rules.

Exit: the Phase 0 render-thread blocking counter for a frame with per-frame uploads matches the no-upload case. Smoke matrix and sync validation clean on a device with a dedicated transfer queue and on one with a shared queue, where available.

### Phase 4 — Animated scene buffers updated in place (F5) (M, after Phase 3)

1. Replace per-update generation buffers for nodes, vertices and UVs (and materials when dirty) with rings of R = frame-slot-count persistent buffers, reallocated only on growth. Frame k writes entry `k % R` with `UpdateBuffer`. Its previous user, frame k − R, has completed by `BeginFrame`.
2. Upload only changed data: dynamic-class nodes and the deformed vertex ranges (coalesced dirty ranges), not the whole scene. Triangle records still regenerate on visibility changes, which are rare.
3. Consumers holding a previous generation:
   - `HybridGIRenderer` keeps the previous vertex buffer for motion (`HybridGIRenderer.cpp:56, :82, :104`). Key it on a generation counter and invalidate it when the ring may have wrapped (two or more skipped frames).
   - `SceneRayQuery` hashes positions for BLAS reuse and refit (`SceneRayQuery.cpp:25, :201–224`). Confirm it is unaffected.

Tests: after warm-up, animated frames create no buffers and cycle through R stable buffers; contents match the CPU data; motion uses the previous frame's vertices; descriptor-cache misses are zero during steady playback.

Exit: the animated profile shows zero buffer creations per steady frame, upload bytes proportional to dynamic data, lower CPU frame time, and `smoke_gltf_rendering.py` passes.

### Phase 5 — No diagnostic allocations on success paths (F6) (S–M)

1. Add `const char*` overloads of `RDGResult::Check`, `RDGPassCmdEncoder::Check`, `RenderGraph::Check` and `RDGPassCompiler::Check` that construct a `std::string` only on failure. Add a formatted variant, evaluated only on failure, for messages that include names.
2. Convert the hot paths first:
   - encoder commands: `RequirePass`, `RequireIndirect`, `Draw*`, `Dispatch*`, `SetPushConstants`, `SetShaderValue`;
   - per-pass compile checks: `CompileGraphicsPass` and `CompileComputePass` messages;
   - per-resource checks: the materialization check in `CompileGraphInternal`.
3. Optional: drop the per-pass `HeapVector<uint64_t> offsets` allocation in `ExecuteGraphicsPass` (`RenderGraph.cpp:5074`).

Tests: the allocation-counting target reports zero allocations per successful draw, dispatch and push-constant call; existing failure-message assertions pass unchanged.

Exit: the Phase 0 encoder benchmark shows the per-draw improvement.

### Phase 6 — Small fixes (F7, F8, F9) (S)

1. **F7.** Remove view-cache entries when their texture leaves the manager (pool eviction, retained-import release, `Reset`), so the cache is bounded by live textures. Test: the cache size stays bounded across a repeated resize loop.
2. **F8.** In `LoadSceneTextures`, reserve the total payload and append each mip with one bulk copy.
3. **F9.** No behavior change. Document in the `RenderDevice` and `StagingUploadQueue` headers that `VERIFY_EXPR*` entry checks abort in Release, so callers must validate content first. Audit content-reachable call sites and convert any reachable from unvalidated content into error returns.

### Phase 7 — Fewer native submissions (F10) (M)

Batches and calls are different limits:
- **Batches** (`VkSubmitInfo`, each with its own waits and signals): the minimum is set by cross-queue sync points, not by the queue count. A batch signals only after all of its work completes, and its waits hold it from the start. Graphics work before a fork and after a join must therefore stay separate batches; merging them would serialize the overlap that async compute exists for. The steady GI frame's six groups are already near this minimum for its dependency shape (two fork/join cycles).
- **Calls** (`vkQueueSubmit`): batches can share a call. Without wait-before-signal, a waiting batch must go in a call after its producer's call. The floor is then one call per run of same-queue groups in submission order, so the steady frame drops from 6 to 5. The first frame drops far more, once regrouping (step 3) stops splitting same-queue work.

1. **Pack same-queue runs.** In `ExecuteGroups`, submit each maximal run of consecutive groups on the same native queue (by `queueEquivalenceId`) with one `vkQueueSubmit`, one `VkSubmitInfo` per group. Each group keeps its own serial, signal and accepted result. The timeline path already builds several `VkSubmitInfo`s per call (`BuildTimelineSubmitBatch`, `VulkanQueue.cpp:418`). Keep group boundaries as merge barriers wherever another queue waits on the earlier group: `MergeWorkloads` (`VulkanQueue.cpp:378`) gives merged workloads the later serial, which would delay that signal. A rejected call rejects every group in it, and acceptance and failure results map back per group. The fence-based path keeps one call per workload (`VulkanQueue.cpp:644`).
2. **Order groups for packing.** Stably reorder the group list, respecting predecessors, so same-queue groups that do not depend on an intervening other-queue group become adjacent.
3. **Regroup without the equality rule.** Replace exact equality of first other-queue consumer sets (`RenderGraph.cpp:4616`) with a dominance test. Appending pass P to the open group delays no consumer if every other-queue consumer of the open group also depends on P. Keep the sealing rule (`:4701`). Check the result on the first-frame graph: its interleaved irradiance and prefilter passes should coalesce.
4. **One retirement sweep per call.** Each `FlushAllGPUCommands` walks every queue and retires completed work; after step 1 it runs once per call, not once per group.

Not proposed now: one call per queue using timeline wait-before-signal with pre-assigned serials. It is valid Vulkan, but a failed later call would leave an earlier queue waiting forever. That turns today's retryable rejection into a hang. Revisit it with device-loss recovery.

Tests:
- RHI: count calls with the existing execution counter (`VulkanQueue.cpp:585`). The schedule G, G, C, G, G submits in 3 calls with per-group results, and every group another queue waits on keeps its own serial. A rejection in the middle of a run rejects the whole run and leaves earlier runs accepted. Logical queues that alias one native queue pack together.
- RDG: a graph of interleaved independent passes with different downstream other-queue consumers coalesces where the dominance test allows. `ValidateSchedule` and the existing schedule, async-compute and submission-failure suites pass unchanged.

Exit: the demo profile reports native submissions per frame. The GI smoke's first frame falls from 176 calls (record the new count), and the steady frame falls from 6 to 5. Captures are byte-identical; smoke matrix and synchronization validation are clean.

### Phase 8 — Make async compute overlap (F11) (L, gated)

1. **Measure, then decide the default.** Profile the current GI frame in Release with async compute off and on (Phase 0 protocol, plus an Nsight queue timeline). If on is not faster, change the `async_compute` default in `Data/engine.cfg` to `off` (keeping `auto` selectable) until the steps below land and measure faster.
2. **Queue-aware ordering.** Replace the FIFO topological order (`RenderGraph.cpp:4057`) with a deterministic list scheduler. Among ready passes, prefer, in order: passes on the path to a compute-queue chain (start forks early); passes on the open group's queue (keep runs together); and independent graphics work before passes that wait on compute (defer joins). Version dependencies are unchanged. Barriers are generated from the compiled order, so captures must stay byte-identical (invariant 7).
3. **One compute run for the voxel chain.** Find and record why hardware-RT sky irradiance and radiance injection are pinned to graphics (`VoxelGIRenderer.cpp:552, :643`). Where their acceleration-structure buffers are compute-accessible, opt them in, so voxelize → inject → radiance mips becomes one fork and one join instead of two.
4. **Give the chain slack (renderer option).** Let Hybrid GI and lighting optionally read the previous frame's voxel radiance, double-buffered with the existing `m_previousRadiance` (`VoxelGIRenderer.h:300`). This frame's voxel update can then overlap the G-buffer, shadows and Hybrid GI. Measure the one-frame-latency quality cost on dynamic lights and moving objects before considering it as a default.
5. **Opt in only what pays.** Evaluate other independent compute passes for `ePreferAsyncCompute` individually, and keep only those with a measured gain.
6. **Transient reuse with multiple queues.** Instead of disabling reuse for every multi-queue frame (`RenderGraph.cpp:4731`), allow it for allocations whose uses all stay on one queue. Measure peak memory.

Tests: scheduler unit tests covering an earlier fork, a deferred join, preserved dependencies and determinism. The voxel chain forms one compute group when eligible. The previous-frame radiance option keeps the correct buffer pairing across resize, a declined frame and history reset. Single-queue transient reuse stays inside multi-queue frames, and cross-queue allocations are still excluded.

Exit: the Nsight timeline shows graphics/compute overlap in steady GI frames. In Release, GPU frame time with async on is no worse than off at 720p and 1080p; record the gain. Image tools pass, captures are byte-identical with step 4 off, and there are no hazards.

## 5. Validation

| Scope | Validation |
| --- | --- |
| Every phase | Debug and Release builds; RenderCoreTest, VulkanRHITest, VulkanRHIIntegrationTest, CommonTest, the editor tests and the UI/scene suites; smoke matrix (modes 1–3 × RHI thread 0/1 × async compute 0/1, with resize, minimize/restore and shutdown) under validation and synchronization validation; `tools/check_no_exceptions.py`; whole-file clang-format. |
| Phase 1 | Forced-recreation smoke option, threaded, vsync on and off; interactive resize and monitor moves in the editor. |
| Phases 2, 5 | Phase 0 benchmarks before and after; captures byte-identical. |
| Phase 3 | Render-thread blocking counters; dedicated and shared transfer queues; `AsyncUploadTests.inl`. |
| Phase 4 | Animated profile; `smoke_gltf_rendering.py`; motion and temporal GI review on an animated scene. |
| Phase 7 | Native submissions per frame (first and steady frames) before and after; captures byte-identical; async on and off; dedicated and shared queue devices. |
| Phase 8 | Release async off/on A/B at 720p and 1080p; Nsight queue timeline; peak memory; image tools; with step 4 on, a temporal-quality review on dynamic lights and moving objects. |

Record environment, commands, counters and measurements in `Doc/RenderCoreProductionFixVerification.md`.

## 6. Order and size

| Phase | Size | Depends on | Notes |
| --- | --- | --- | --- |
| 1 Declined frames | M | — | Correctness; do first. Independent of the baseline. |
| 0 Baseline | S | — | Required before Phases 2–5; can run alongside Phase 1. |
| 2 State cost | M–L | 0 | Largest scaling win; preserves the transaction model exactly. |
| 3 Upload stalls | M–L | 0 | Spike 3a versus 3b first. |
| 4 Animated buffers | M | 3 | Benefits from cheap uploads; independent of Phase 2. |
| 5 Diagnostic strings | S–M | 0 | Mechanical; can land any time after the baseline. |
| 6 Small fixes | S | — | Any time. |
| 7 Native submissions | M | 0 | Steps 1–2 are RHI/RDG plumbing; step 3 changes grouping and needs its own A/B. |
| 8 Async overlap | L | 0, 7 | Step 1 (measure, maybe default off) can land first; steps 2–6 are gated on measured gains. |
