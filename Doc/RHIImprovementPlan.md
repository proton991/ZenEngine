# RHI correctness and performance improvement plan

Status: implemented, 2026-10-01, in `c9fdebfd`. Phases 0–6, 8 and 9 are implemented. Phase 7 was measured, failed its gate and was reverted. The optional small-pool comparison (Phase 2, step 4) was not run. Measurements and results are in [RHIImprovementVerification.md](RHIImprovementVerification.md). Section 5 is the guide for the remaining deferred work.

The findings in sections 2–4 come from a static review of the active `Graphics/RHI` and `Graphics/VulkanRHI` code at revision `d22e0641`. Their file references describe that revision; Phase 9 has since deleted some of those files, for example `VulkanRenderPass.cpp`.

Fix the correctness bugs found in the review, remove per-draw locking and casting overhead from the recording and translation paths, and stop enabling device features the engine does not use. When everything else is finished, a final cleanup phase deletes the legacy renderer (`Graphics/Val`, the old RenderCore, and every `ZEN_BUILD_LEGACY` sample) and removes the unused legacy code paths from the RHI. Every performance change is gated by a before/after measurement with the existing profiler; a change that does not help is not kept.

Error and failure handling is deferred: this plan neither implements it nor depends on it (see section 1).

Scope: the active `Graphics/RHI` and `Graphics/VulkanRHI` code, their `RenderCore/V2` callers, and their tests. Phases 0–8 do not touch `Graphics/Val` or the other `ZEN_BUILD_LEGACY` targets; Phase 9 deletes them.

## 1. Relationship to other plans

| Plan | Relationship |
| --- | --- |
| [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md) (proposed) | **Deferred; not executed as part of this plan.** It owns unchecked native results, factories that publish invalid objects (for example the unchecked `vkCreatePipelineLayout` and buffers left with a null `VkBuffer`), RHI job admission/cancellation, and routing final releases through ordered cleanup. No phase here implements, waits for, or depends on any part of it. When that plan is executed later, it may reclassify the paths changed here (for example Phase 5's asynchronous release); Phase 5's `Invoke` fast path already matches its rule that reentrant work runs directly. |
| [SynchronizationSimplificationPlan.md](SynchronizationSimplificationPlan.md) (implemented) | Preserve its invariants. It excluded Synchronization2 and a presentation redesign; both remain deferred here. |
| [AsyncComputeImplementationPlan.md](AsyncComputeImplementationPlan.md) | Async-compute behavior and defaults are unchanged. Validate with `--async-compute=0` and `1`. |

## 2. Findings addressed

| ID | Problem | Evidence | Phase |
| --- | --- | --- | --- |
| B1 | `VulkanRHI` caches a raw transfer-context pointer, but `RHICommandList::Create` takes ownership of the context. Destroying that list leaves the cache dangling; the next `GetTransferCommandContext()` returns freed memory. | [VulkanContext.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp) `GetTransferCommandContext`, [RHICommandList.cpp](../ZenCore/Source/Graphics/RHI/RHICommandList.cpp) `Create` | 1 |
| B2 | `GetTextureFormatPixelSize` has no 8-bit, BGRA or depth entries and returns `0x7fffffff`, so `CalculateTextureSize` produces a meaningless value for most textures. `eR8G8B8UNORM` is declared as `30`, which is `VK_FORMAT_B8G8R8_UNORM`. RenderCore duplicates two partial workaround switches. | [Format.h](../ZenCore/Include/Graphics/Common/Format.h), [VulkanTexture.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp), [RDGResourceManager.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp) `EstimateBytes`, [RDGCopyValidation.h](../ZenCore/Source/Graphics/RenderCore/V2/RDGCopyValidation.h) | 1 |
| B3 | Debug Printf is enabled whenever the validation layer is present, but its INFO-severity output is not subscribed or logged. VERBOSE is subscribed and then discarded. | [VulkanContext.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp) `PopulateDebugMessengerCreateInfo`, `CreateInstance` | 1 |
| B4 | Small header bugs: stage-name table order, `RHIShaderStageFlagToString` misses Geometry and pops an empty string, `AddAttachments` assert is off by one, `SetStageSPIRV` never increments its count, transition structs leave enum fields uninitialized, `static` functions defined in headers. | [RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h), [RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h) | 1 |
| B5 | The legacy render-pass path caches framebuffers by a 32-bit layout hash that omits attachment identity and never invalidates them. It is unreachable because dynamic rendering is forced on. `m_imageLayoutCache` is written on every texture barrier and never read. Other unused legacy declarations, stubs and commented-out API remain throughout the RHI. The legacy renderer (`Graphics/Val`, the old RenderCore, the TinyGLTF loader) and its samples are kept behind `ZEN_BUILD_LEGACY` but no longer build against the current RHI. | [VulkanRenderPass.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanRenderPass.cpp), [VulkanTexture.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp) `UpdateImageLayout`, [ZenSamples/CMakeLists.txt](../ZenSamples/CMakeLists.txt), inventory in Phase 9 | 9 |
| P1 | Every draw and dispatch captures and releases a bindless epoch, taking two or three mutexes on the render thread, even when the pipeline has no bindless set. The RHI thread takes the bindless mutex again in `Flush()` for every draw. | [RHICommandList.h](../ZenCore/Include/Graphics/RHI/RHICommandList.h) `RHICommandWithBindlessEpoch`, [VulkanDescriptorPool.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp) `CaptureEpoch`/`Flush` | 3 |
| P2 | `RHICommandListBase::Execute` does a `dynamic_cast` per command; every `TO_VK_*` macro is a `dynamic_cast` and several run per draw. | [RHICommandList.cpp](../ZenCore/Source/Graphics/RHI/RHICommandList.cpp), [VulkanTypes.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanTypes.h) | 4 |
| P3 | `PreDraw`/`PreDispatch` redo descriptor resolution, retention and dynamic-offset sorting on every draw even when nothing changed. | [VulkanDescriptorSetState.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp) `BuildDescriptorSetList`, `AppendDynamicOffsetsForSet` | 4 |
| P4 | `RHIThread::Invoke` allocates a `packaged_task`, a shared pointer and a Win32 event on every call, including inline mode and calls already on the RHI thread. | [RHIThread.h](../ZenCore/Include/Graphics/RHI/RHIThread.h) | 5 |
| P5 | Final `ReleaseReference`, `IRHICommandContext::OnFinalRelease`, `VulkanBuffer::Map`/`Unmap` and `GetTextureCopyCapabilities` block the caller on an RHI-thread round trip. | [RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h), [VulkanBuffer.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanBuffer.cpp), [RHICommandListExecutor.cpp](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp) | 5 |
| P6 | Each recorded `SetShaderParameters` copies four `HeapVector`s, up to four heap allocations per command. | [RHICommandList.h](../ZenCore/Include/Graphics/RHI/RHICommandList.h) `RHICommandSetShaderParameters` | 4 |
| P7 | Completion polling queries the timeline counter once per pending workload and runs `Collect()` once per released workload. Command buffers are reset with `RELEASE_RESOURCES` on every reuse. | [VulkanQueue.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanQueue.cpp), [VulkanCommandList.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp) `Begin` | 6 |
| G1 | `pEnabledFeatures` enables every supported core feature, including `robustBufferAccess`. Extension feature structs pass back every supported bit, including capture/replay features. | [VulkanDevice.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp) `SetupDevice`, [VulkanExtension.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanExtension.cpp) | 2 |
| G2 | Uniform ring blocks use `AUTO_PREFER_HOST`, so the GPU reads per-draw uniforms from system memory even when device-local host-visible memory exists. | [VulkanMemory.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanMemory.cpp) `AllocBuffer` | 2 |
| G3 | The viewport depth buffer prefers `D32_SFLOAT_S8_UINT`, but no active RenderCore pipeline enables stencil testing. | [VulkanDevice.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp) `GetSupportedDepthFormat` | 2 |
| D1 | The descriptor-set cache searches only 2 of its 3 ring slots (512 sets each) and evicts first-in-first-out regardless of use. `SetPipeline` discards all descriptor state on every pipeline change, even between compatible layouts. | [VulkanDescriptorPool.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanDescriptorPool.h), [VulkanDescriptorSetState.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp) `SetPipeline` | 7 |
| D2 | Buffer sizes and offsets are 32-bit throughout the RHI. Float specialization constants are stored as `int`. | [RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h), [VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp) | 8 |

## 3. Invariants to preserve

1. The lifetime contracts in [RHI/README.md](../ZenCore/Include/Graphics/RHI/README.md) stay intact. Raw resource arguments remain borrowed; command lists retain referenced resources until reset, rollback or destruction. Earlier command lists keep their bindless protection until reset, rollback or destruction, and old parameter commands can still replay an identical registration under their captured interval.
2. CPU handoff, native acceptance, GPU completion and presentation retirement remain distinct events.
3. Inline and threaded execution behave the same. The RHI thread never waits for RenderCore; Windows waits keep servicing sent messages.
4. Native objects are still created, translated and destroyed on the RHI thread (`CheckOwnership`).
5. A performance change lands only with a comparable measurement showing improvement or neutrality, and with no new validation or synchronization-validation errors.
6. Failure behavior is unchanged. Add no new error reporting, result types or failure paths. Where a change passes through an existing failure path (for example resource-type validation in 4.2, or `Map` returning null for non-host-visible memory in 5.3), preserve its current observable behavior exactly.
7. Apply the repository C++ rules to every modified function: one return at the end, explicit types except iterators, short necessary lambdas, project containers such as `HeapVector`, shared helpers for repeated logic, no new exception-based control flow, and the project `.clang-format` (`tools/format_active.py`).

## 4. Implementation phases

Each phase leaves the tree building and its affected suites passing, and can be reviewed on its own.

### Phase 0 — Baseline and counters

1. Build the `x64-windows-msvc-release` preset. Capture `scene_renderer_demo` with `--mode=3` and `--mode=2`, each with `--rhi-thread=0` and `--rhi-thread=1`:

   ```powershell
   .\build\x64-windows-msvc-release\bin\scene_renderer_demo.exe --mode=3 --frames=600 --warmup=60 --fixed-step --vsync=0 --disable-validation --rhi-thread=1 --profile=build/profiles/rhi-baseline-m3-t1
   ```

2. Record median and p95 CPU frame time and GPU frame/pass times (`*.frames.csv`, `*.passes.csv`), plus the RHI execution time the demo already prints from `RHIThreadMetrics`.
3. Add cheap per-frame counters (relaxed atomics or RHI-thread-only integers) exposed through the existing metrics path: native draws/dispatches, `vkQueueSubmit` calls, descriptor-cache hits/misses/inserts/slot retirements, and bindless epoch captures. Keep them permanently only if their cost is not measurable.
4. Record the passing test list for `VulkanRHITest`, `VulkanRHIIntegrationTest`, `RenderCoreTest` and `CommonTest`.

Exit: baseline numbers and environment recorded in `Doc/RHIImprovementVerification.md`.

### Phase 1 — Correctness fixes

1. **Transfer context ownership (B1).** Make `GetTransferCommandContext()` return a new context on every call, as `GetCommandContext()` does, and remove `DynamicRHI::m_pTransferContext`. Update the executor wrapper and the fake backend in [RenderCoreTests.cpp](../ZenSamples/RenderCoreTest/RenderCoreTests.cpp). Rejected alternative: keeping one shared context behind a `RefCountPtr` would let two command lists share one context's workloads and native recording state.
   - Test: create and destroy a command list from a transfer context twice within one backend lifetime (extend [VulkanUploadIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanUploadIntegrationTests.cpp)).
2. **Format table (B2).**
   - Make `GetTextureFormatPixelSize` cover every `DataFormat` enumerator, and add named `eB8G8R8A8UNORM` / `eB8G8R8A8SRGB` values for the swapchain formats that are currently cast in unnamed. Return `0` for unknown formats instead of `0x7fffffff`.
   - Change `eR8G8B8UNORM` to `23` (`VK_FORMAT_R8G8B8_UNORM`) and confirm the RenderCore tests that use it.
   - Replace the duplicated switches in `RDGResourceManager::EstimateBytes` and `CopyTexelBytes` with the shared table. Keep the aspect-specific copy rules in `RDGCopyValidation.h`: copy sizes per aspect differ from memory footprint for combined depth/stencil formats.
   - In `VulkanTexture`, delete `CalculateTextureSize` and decide small-pool placement from the image's actual 64-bit memory requirements.
   - Tests: every enumerator returns a nonzero size; BGRA8 and depth formats produce correct RDG estimates.
3. **Validation messenger (B3).** Add `RHIOptions::SetDebugPrintfEnabled` (default off) and a `--validation-printf` demo flag. Request `VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT` only when it is set; then subscribe to INFO and log Debug Printf messages with `LOGI`. Drop the VERBOSE subscription. The `GL_EXT_debug_printf` line in `voxelization_large_triangles.comp` is unused and can be removed the next time shaders are recompiled.
4. **Header bugs (B4).** Fix the stage-name table order; include Geometry and guard the empty case in `RHIShaderStageFlagToString`; change the `AddAttachments` assert to `<=`; increment `m_stageCount` only when the stage is new; give `RHITextureTransition`/`RHIBufferTransition` enum fields defaults; change header `static` functions to `inline`.

Exit: affected suites pass; a validation-enabled demo run shows no new messages.

### Phase 2 — Device configuration (GPU)

1. **Explicit feature set (G1).**
   - Inventory what the engine needs: `OpCapability` declarations across `Data/SpvShaders` (via `spirv-dis` from the Vulkan SDK) and pipeline/sampler state that requires a feature. Known starting points: geometry shaders in VoxelGI, voxelization storage writes (`fragmentStoresAndAtomics`, possibly `vertexPipelineStoresAndAtomics`), `samplerAnisotropy`, and `fillModeNonSolid` / `depthClamp` because RenderCore pipeline keys include `wireframe` and `enableDepthClamp`.
   - Build `VkPhysicalDeviceFeatures` from a required list plus an optional list enabled only when supported; publish optional results in `RHIGPUInfo`. Keep `GetUnsupportedReason` consistent with the required list.
   - Leave `robustBufferAccess` off by default, with an `RHIOptions` switch for debugging out-of-bounds access.
   - For extension features, copy the queried struct into an enabled struct containing only the bits the engine relies on. Do not enable capture/replay or multi-device bits.
   - Exit: validation-enabled runs of every suite and demo mode report no missing-feature errors; the GPU pass-time comparison is neutral or better.
2. **Depth format (G3).** Prefer `D32_SFLOAT` for the viewport depth buffer, falling back to `D32_SFLOAT_S8_UINT` and then `D24_UNORM_S8_UINT`. Document the change in the RHI README. Compare `--capture=frame.ppm` output before and after for each mode.
3. **Uniform ring placement (G2).** Add an allocation type for CPU-written, GPU-read data (for example `eCPUWriteGPURead`). Use `VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE` with sequential-write host access and required `HOST_VISIBLE | HOST_COHERENT`, so VMA selects device-local host-visible memory when present and falls back otherwise. Use it only for `VulkanUniformBufferAllocator` blocks; staging keeps `eCPUWrite`. Check fallback behavior when the 256 MiB BAR heap is exhausted on systems without ReBAR.
   - Exit: GPU pass times improve or stay neutral on a discrete GPU and are unchanged on integrated GPUs.
4. **Small allocation pools (optional).** VMA already sub-allocates small resources. Measure committed memory with and without the custom small pools and keep them only if they reduce it.

### Phase 3 — Bindless epochs without per-draw locks (P1)

1. Make the bindless manager's current epoch readable without its mutex: an atomic written under the lock with release ordering and read with acquire ordering.
2. Move capture ownership from individual commands to `RHICommandList`. The list keeps a small ordered set of captured epochs. On a draw, dispatch or bindless parameter command, read the current epoch; capture through `RHICaptureBindlessEpoch` only when it differs from the last captured epoch. Commands keep only the epoch value used by `RHISetRecordedBindlessEpoch` and no longer release anything in their destructors.
3. Release captures in `Reset`. Extend `CommandCheckpoint` with the capture count so `RollbackCommands` releases only captures added after the checkpoint. `DetachCommands` moves captures together with the commands they protect.
4. Correctness argument: a command may reuse an epoch captured earlier by its list. Holding an older epoch only extends protection, because a slot retired at epoch E is reclaimed only after every epoch up to and including E completes. Commands recorded after a retirement do not publish the retired index, per the existing README contract.
5. Gate `Flush()` with an atomic pending-writes flag, set under the lock when writes are queued and cleared after `WriteDescriptorSetBatch`, so draws with nothing to flush take no lock.
6. Tests: a list recording N draws performs one capture; a retirement while a list holds a capture defers reclamation until reset or retirement; rollback releases only later captures; a detached batch protects its slots until batch retirement; `ResetBindlessResources` is still rejected while a list holds a capture. Run [VulkanBindlessRetirementIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanBindlessRetirementIntegrationTests.cpp), `VulkanBindlessSceneResetTests.inl` and [RHIThreadingTests.inl](../ZenSamples/RenderCoreTest/RHIThreadingTests.inl).

Exit: zero bindless lock acquisitions on the render thread in steady-state frames (Phase 0 counter); CPU recording time reduced.

### Phase 4 — Translation hot path (P2, P3, P6)

1. **Command dispatch.** Use `static_cast<RHICommand*>` in `Execute`. Require `RHICommand` in `AllocateCmdTyped`'s `static_assert` and remove the unused untyped `AllocateCmd`.
2. **Vulkan casts.** Replace the `TO_VK_*` macros with inline functions that `static_cast` and, in debug builds, verify `GetResourceType()`. Where code relies on a null result for the wrong type (`ValidateBindingState`, `AppendDescriptorBindingWrites`, `GetImageView`, `GetBindlessTextureView`, `IsValidBindlessResource`), check `GetResourceType()` explicitly first. Add a negative test: binding a texture where a sampler is expected still fails validation.
3. **Descriptor fast path.**
   - Precompute each set's ordered dynamic-offset slots (binding and element count) in `VulkanShader::Init`, replacing the per-draw scan and `stable_sort`.
   - Give `VulkanCommandContextBase` a workload generation incremented in `StartWorkload`.
   - In `FlushPendingDescriptorWrites`, reuse the previous set list and offsets and skip retention when no set or packed value is dirty, the pipeline, cache revision and uniform generations are unchanged, and the workload generation matches the last flush.
   - In `RecordLifetime`, compare against the most recently recorded ID before the linear search.
   - RenderCore's scene loops change only push constants between draws, so they should take the fast path.
4. **Index buffer.** Cache the bound index buffer, offset and type in `FVulkanCommandBuffer`, invalidated by `InvalidateCachedState` and `Begin`.
5. **Shader-parameter commands.** Introduce an `RHIShaderParameterView` (spans for value parameters, value bytes, resource parameters and bindless parameters) and change `IRHICommandContext::RHISetShaderParameters` to take it. The recorded command copies the arrays into the command arena with `AllocateCmdData`. `RHIBatchedShaderParameters` stays the RenderCore-facing builder and produces a view. Update the fake contexts in RenderCore tests.
6. **Small allocations.** Use fixed `MAX_NUM_COLOR_ATTACHMENTS` arrays in `RHIBeginRendering` and `SmallVector` storage in `VulkanPipelineBarrier`.

Exit: [VulkanBindingIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanBindingIntegrationTests.cpp), [VulkanRecordingIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanRecordingIntegrationTests.cpp), [VulkanDescriptorIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanDescriptorIntegrationTests.cpp) and [VulkanDescriptorLifetimeTests.cpp](../ZenSamples/CommonTest/VulkanDescriptorLifetimeTests.cpp) pass; RHI execution time per frame is reduced. Optionally add a non-gating benchmark that records 10,000 draws through the inline executor.

### Phase 5 — RHI thread round trips (P4, P5)

1. **`Invoke` fast path.** When `IsCurrentThread()` is true, call the function directly. Exceptions propagate as they do today through `packaged_task`.
2. **Asynchronous final release.** `ReleaseReference` and `IRHICommandContext::OnFinalRelease` use the existing `Dispatch` when called off the RHI thread in threaded mode and run inline otherwise. Do not add job classes, admission results or cancellation; those belong to the deferred error-handling plan.
   - FIFO order keeps destruction after any earlier queued work that used the object. Executor teardown already invokes `DeleteBackend` before `Stop`, so queued releases run first.
   - Shutdown and terminal-failure behavior stay as they are today.
   - `NotifyResourceDestroyed` now arrives later; confirm RenderCore consumers tolerate the delay ([SceneResourceLifetimeTests.inl](../ZenSamples/RenderCoreTest/SceneResourceLifetimeTests.inl)).
3. **Persistent mapping.** Create `eCPUWrite`/`eCPURead` buffers with `VMA_ALLOCATION_CREATE_MAPPED_BIT`. `Map` returns the mapped pointer without an RHI round trip; `Unmap` becomes a no-op. Map on non-host-visible memory still returns null, as today.
4. **Read-only queries.** Serve `GetTextureCopyCapabilities` without `Invoke` (`vkGetPhysicalDeviceFormatProperties` is thread-safe), or from a per-format cache filled at initialization. Combine the three `Invoke` calls in `RHICommandListExecutor::WaitForCompletion` into one.

Exit: [RHIThreadingTests.inl](../ZenSamples/RenderCoreTest/RHIThreadingTests.inl) inline/threaded parity and [RHIWindowThreadingTests.cpp](../ZenSamples/CommonTest/RHIWindowThreadingTests.cpp) pass; `--rhi-thread=1` CPU frame time is compared with the baseline.

### Phase 6 — Queue maintenance (P7)

1. In `ProcessPendingWorkloads`, query the timeline counter once and retire every workload at or below it, waiting only when required. Remove `Collect()` from `ReleaseWorkload` and run it once per processing or discard call.
2. Reset command buffers with flags `0` instead of `RELEASE_RESOURCES`; `FreeUnusedCommandBuffers` already trims buffers idle for 10 seconds.

Exit: [VulkanQueueIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanQueueIntegrationTests.cpp) and [VulkanCommandBufferLifetimeTests.cpp](../ZenSamples/CommonTest/VulkanCommandBufferLifetimeTests.cpp) pass; both the timeline and fence submission paths are exercised.

### Phase 7 — Descriptor cache (gated by measurement, D1)

Proceed only if Phase 0 counters show slot retirements every frame or steady-state misses for a supported scene.

1. Search every live ring slot, or promote entries hit in the oldest slot.
2. In `SetPipeline`, clear only sets whose layout ID differs from the previous pipeline.
3. Build content keys in a reused scratch key and hash incrementally, removing the per-resolve heap allocations in `BuildContentKey`.

Exit: lower miss and insert counts; [VulkanDescriptorLifetimeTests.cpp](../ZenSamples/CommonTest/VulkanDescriptorLifetimeTests.cpp) still passes.

### Phase 8 — API width and hygiene (D2)

1. Make buffer sizes and offsets 64-bit: `RHIBufferCreateInfo::size`, `GetRequiredSize`, `RHIClearBuffer` (with an explicit whole-size value), `VulkanBuffer::GetOffset`, and the `AllocBuffer`/`AllocImage` size parameters.
2. Store specialization constants as a raw 32-bit value plus type, so float constants can hold fractional values.
3. Update [RHI/README.md](../ZenCore/Include/Graphics/RHI/README.md) for every contract change in this plan.

### Phase 9 — Legacy code cleanup (last, B5)

Start only when Phases 0–8 are finished (Phase 7 either done or skipped by its gate) and their results are recorded in the verification document. Doing the cleanup last means the earlier phases are reviewed against the code as it exists today, and the cleanup itself is a behavior-neutral change set.

Goal: leave one renderer. Delete the legacy Vulkan wrapper (`Graphics/Val`), the old RenderCore, the legacy glTF loader and every `ZEN_BUILD_LEGACY` sample together with their build plumbing, dependencies, shaders and documentation. Then remove every remaining legacy or unused code path from `Graphics/RHI` and `Graphics/VulkanRHI`, leaving dynamic rendering, the recorded command list, and the executor as the only paths. Git history keeps the deleted code.

Rules for the whole phase:

1. Confirm every deletion with a usage search and a clean build of the default presets, not by name alone. The default-built targets after this phase are ZenCore, ZenUI, `scene_renderer_demo`, `GLTFCorpusImport` and the test executables.
2. Keep APIs that are lightly used but active or tested, for example `ResolveTexture` (covered by `VulkanPipelineIntegrationTests`) and `RHIDebug` (used by `RenderDevice` and `TextureManager`).
3. Delete in small groups, each building and passing tests on its own. Do 9.1 before 9.2 so the RHI inventory reflects the final tree.

#### 9.1 Remove the legacy renderer and samples

1. **Detach active code first.** `RenderCore/V2/ComputeVoxelizer.cpp` includes `Graphics/Val/CommandBuffer.h` but uses nothing from it (`DrawIndexedIndirectCommand` comes from `RenderCoreDefs.h`). Remove the include and add any standard or Vulkan headers that were only reaching it transitively. At `d22e0641` this is the only active include of legacy code; re-run the search before deleting.
2. **Delete legacy engine code.**
   - `ZenCore/Include/Graphics/Val` and `ZenCore/Source/Graphics/Val` (46 files).
   - The old RenderCore: the 10 headers directly in `ZenCore/Include/Graphics/RenderCore/` (`RenderBuffers.h`, `RenderConfig.h`, `RenderContext.h`, `RenderDevice.h`, `RenderFrame.h`, `RenderGraph.h`, `RenderGraphDefinitions.h`, `ResourceCache.h`, `ShaderManager.h`, `TextureManager.h`) and the 5 sources directly in `ZenCore/Source/Graphics/RenderCore/`. `RenderCore/V2` stays.
   - The TinyGLTF loader: `AssetLib/GLTFLoader.h` and `GLTFLoader.cpp`. `FastGLTFLoader` stays.
3. **Delete legacy samples** (25 tracked files):
   - `ZenSamples/ZenCoreTest`.
   - `ZenSamples/VulkanRHIDemo/{HelloTriangle, Gears, PushConstants, SpecializationConstants, Offscreen, ShadowMapping}` and the shared `VulkanRHIDemo/Application.h/.cpp`. `VulkanRHIDemo/SceneRenderer` stays at its current path, because tools and docs refer to it.
   - `ZenSamples/Applications` (`HelloTriangle`, `GLTFViewer`, `SceneGraphDemo`, `Application.h`).
4. **Remove the build plumbing.**
   - Remove the `ZEN_BUILD_LEGACY` option from the root [CMakeLists.txt](../CMakeLists.txt).
   - In [ZenCore/CMakeLists.txt](../ZenCore/CMakeLists.txt), remove the legacy files from `ZEN_CORE_HEADERS`/`ZEN_CORE_SOURCES`, the `if (NOT ZEN_BUILD_LEGACY)` filter block, and the `spirv-cross-glsl`/`tinygltf` link.
   - In [ZenSamples/CMakeLists.txt](../ZenSamples/CMakeLists.txt), remove all three `if (ZEN_BUILD_LEGACY)` blocks.
   - In [External/CMakeLists.txt](../External/CMakeLists.txt), remove the SPIRV-Cross and TinyGLTF declarations. Their download folders under `External/` are already ignored by Git.
5. **Delete shaders used only by the removed samples.** They are compiled into `Data/SpvShaders` by the shader glob, so they currently cost build time.
   - Referenced only by legacy code at `d22e0641`: `GLTFViewer/viewer.{vert,frag}`, `Offscreen/{mirror,phong}.{vert,frag}`, `SGDemo/{main.vert,pbr.frag}`, `ShadowMapping/scene.{vert,frag}`, and in `Data/Shaders/` itself `gbuffer.frag`, `gears.{vert,frag}`, `push_constants.{vert,frag}`, `triangle.vert`, `triangle_fixed.frag`, `uber.{vert,frag}`.
   - Referenced by nothing: `Environment/skybox.vert`, `Offscreen/quad.{vert,frag}`, `VoxelGI/compact.glsl`, `triangle_fixed.vert`.
   - The rest of `ShadowMapping/` (`evsm.*`, `scene_shadow.*`) and the textures in `Data/Textures` are used by active code; keep them.
6. **Update documentation.**
   - [README.md](../README.md): remove the SPIRV-Cross/TinyGLTF dependency rows and the legacy-renderer build notes. Keep the dated milestone history (M0–M2) and its images as a record of the project, and add one line stating that those demos were removed in this cleanup and remain in Git history.
   - Remove the `-DZEN_BUILD_LEGACY=ON` build note in [ActiveCodeCleanup.md](ActiveCodeCleanup.md).
   - In [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md), change the deferred "legacy `Graphics/Val` migration" item to say the legacy code was removed.

Exit for 9.1: a fresh configure of every default preset succeeds without fetching SPIRV-Cross or TinyGLTF; Debug and Release build; the full test suites and `scene_renderer_demo` modes pass; a search for `ZEN_BUILD_LEGACY`, `Graphics/Val`, `zen::val`, `tinygltf` and the deleted sample names finds no references outside Git history and the verification document.

#### 9.2 Remove unused RHI code

Inventory every declaration in `ZenCore/Include/Graphics/RHI`, `ZenCore/Include/Graphics/VulkanRHI` and their sources that has no caller left in the default-built targets after 9.1.

Candidates found at `d22e0641` (re-confirm after Phases 1–8 and 9.1, which may add or remove users):

| Group | Candidates |
| --- | --- |
| Legacy render-pass path | `VulkanRenderPass.h/.cpp` (`VulkanRenderPassBuilder`, `VulkanFramebuffer`); `GetOrCreateRenderPass`, `GetOrCreateFramebuffer`, `m_renderPassCache`, `m_framebufferCache`; `VulkanViewport::GetCompatibleFramebufferForBackBuffer` and `m_framebuffer`; the non-dynamic branches in `RHIBeginRendering`, `RHIEndRendering`, `EndWorkload` and `InitGraphics`; `RHIOptions::useDynamicRendering` and `UseDynamicRendering()` (update its caller in `RenderDevice.cpp`); `RHIRenderPassLayout`, `RHIFramebufferInfo`, `RHIRenderPassClearValue` and its `ToVkClearColor`/`ToVkClearDepthStencil` overloads; `MAX_NUM_SUBPASSES`; `RHIGfxPipelineCreateInfo::subpassIdx` and `RHIPipeline::m_subpassIdx`; `RHIRenderingLayout::GetHash32`, `GetRHITextureData` and `GetRHIRenderTargetClearValueData`; `TO_VK_FRAMEBUFFER` and `TO_VK_RENDER_PASS`. |
| Write-only state | `m_imageLayoutCache`, `UpdateImageLayout`, `RemoveImageLayout`, `GetImageCurrentLayout`; `RHIPipeline::m_pRenderingLayout` (read only during `Init`; pass the layout to `Init` instead of storing a pointer that later dangles); `RHITexture::m_pBaseTexture`. |
| Old handle API | `Handle`, `RHI_DEFINE_HANDLE`, `HASH_DEFINE` and the commented-out handle list in `RHIDefs.h`; the platform headers' `RHIDefs.h` includes if nothing else in it is used. |
| Deprecated commands | `AddTextureTransition`, `RHICommandAddTextureTransition` and `RHIAddTextureTransition` (the Vulkan implementation only logs an error); update the fake context in `RenderCoreTests.cpp`. |
| Unused shader helpers | `RHIShaderGroupSource`, `RHIShaderLanguage`, the stub `RHIShaderUtil::CompileShaderSourceToSPIRV`; `PrintShaderGroupInfo` (keep only if wanted as a debugging aid); unused `GetHash32` methods on `RHITexture`, `RHIShader` and `RHIShaderGroupSPIRV`. |
| Unused options and helpers | `RHIOptions::VKUploadCmdBufferSemaphore`, `WaitForFrameCompletion`, `MaxDescriptorSetPerPool` and their fields; `RHISamplerInfo` (duplicate of `RHISamplerCreateInfo`); the `ALLOCA` macros; `VulkanFenceManager::WaitAndReleaseFence`; secondary command-buffer stubs (`VulkanCommandBufferType::eSecondary` is never allocated). |
| Commented-out code | Old handle-based API and dead blocks in `DynamicRHI.h`, `VulkanRHI.h`, `RHICommon.h`, `RHICommandList.h` (`PRIVATE_ARRAY_DEF`), `VulkanBuffer.cpp`, `VulkanTexture.cpp`, `VulkanPipeline.cpp`, `VulkanViewport.cpp` and `VulkanContext.cpp`. |

Also update [RHI/README.md](../ZenCore/Include/Graphics/RHI/README.md): remove the sentences about deferred legacy render-pass/framebuffer fixes and describe dynamic rendering as the only path. Remove deleted sources from [ZenCore/CMakeLists.txt](../ZenCore/CMakeLists.txt), and from [ZenSamples/CMakeLists.txt](../ZenSamples/CMakeLists.txt) where test targets compile RHI sources directly.

Exit for 9.2: default presets build in Debug and Release with no new warnings; the full final regression in section 6 passes; a usage search finds no references to the deleted names; the verification document lists what was removed in 9.1 and 9.2.

## 5. Deferred work and separate designs

These items were outside this plan's scope. Phases 0–6, 8 and 9 are implemented and Phase 7 was rejected by its gate; see [RHIImprovementVerification.md](RHIImprovementVerification.md). This section is the guide for the remaining work. Each item states the current code, what to measure before starting, a design outline, what must not change, and when it is done. Current-state descriptions were checked against `e79660ec`; re-check them before starting.

**Status (2026-10-02).** This section was executed with its gates; results are in [RHIDeferredWorkVerification.md](RHIDeferredWorkVerification.md). 5.1 option A, 5.7 and 5.8 are implemented. 5.6 and 5.11 were measured: the pipeline-cache file was removed again because it did not speed up startup, and the small pools were deleted because they added memory. The gates of 5.1 option B, 5.2 and 5.4 did not open. 5.5 traced the per-frame miss to a RenderCore pool budget; its fix awaits a RenderCore decision. 5.3, 5.9 and 5.10 were not started, by decision. Each item below starts with its outcome; the rest of its text is the original guidance.

### 5.0 Rules for every item

1. **Measure first when an item has a gate.** Repeat the Phase 0 profile at the current revision before changing code: Sponza, voxel resolution 256, 1280 × 720, `--mode=2` and `--mode=3`, `--rhi-thread=0` and `1`, 60 warm-up and 600 measured frames, `--fixed-step --vsync=0 --disable-validation`. Add `--async-compute=1` runs for items that change submission. Compare the `cpu_frame_ms`, `rhi_execution_ms`, `gpu_frame_ms`, `submissions` and `descriptor_*` columns of `*.frames.csv`. CPU medians differed by up to 16% and GPU medians by under 1% between the two final batches of this plan, so smaller CPU gains need repeated before/after pairs.
2. **Keep a change only if it helps or is neutral** and adds no validation or synchronization-validation errors (section 3, invariant 5). Items without a performance gate must be neutral.
3. **Section 3 invariants and the [RHI README](../ZenCore/Include/Graphics/RHI/README.md) contracts apply.** Update the README for every contract change.
4. **Validate as in section 6.** Run both RHI thread modes, and both the timeline and fence submission paths when submission changes. Behavior-neutral items must keep the three mode captures byte-identical.
5. **Record results in a new verification document per item.** `RHIImprovementVerification.md` stays the record of the executed phases.

| Item | Kind | Gate before implementation | Size | Depends on | Outcome (2026-10-02) |
| --- | --- | --- | --- | --- | --- |
| 5.1 Presentation copy | CPU and GPU cost | Option A: none. Option B: copy measurable in GPU time | A: S–M, B: L | — | A implemented; B gate closed |
| 5.2 One submit per queue per frame | CPU cost | Submission overhead measurable with async compute on | M | 5.1 A | Gate closed |
| 5.3 Synchronization2 | Barrier precision | Must be neutral; start when per-barrier stages are needed | M | — | Not started |
| 5.4 Per-frame command pools | CPU cost | Command-buffer acquire/reset measurable | M | — | Gate closed |
| 5.5 Descriptor binding model | CPU cost | Diagnose the per-frame miss; descriptor work measurable in a heavier scene | S (diagnosis) to L | — | Diagnosed; RenderCore fix pending |
| 5.6 Persistent pipeline cache | Startup time | Warm start measurably faster | S | — | Measured, not kept |
| 5.7 Dynamic uniform-buffer limit | Correctness guard | None | S | — | Implemented |
| 5.8 VSync present mode | Product policy | A decision | S | — | Implemented: configurable, FIFO for VSync on |
| 5.9 Global singletons | Maintainability | A concrete need | L | Coordinate with 5.10 | Not started |
| 5.10 Error handling | Robustness | Separate plan | L | — | First delivery in R1-R6 |
| 5.11 Small allocation pools | Memory, simplification | The measurement decides | S | — | Deleted |

Suggested order: 5.7 before adding passes or shader resource types (for example the ray-query work); then 5.1 option A; then 5.6 and 5.11, which are small and decided by measurement; 5.8 once the policy is chosen; 5.3 together with render-graph Phase 4; 5.2, 5.4 and 5.5 only if their gates open. 5.9 and 5.10 are separate projects; do not interleave them.

### 5.1 Presentation without the backbuffer copy

**Outcome.** Option A is implemented: the copy joins the last graphics group's `vkQueueSubmit` as its own submit info, and the group's accepted serial stays exact. Option B's gate did not open: the copy measured about 8 µs of GPU time, about 0.6% of the 720p GPU frame and within run-to-run variation. Devices without timeline semaphores still make one call per workload. The review follow-up validates successful frames, rejection/retry and fence partial failure in inline and threaded execution, forcing the real fence path on the timeline-capable device. The new interleaved full-change comparison is frame-time neutral within observed variation (CPU −0.73% to +0.56%, GPU −0.30% to +0.33%); detailed median/p95 results and the isolated presentation comparison are in the verification document. The earlier VSync pacing check at 97% GPU load was unchanged.

**Current state.**

- RenderCore renders into a viewport-owned color backbuffer. `VulkanViewport::CreateSwapchain` creates it at the swapchain extent and format with color-attachment and transfer-source usage. Swapchain images have transfer-destination usage, plus color-attachment usage when supported, but not transfer-source usage.
- After every render group is accepted, `RHICommandListExecutor::ExecuteFrame` ([RHICommandListExecutor.cpp](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp)) takes a dedicated present command list. `VulkanViewport::PrepareForPresent` ([VulkanViewport.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp)) acquires the image, records `CopyBackBufferToSwapchainImage` (a copy, or a linear blit when extents differ), waits on the acquire semaphore at `TRANSFER` and signals the image's render-complete semaphore. That list is a separate `ExecuteBatch`. `Present()` then requires a new accepted signal on the semaphore.
- `RenderDevice::ExecuteRenderGraph`, the inline presentation path, does the same and keeps the README guarantee that a rejected copy submission preserves the acquired image for a rebuilt submission.
- These treat the backbuffer as a stable texture: `DeferredLightingRenderer` (lighting, tone map, markers), the geometry and compute voxelizers, `ZenUI`'s `UIRenderer`, the demo's capture readback, and RenderDevice state tracking (`UpdateTextureState`, `InvalidateExternalTextureState`).

**Cost today.** One full-screen copy on the GPU, plus one extra `vkQueueSubmit`, semaphore wait and `ExecuteBatch` round (finalization, progress publication, batch collection) per frame. Final profiles show 2 submissions per frame with async compute off. The copy has no GPU timing interval, so its GPU cost is unknown.

**Option A: append the copy to the last graphics submission (do this first).**

1. Acquire before the last graphics group is finalized instead of after every group is accepted. Attach the acquire wait (at `TRANSFER`) and the render-complete signal to that group's workload, and record the copy at the end of its command list.
2. Keep the backbuffer and all its consumers.
3. Define the rejection outcome: if the combined submission is rejected, neither rendering nor the copy was accepted, and the acquired image stays held for the next attempt, as today. Remove the separate present-list path from both the executor and the inline path.
4. The CPU now acquires earlier. Check frame pacing with VSync on, where acquisition can block.

Done when `submissions` is 1 per frame with async compute off; captures are byte-identical; swapchain integration tests and resize, minimize and out-of-date smoke runs pass with synchronization validation.

**Option B: render the final passes into the swapchain image.** Start only if a GPU timing interval around the copy shows it is measurable relative to `gpu_frame_ms` at the target resolutions.

1. Acquire before translating the first pass that writes the final image. Import the acquired image into the render graph each frame as an external texture with its own layout tracking (undefined → color attachment → present).
2. Require color-attachment usage on the swapchain, falling back to the copy when unsupported. Add transfer-source usage if capture reads the image; otherwise keep a capture-only copy.
3. Move each backbuffer consumer above to the imported image, or keep the offscreen backbuffer for passes that need it and copy only then.
4. Handle extent mismatch (today's blit) by recreating before rendering, or by using the copy path for that frame.
5. Replace the rejected-copy retry guarantee with an explicit rule for a rejected frame that holds an acquired image.

**Preserve.** The README presentation contracts: only successful or suboptimal acquisition publishes an image; presentation requires a new accepted graphics-queue signal; timeout skips, out-of-date recreates and surface loss replaces; zero-size suspension keeps the last usable backbuffer; present fences or reacquisition, not graphics completion, retire presentation semaphores.

### 5.2 One `vkQueueSubmit` per queue per frame

**Outcome.** Gate closed. Steady-state frames have one group even with async compute enabled, so after 5.1 A they make one submission. A `vkQueueSubmit` costs about 16–18 µs, so revisit this item for a workload whose steady frames have several groups.

**Current state.**

- `ExecuteGroups` submits each render group on its own. `ResolvePredecessors` requires every predecessor serial to be accepted already. `ExecuteBatch` then finalizes, submits and flushes that group, publishes progress and collects completed batches. The presentation copy is one more `ExecuteBatch`.
- Within one flush, `VulkanQueue::SubmitWorkloadsWithTimelineSemaphore` ([VulkanQueue.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanQueue.cpp)) already merges adjacent workloads without semaphores and submits everything pending in one call with several `VkSubmitInfo`s.
- `RHISubmissionState::Accept` records each group's acceptance separately. `kLatestSubmitted` resolves against accepted serials.

**Gate.** Only if submission cost matters. Time each group's `ExecuteBatch` (or `vkQueueSubmit` itself) and measure with `--async-compute=1`, where frames have several groups. After 5.1 option A, frames without async compute already make one submission, so this item concerns only multi-group frames.

**Design.**

1. Reserve each group's serial on its queue before submitting anything, and resolve predecessors in the same batch to reserved serials. Timeline semaphores allow a wait to be submitted before its signal, so a group no longer has to be accepted before the next one is translated.
2. Finalize every group, then make one `vkQueueSubmit` per queue. Acceptance becomes per call. Define how `RHISubmissionState` records several groups accepted by one call, and how an accepted first queue followed by a failed second queue is reported; that is the error-handling plan's accepted-prefix (`eFatal`) case.
3. Publish progress and collect batches once per frame rather than once per group.
4. Keep the current per-group path for devices without timeline semaphores.

**Preserve.** Serials advance only for accepted native submissions; each queue keeps its own serial timeline; `kLatestSubmitted` never refers to future work; lifetime-tracker and bindless recording counts move to serials only on acceptance; `RenderSubmissionHistory` receipts stay exact.

**Done when** submissions per frame equal the number of queues used, and the async-compute smoke matrix, executor tests and queue tests pass on both submission paths.

### 5.3 Synchronization2 migration

**Outcome.** Not started, by decision. Its gate is still render-graph Phase 4 or another consumer of per-barrier stages.

**Current state.** The backend targets Vulkan 1.2 (`ZEN_VK_API_VERSION`; device selection rejects older devices). `VulkanPipelineBarrier` ([VulkanSynchronization.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSynchronization.cpp)) issues `vkCmdPipelineBarrier` with one source and one destination stage mask for every barrier in the call. `RHIPipelineStageFlagBits` in [RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h) is a 32-bit enum of Vulkan 1.0 stages. Submission uses `VkSubmitInfo` with `VkTimelineSemaphoreSubmitInfo`. The [synchronization simplification plan](SynchronizationSimplificationPlan.md) left this migration out of scope.

**Why.** Synchronization2 gives:

- per-barrier stage and access masks (render-graph Phase 4 lists per-barrier stages);
- finer stages, such as copy, blit, clear and resolve instead of `TRANSFER`;
- 64-bit flags for newer stages;
- per-semaphore stage masks in `vkQueueSubmit2`.

The ray-query work does not need it: acceleration-structure build stages also exist in the legacy flags.

**Gate.** None for performance; the change must be neutral. Start it with render-graph Phase 4 or when another consumer needs per-barrier stages.

**Design.**

1. Require `VK_KHR_synchronization2` (or Vulkan 1.3) and its feature, and add it to the README's device requirements. Confirm availability on the macOS target first. Keeping both barrier paths would double the testing burden.
2. Translate RHI stages and accesses to `VkPipelineStageFlags2` and `VkAccessFlags2` in `VulkanPipelineBarrier`, carrying masks per barrier in `VkDependencyInfo`. Keep the existing RHI enum values; widen the enum only when adding stages.
3. Move the timeline and fence submission paths to `vkQueueSubmit2`, and update the presentation copy's barriers.
4. Leave per-barrier stage generation in the render graph to its Phase 4.

**Done when** captures are byte-identical, full suites and the smoke matrix pass with synchronization validation, and GPU pass medians stay within run-to-run variation.

### 5.4 Per-frame command pools

**Outcome.** Gate closed. Command-buffer setup costs about 4–6 µs per buffer and ending one about 1 µs, roughly 10–15 µs per frame; per-frame pools could save only part of that, below CPU frame-time variation.

**Current state.**

- Each command context takes an `FVulkanCommandBufferPool` from its queue's pool cache (`VulkanQueue::AcquireCommandBufferPool`) and returns it when destroyed. Pools use `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`.
- `FVulkanCommandBuffer::Begin` resets each buffer individually with flags 0 (Phase 6). `SetupNewCommandBuffer` scans the pool's in-use list for a ready buffer under the pool mutex. `FreeUnusedCommandBuffers` frees buffers idle for 10 seconds (all in [VulkanCommandList.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp)).
- Contexts are not tied to frames. Transfer contexts are created per request, and presentation lists live until executor destruction.

**Gate.** Add a counter or CPU timer for command-buffer acquisition, reset and begin. Proceed only if it is a measurable share of `rhi_execution_ms`. Steady-state frames make two submissions, each with at least one command buffer, so the expected gain is small.

**Design.**

1. Create pools per queue and frame slot, without the reset-buffer flag, and allocate frame command buffers from the current slot's pool.
2. Reset a slot's pools with `vkResetCommandPool` once that slot's required serials on every queue have completed. RenderCore already uses that requirement for frame-slot reuse.
3. Keep a separate pool path for work that is not frame-scoped: uploads, standalone graphs, transfer contexts and the retained presentation lists.
4. Abandoned or uncertain submissions keep their command buffers until teardown, as established in `b28c1f0d`.

**Done when** command-buffer lifetime and queue tests pass on both submission paths, and `rhi_execution_ms` improves or is neutral.

### 5.5 Descriptor binding model

**Outcome.** Diagnosed. The per-frame miss is the deferred-lighting set: `offscreen_albedo` and `offscreen_roughness` get new texture objects every frame. The render graph's 256 MiB transient-pool budget is smaller than two frame slots of the default 2048² G-buffer, so each trim evicts those targets. A 512 MiB budget removes every miss without changing frame time. The fix, together with a screen-sized G-buffer, is planned in [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md). The gate for further descriptor work did not open.

**Current state.**

- Scene materials are already bindless-first. Textures come from the global bindless heap (set 0), material data from a `MaterialBuffer` storage buffer, and per-draw changes are push constants holding node and material indices; see `Data/Shaders/Common/material.glsl` and `Data/Shaders/SceneRenderer/offscreen.vert`.
- The remaining cached descriptor sets are per-pass: camera uniforms, node/material/UV buffers, G-buffer inputs and voxel GI resources.
- The final counters per 600 frames are 600 misses and 600 inserts (one per frame), 3,600 (PBR) or 4,800 (GI) hits, and zero retirements: 7–9 set resolutions per frame. Phase 7's cache changes did not remove the per-frame miss. The verification does not identify which set misses.

**Gate.** Diagnosis needs no gate. Go further only if a heavier scene, with more passes or materials, shows descriptor resolution in `rhi_execution_ms` or misses that grow with scene content.

**Design.**

1. Diagnose first. Log the content key of each miss to find which set's identity changes every frame, for example a per-frame or transient resource that gets a new stable ID. Fix it at the source if avoidable.
2. For per-pass sets that legitimately change every frame, consider `VK_KHR_push_descriptor`. It is an optional extension with one push set per pipeline layout, bounded by `maxPushDescriptors`. Push descriptors bypass the content cache and pool lifetime tracking, but not the resource-retention contract.
3. Moving per-pass buffers to buffer device addresses is a larger redesign. Consider it with the ray-query work, which needs device addresses for acceleration structures anyway.

**Preserve.** Descriptor-pool retention stays independent of resource retention; packed uniforms keep dynamic offsets; the cache-revision and generation checks used by the Phase 4 fast path stay valid.

### 5.6 Persistent `VkPipelineCache` on disk

**Outcome.** Implemented, measured and removed. A warm 116 KB engine cache did not shorten the first frame (28.2 vs 27.9 ms PBR, 192.8 vs 189.4 ms GI): the AMD driver's own on-disk cache already provides warm starts. The tested implementation is kept as a patch with the measurements.

**Current state.** `VulkanDevice` ([VulkanDevice.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp)) creates an empty device-owned `VkPipelineCache` at initialization and destroys it at shutdown without saving. Creation failure falls back to `VK_NULL_HANDLE`. Graphics and compute pipeline creation in [VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp) use it. RenderCore keeps a separate 256-entry LRU of complete pipelines and accumulates pipeline creation CPU time in `RenderDevice`'s pipeline metrics (`creationCPUUs`).

**Gate.** Measure startup with an empty cache and a warm one: total `creationCPUUs` and time to first frame, in Debug and Release, for Sponza and the glTF smoke assets. Keep the change only if warm startup is measurably faster.

**Design.**

1. **Load.** Read the file and check an engine header (magic, format version, data size, checksum), then the Vulkan header's `vendorID`, `deviceID` and `pipelineCacheUUID` against the selected device. Pass the data to `vkCreatePipelineCache` only if everything matches; otherwise delete the file and start empty.
2. **Save.** At shutdown, call `vkGetPipelineCacheData`, write a temporary file and rename it, so a crash never leaves a partial file. Cap the file size.
3. Add an `RHIOptions` path and enable switch. Tests run with the cache disabled, so results do not depend on earlier runs.
4. Keep every native call on the RHI thread.

**Done when** unit tests reject corrupt, truncated, wrong-device and wrong-version files, and cold/warm measurements are recorded.

### 5.7 Dynamic uniform-buffer limit audit

**Outcome.** Implemented. Shader creation returns null before creating native objects when a layout exceeds the uniform-buffer limits. The RX 7900 XT driver reports 8 dynamic uniform buffers per layout, the specification minimum.

**Current state.**

- Every reflected uniform buffer becomes `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` (`VulkanTypes.cpp`), because packed values use bind-time offsets into the uniform ring.
- Nothing compares a pipeline layout with `maxDescriptorSetUniformBuffersDynamic` or `maxPerStageDescriptorUniformBuffers`. The first limit counts across all sets of a layout, with each array element counting, and its spec minimum is 8. The only related query, `VulkanDevice::GetDescriptorSetUpdateAfterBindLimit`, sizes update-after-bind pools and variable-count arrays.
- `vkCreatePipelineLayout` is unchecked (see 5.10).
- A source scan finds at most four uniform blocks per shader program today (`forward_material*`, `forward_scatter*`).

**Gate.** None. Do it before adding passes or shader resource types.

**Design.**

1. While building pipeline layouts in `VulkanPipeline.cpp`, count dynamic uniform-buffer descriptors per layout and per stage and compare them with the device limits.
2. On a violation, fail creation before calling Vulkan. Log the shader, count and limit, and return null without publishing a partial object, following the README's texture-creation convention.
3. Keep the dynamic-only policy unless a shader reaches the limit. Static descriptors for zero-offset explicit bindings would change descriptor-cache identity and need their own design.

**Done when** a test shader over the limit fails cleanly and every existing shader still creates.

### 5.8 VSync present-mode policy

**Outcome.** Implemented as the explicit setting: `RHIOptions::SetPresentMode`, the `present_mode` configuration key and `--present-mode=`. VSync on now selects FIFO; VSync off is unchanged.

**Current state.** `ChoosePresentMode` in [VulkanSwapchain.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp) prefers `IMMEDIATE`, then `MAILBOX`, then `FIFO` with VSync off, and `MAILBOX` then `FIFO` with VSync on. VSync is a boolean passed to viewport creation. The demo's `--vsync` flag defaults to on, and there is no configuration-file setting. Profiling uses `--vsync=0`.

**Decision required.** This is a product choice:

| VSync-on policy | Behavior |
| --- | --- |
| `MAILBOX` first (current) | No tearing and low latency, but the GPU renders uncapped and discards frames: more power and heat, and pacing differs from the display rate |
| `FIFO` only | Conventional VSync, capped at the refresh rate and always supported, with higher latency |
| Explicit setting | A configurable present mode (`fifo`, `mailbox`, `immediate`, optionally `fifo_relaxed`), with `vsync` kept as shorthand |

Whatever the choice, fall back to `FIFO` (the only mode the spec guarantees), document the policy in the README presentation section, and update `VulkanSwapchainIntegrationTest.VSyncChoicesUseOnlyAdvertisedModes` and any other selection tests.

### 5.9 Replacing global singletons

**Outcome.** Not started, by decision; no concrete need exists yet.

**Current state.** References across ZenCore, ZenSamples and ZenUI at `e79660ec`:

| Global | References | Files |
| --- | ---: | ---: |
| `GVulkanRHI` | 192 | 24 |
| `GDynamicRHI` | 125 (69 in RenderCore) | 20 |
| `RHIOptions::GetInstance()` | 43 | 10 |
| `GRHIFrameState` | 26 | 15 |
| `GVkMemAllocator` | 17 | 7 |

`GetRHIThread()` returns a function-local static. Consequences: one device per process, tests that share global state, and hidden dependencies.

**Gate.** A concrete need: several devices, tests that run in parallel in one process, or a second backend. Without one, this is churn.

**Design.** Behavior-neutral steps, one group at a time, each with full suites and capture comparison:

1. Give Vulkan backend objects their `VulkanRHI` and `VulkanDevice` at construction. Make the allocator a `VulkanRHI` member and remove `GVkMemAllocator`.
2. Replace the `RHIOptions` singleton with an immutable settings value passed at RHI creation and readable from the device.
3. Pass `DynamicRHI&` to RenderCore through `RenderDevice` instead of using `GDynamicRHI`.
4. Make the executor own its RHI thread.

Schedule this before or after the error-handling plan, not interleaved: both change factory signatures and their callers.

### 5.10 Error and failure handling

**Outcome.** First delivery implemented through section 8 R1-R6; see [RHIProductionVerification.md](RHIProductionVerification.md). This is not completion of the entire separate error-handling plan.

The following is the original review evidence at `e79660ec`, before that delivery, from [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md). It reproduced three containment failures: an unsuccessful `vkEndCommandBuffer` reaching submission, and an undersized uniform buffer or a forced descriptor-allocation failure reaching dispatch. The original review also identified:

- `vkCreatePipelineLayout` is unchecked;
- 27 `VKCHECK` sites log and continue (for example `vmaCreateBuffer`);
- 58 `LOG_ERROR_AND_THROW` sites remain, and `RHIThread::Run` has no catch;
- a final release that arrives after `RHIThread::Stop` runs on the calling thread after the backend has been deleted.

The first delivery now contains these failures and makes worker shutdown explicit. New factories continue to return null on recoverable failure without publishing partial objects; backend initialization and internal invariant failures retain explicit fatal paths.

### 5.11 Custom small-allocation pools (from Phase 2, step 4)

**Outcome.** Deleted with the image size probe. The review follow-up completes the measurement gate: 84 interleaved runs covering Sponza modes 1–3 and all 18 glTF smoke assets show exactly 32 MiB less peak committed and peak device-local memory without the pools in every workload. Live commitments were also sampled; the glTF results are 32 MiB lower in every repeat. Full values and the one variable Sponza live sample are recorded in the verification document.

**Current state.** `VulkanMemoryAllocator` ([VulkanMemory.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanMemory.cpp)) keeps one custom VMA pool per memory type for allocations of at most 4 KiB (`SMALL_VK_ALLOCATION_SIZE`). Buffers use their requested size; images use their native requirement, probed only when the packed texel size fits. VMA already sub-allocates small resources from its default blocks. Phase 2's committed-memory comparison was not run.

**Gate and design.** Compare `committedBytes`, `peakCommittedBytes` and `deviceLocalBytes` from `GetGPUMemoryStats()` with and without the pools, across demo modes 1–3 and the glTF smoke assets. If the pools do not reduce committed memory, delete them and the image probe. Otherwise keep them and record the numbers.

## 6. Validation and delivery

| Scope | Validation |
| --- | --- |
| Each phase | Build the affected targets in Debug and Release; run the suites named in the phase; inspect production callers of changed APIs. |
| Phases 3–6 | Run `VulkanRHITest` and `VulkanRHIIntegrationTest` with validation and synchronization validation enabled. |
| Phase 9 | Behavior-neutral: run the full final regression below after each deletion group, and compare `--capture=frame.ppm` output for each mode against the post-Phase-8 capture. |
| Final regression | Build `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, `CommonTest` and `scene_renderer_demo`; run the full suites. Exercise `--mode=1/2/3`, `--rhi-thread=0/1` and `--async-compute=0/1`, including resize, minimize and shutdown, with validation on. Use `tools/validate_voxel_gi.py` and `tools/smoke_gltf_rendering.py` where they cover a changed path. |
| Performance | Repeat the Phase 0 captures with identical settings. Report median and p95 CPU frame, RHI execution, GPU frame and changed GPU passes. Report a result only when baseline and changed runs are comparable. Neutral results are acceptable for the correctness and cleanup phases. |

Record environment, commands, results and any skipped capability-dependent cases in `Doc/RHIImprovementVerification.md`. Preserve the user's working tree and staging; do not stage or commit as part of this plan.

## 7. Order and size

| Phase | Size | Depends on | Notes |
| --- | --- | --- | --- |
| 0 Baseline | S | — | Required before any performance phase. |
| 1 Correctness | S | — | Independent fixes; can land first. |
| 2 Device configuration | M | 0 | Most effort is the feature inventory. |
| 3 Bindless epochs | M | 0 | Largest expected CPU win; lands before 4.3. |
| 4 Translation hot path | M–L | 3 | 4.1, 4.2 and 4.4 are independent of 3. |
| 5 RHI round trips | S–M | 0 | No dependency on the deferred error-handling plan. |
| 6 Queue maintenance | S | 0 | |
| 7 Descriptor cache | M | 0 | Only if the counters justify it. |
| 8 API width | M | — | Touches many call sites. |
| 9 Legacy cleanup | L | 0–8 finished | Always last; behavior-neutral. 9.1 (legacy renderer and samples) before 9.2 (unused RHI code). |

Completion means B1–B4 are fixed with regression coverage, the steady-state draw path takes no bindless locks and performs no `dynamic_cast`, the device enables only the features the engine requires, and the verification document records comparable before/after measurements for every kept performance change. Phase 9 has then removed `Graphics/Val`, the old RenderCore, the TinyGLTF loader, every `ZEN_BUILD_LEGACY` sample and option, and the legacy and unused RHI code paths (B5). Error and failure handling remains as it is today.

## 8. Production-readiness TODOs

Status: implementation continued phase by phase, 2026-10-02. R1-R6, R9, R12, R15 and R16 are implemented. R7 diagnostics and R8 validation tooling are implemented, with their acceptance limits still open. R10, R11, R13 and R14 retain the explicit measurement/consumer/platform conditions below. See [RHIProductionVerification.md](RHIProductionVerification.md) for current evidence and remaining work. RHI production certification is not claimed while R7/R8 acceptance remains outstanding.

A review of `Graphics/RHI`, `Graphics/VulkanRHI` and their `RenderCore/V2` callers at `e79660ec` plus the uncommitted 2026-10-02 working tree asked whether the RHI is ready for production use. The submission, synchronization, lifetime-tracking and swapchain designs are sound, but the RHI is not ready for a shipped product: native failures can publish objects that look valid, device loss leaves no diagnostics, and the recent work has been validated on one GPU. Each item below states the problem, the evidence and when it is done. File references describe the reviewed revision.

Test evidence at review time (Debug build, Windows 10, Radeon RX 7900 XT, no overlay): `VulkanRHITest` passed 42 of 42 tests. `VulkanRHIIntegrationTest` passed 315 tests and skipped 6 for missing capabilities (4 swapchain-maintenance presentation fences, 1 EXT surface-maintenance instance dependency, 1 D24S8), with no validation messages and no leaks.

Section 3, invariant 6 (failure behavior is unchanged) applied to Phases 0–9 and does not apply here. R1–R6 deliberately change failure behavior and follow the contracts in section 2 of [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md). The rules in section 5.0 apply to every item.

| ID | Status | Priority | Item | Size | Related |
| --- | --- | --- | --- | --- | --- |
| R1 | Implemented | Blocker | Failed pipeline creation is cached as a valid pipeline | S | 5.10 |
| R2 | Implemented | Blocker | Failed buffer allocation publishes a buffer without memory | S | 5.10 |
| R3 | Implemented | Blocker | Other unchecked native object creation | M | 5.10 |
| R4 | Implemented | Blocker | Recording failures do not stop dependent native work | M | 5.10 |
| R5 | Done | Blocker | Assertions never stop execution | S | — |
| R6 | Implemented | Blocker | Exceptions and shutdown on the RHI thread | M | 5.10 |
| R7 | Manual acceptance open | Blocker | No device-loss diagnostics | M | — |
| R8 | Hardware acceptance open | Blocker | Validated on one GPU and one operating system | M | — |
| R9 | Internal RHI | Decision | Raw RHI release does not wait for the GPU | S (decision) to L | — |
| R10 | Cold-cache gate | Scale | Synchronous on-demand pipeline compilation | M–L | 5.6 |
| R11 | Heavier-scene gate | Scale | Single-threaded recording and translation | L | — |
| R12 | Implemented | Scale | No GPU memory budget | M | R2 |
| R13 | Consumer gate | Features | Missing API features | Per feature | 5.3 |
| R14 | Platform gate | Minor | Presentation requires the graphics queue | M | 5.1 |
| R15 | Implemented | Minor | Double gamma on sRGB-only surfaces | S | — |
| R16 | Implemented | Minor | 32-bit index-buffer offsets | S | Phase 8 (D2) |

### R1. Failed pipeline creation is cached as a valid pipeline

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR1Verification.md).

**Problem.** Both `VulkanPipeline::CreateObject` overloads ([VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp)) always return the object. `InitGraphics` and `InitCompute` pass `vkCreateGraphicsPipelines` and `vkCreateComputePipelines` to `VKCHECK`, which logs and continues with a null `m_vkPipeline`. `RenderDevice::GetOrCreateGfxPipeline` and `GetOrCreateComputePipeline` ([RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp)) cache that object, so every later draw binds a null pipeline and `m_pipelineMetrics.failures` never counts it. An invalid rendering layout throws instead of returning null.

**Done when** a native failure destroys the partial object and returns null, an invalid layout logs and returns null, RenderDevice never caches a failed pipeline, and an integration test forces both native calls to fail through `ScopedVulkanCall`.

### R2. Failed buffer allocation publishes a buffer without memory

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR2Verification.md).

**Problem.** `VulkanMemoryAllocator::AllocBuffer` ([VulkanMemory.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanMemory.cpp)) passes `vmaCreateBuffer` to `VKCHECK`, and `VulkanBuffer::CreateObject` ([VulkanBuffer.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanBuffer.cpp)) never checks the result. The caller receives a buffer with a null `VkBuffer`, and `Map()` returns a null pointer for CPU-visible allocations, so exhausting device memory crashes at the first write. `MapBuffer` follows the same pattern.

**Done when** `CreateBuffer` returns null on allocation failure without publishing anything, RenderCore callers (resource creation, staging blocks and uniform-ring growth) handle null, and a test forces the allocation to fail.

### R3. Other unchecked native object creation

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR3Verification.md).

**Problem.** These calls are unchecked or only logged, and their owners continue as if they succeeded:

| Call | Result handling | Owner |
| --- | --- | --- |
| `vkCreatePipelineLayout` | Unchecked | `VulkanShader::Init` ([VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp)) |
| `vkCreateShaderModule`, `vkCreateDescriptorSetLayout` | Logged | `VulkanShader::Init` |
| `vkCreateDescriptorSetLayout` | Logged | `VulkanBindlessDescriptorPoolManager::CreateGlobalBindlessDescriptorSet` ([VulkanDescriptorPool.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp)) |
| `vkCreateDescriptorPool` | Logged | `VulkanDescriptorPool` constructor |
| `vkCreateSampler` | Logged; `VulkanSampler::CreateObject` never returns null | `VulkanSampler::Init` ([VulkanTexture.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp)) |
| `vkAllocateCommandBuffers`, `vkCreateCommandPool` | Logged | `FVulkanCommandBuffer::AllocMemory`, `FVulkanCommandBufferPool` constructor ([VulkanCommandList.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp)) |
| `vkCreateFence` | Logged | `VulkanFence` constructor ([VulkanSynchronization.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSynchronization.cpp)) |

**Done when** each owner cleans up and reports failure, following the README's texture-creation convention, each factory's callers handle it, and `VKCHECK` remains only where continuing after a failure is safe.

### R4. Recording failures do not stop dependent native work

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR4Verification.md).

**Problem.** `FVulkanCommandBuffer::Begin` ignores the result of `vkResetCommandBuffer` and only logs a failed `vkBeginCommandBuffer`. `End` ignores `vkEndCommandBuffer`. Both advance the buffer state regardless, so an invalid command buffer reaches submission. `VulkanGfxState::PreDraw` and the compute equivalent return `void`, so draws and dispatches are issued after descriptor resolution or uniform preparation fails. `PreDraw` also dereferences the current pipeline without a check. The error-handling plan reproduced three of these failures: a failed end reaching submission, and an undersized uniform buffer and a forced descriptor-allocation failure reaching dispatch.

**Done when** the Recording section of the error-handling plan is delivered: a sticky error on the native context, state changes only after native success, draws and dispatches gated within the same command, failed transactions discarded and never submitted, and the three reproduced scenarios kept as regression tests.

### R5. Assertions never stop execution

**Status.** Implemented and verified, 2026-10-02. Results and commands are in [RHIProductionR5Verification.md](RHIProductionR5Verification.md).

**Problem.** `VERIFY_EXPR`, `VERIFY_EXPR_MSG` and `VERIFY_EXPR_MSG_F` ([Errors.h](../ZenCore/Include/Utils/Errors.h)) only log, in every build configuration. About 90 invariant checks in the RHI are therefore advisory, and violations appear only as log lines. `VKCHECK` ([VulkanCommon.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanCommon.h)) asserts only in Debug and discards the result of `CheckVkResult`.

**Done when** a failed verification breaks into the debugger or aborts in Debug, Release builds either reject the invalid work or take an explicit fatal path, and the full suites and smoke matrix pass with the stricter checks. Expect the change to expose violations that are only logged today; fix those before landing it.

**Implementation.** All three verification macros print their diagnostic to standard error and abort in every build, independently of logger configuration. `VKCHECK` also aborts on negative results in every build. Recoverable descriptor validation keeps its explicit false result; failed shader construction releases its initial reference before destruction. Staging misuse tests now require termination, and upload-cancellation coverage uses a valid request rejected at the frame completion gate. Debug/Release default builds, all suites, all 24 smoke runs and three identical captures pass; 16 paired profiles support neutral median frame cost. `ASSERT` keeps its standard debug-only behavior. R1–R4 and R6 subsequently deliver recoverable native failure propagation and worker shutdown.

### R6. Exceptions and shutdown on the RHI thread

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR6Verification.md).

**Problem.**

- `RHIThread::Run` ([RHIThread.cpp](../ZenCore/Source/Graphics/RHI/RHIThread.cpp)) has no `catch`. `Invoke` is safe because `packaged_task` captures exceptions, but an exception from a task queued with `Dispatch` (final resource `Destroy`, `ExecuteBeginFrame`, GPU frame timing) calls `std::terminate`.
- Factories mix exceptions and null results. Graphics pipeline layout validation, `SetTexelFormat`, `ChooseSurfaceFormat` and `RHIThread::Start` throw; texture and shader creation return null.
- `RHIThread::Dispatch` throws while `Stop` drains the queue. A final `ReleaseReference` in that window, usually from a destructor, terminates the process.
- `m_threaded` is a plain `bool` written by `Start` and `Stop` and read without the mutex by `Dispatch`, `TryDispatch` and `IsCurrentThread`.
- A final release after `Stop` runs on the calling thread after the backend has been deleted (also listed in 5.10).

**Done when** no exception can escape an RHI-thread task, factories report failure in one way, late releases during shutdown have a defined order, and `m_threaded` is atomic or immutable while the worker runs.

### R7. No device-loss diagnostics

**Status.** Manual acceptance open, 2026-10-02. [Implementation and verification](RHIProductionR7Verification.md).

**Problem.** `VK_ERROR_DEVICE_LOST` blocks further submissions, but nothing records the cause. The backend uses no `VK_EXT_device_fault`, no command-stream breadcrumbs (`VK_AMD_buffer_marker`, `VK_NV_device_diagnostic_checkpoints` or markers written with `vkCmdFillBuffer`), and no Nsight Aftermath or Radeon GPU Detective integration. A device loss on a user's machine leaves nothing to triage.

**Design outline.**

1. Enable `VK_EXT_device_fault` when available, and log its fault and vendor information on device loss.
2. Write a per-queue breadcrumb (pass or debug-label index) into host-visible memory before and after each render-graph pass. On device loss, log the last started and last completed pass on each queue.
3. Put optional vendor integrations behind an `RHIOptions` switch.

**Done when** a manual, opt-in device-loss reproduction (not part of the default suites) reports the faulting queue and pass, and frame time with breadcrumbs enabled is neutral under the section 5.0 measurement rules.

### R8. Validated on one GPU and one operating system

**Status.** Hardware acceptance open, 2026-10-02. [Implementation and verification](RHIProductionR8Verification.md).

**Problem.** Phases 0–9 and section 5 were validated only on a Radeon RX 7900 XT under Windows 10. The review run skipped the paths that other vendors take: swapchain-maintenance presentation fences (4 tests), the EXT surface-maintenance instance dependency (1) and the D24S8 depth fallback (1). An NVIDIA GeForce RTX 5080 was used only for async-compute steps 2 and 3. `VulkanMacOSPlatform` exists, but MoltenVK has never been run. Intel and mobile GPUs have no coverage.

**Done when** both RHI suites, `RenderCoreTest` and the section 6 smoke matrix pass with synchronization validation on at least one NVIDIA and one Intel GPU, the results are recorded in a verification document, and macOS is either validated or declared unsupported. Add a CI job that runs the hardware-independent suites on a software Vulkan driver such as lavapipe or SwiftShader.

### R9. Raw RHI release does not wait for the GPU

**Status.** Internal RHI, 2026-10-02. [Implementation and verification](RHIProductionR9Verification.md).

**Problem.** `RHIResource::ReleaseReference` ([RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h)) dispatches `Destroy` as soon as the count reaches zero, without waiting for GPU work. Only RenderCore's deferred retirement protects resources, and the README requires direct callers to provide their own protection. Code that records native Vulkan must also call `InvalidateCachedState` and capture bindless epochs by hand.

| Option | Consequence |
| --- | --- |
| The RHI is internal to RenderCore | Document this, and audit direct callers (ZenUI, demo capture readback, tests) for correct retention. |
| The RHI is a public API | Move serial-based deferred deletion into the RHI so that release is GPU-safe by default; RenderCore's retirement then becomes an optimization. |

**Done when** the decision is recorded in the README. For the public option, release must be GPU-safe and covered by lifetime tests on both submission paths.

### R10. Synchronous on-demand pipeline compilation

**Status.** Cold-cache gate, 2026-10-02. [Implementation and verification](RHIProductionR10Verification.md).

**Problem.** On a cache miss, `RenderDevice::GetOrCreateGfxPipeline` and `GetOrCreateComputePipeline` create the pipeline synchronously while recording. There is no asynchronous compilation and no warm-up list, so the first use of each pipeline stalls the frame. Section 5.6 showed that the AMD driver cache already speeds up warm starts, but cold starts and other drivers still stall.

**Gate.** With a cold driver cache, measure per-frame `creationCPUUs` and frame-time spikes during the first frames of Sponza and the glTF smoke assets, and after runtime GI reconfiguration. Proceed only if the spikes are visible.

**Design outline.** Compile on worker threads and skip or substitute the draw until the pipeline is ready, or record the pipeline keys a run uses and create them at load time.

### R11. Single-threaded recording and translation

**Status.** Heavier-scene gate, 2026-10-02. [Implementation and verification](RHIProductionR11Verification.md).

**Problem.** One render thread records every command list, and one RHI thread translates them. Nothing records in parallel, and there are no secondary command buffers. This is adequate for the current scenes but limits CPU throughput for heavier ones.

**Gate.** A heavier scene in which recording or `rhi_execution_ms` dominates the CPU frame.

### R12. No GPU memory budget

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR12Verification.md).

**Problem.** The VMA allocator is created without `VK_EXT_memory_budget`, and nothing tracks the operating-system budget or evicts resources when usage exceeds it. Together with R2, exceeding VRAM crashes instead of degrading.

**Done when** `GetGPUMemoryStats()` reports budget and usage per heap when the extension is available, and RenderCore has a policy for allocations near the budget. Depends on R2.

### R13. Missing API features

**Status.** Consumer gate, 2026-10-02. [Implementation and verification](RHIProductionR13Verification.md).

Add each feature only when a consumer needs it, with a test and a README entry.

| Feature | Current state |
| --- | --- |
| Non-indexed indirect draws | Only `DrawIndexedIndirect` and `DispatchIndirect` exist. |
| Indirect-count draws | No `vkCmdDraw*IndirectCount` command. |
| Queries | Only internal timestamp timing; no occlusion or pipeline-statistics queries. |
| Stencil reference | No command, and it is not a dynamic state. |
| Mesh shaders | Not supported. |
| Ray tracing | Acceleration-structure and ray-query extensions are enabled when available, but there is no acceleration-structure resource type or build command. See [HardwareRayQueryEnvironmentLightingPlan.md](HardwareRayQueryEnvironmentLightingPlan.md). |
| HDR output | Swapchain selection accepts only 8-bit formats in `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. |
| Synchronization2 | See 5.3. |

### R14. Presentation requires the graphics queue

**Status.** Platform gate, 2026-10-02. [Implementation and verification](RHIProductionR14Verification.md).

**Problem.** Presentation fails if the selected graphics queue cannot present to the surface; separate presentation queues are not implemented (README, presentation requirements). Desktop drivers expose graphics queues that can present, so this matters only on unusual platforms.

### R15. Double gamma on sRGB-only surfaces

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR15Verification.md).

**Problem.** `ChooseSurfaceFormat` ([VulkanSwapchain.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp)) prefers UNORM formats because the renderer applies gamma itself, but it falls back to `*_SRGB` formats. `VulkanViewport::CreateSwapchain` ([VulkanViewport.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp)) creates the backbuffer in the swapchain format, so on a surface that offers only sRGB formats the hardware encodes the already gamma-corrected output a second time.

**Done when** the backbuffer uses the UNORM equivalent and the presentation copy reinterprets the bits, or the tone-mapping pass skips its gamma for sRGB targets, and a selection test covers an sRGB-only format list.

### R16. 32-bit index-buffer offsets

**Status.** Implemented, 2026-10-02. [Implementation and verification](RHIProductionR16Verification.md).

**Problem.** Phase 8 widened buffer sizes and offsets to 64 bits, but `RHICommandDrawIndexed::Param` and `RHICommandDrawIndexedIndirect::Param` ([RHICommandList.h](../ZenCore/Include/Graphics/RHI/RHICommandList.h)) still carry a `uint32_t indexBufferOffset`, so index data beyond 4 GiB in one buffer cannot be addressed.

**Done when** the offset is `uint64_t` through the command, the context and `BindIndexBuffer`.

### Order

1. R5 (complete), R1 and R2. They are small, remove the worst crashes and make later failures visible.
2. R3, R4 and R6, which together are the first delivery of the error-handling plan (5.10). Schedule them before or after 5.9, not interleaved with it.
3. R7, then R8. R8 can start at any time; repeat it after R1–R6 land.
4. The R9 decision, before anything other than RenderCore uses the RHI.
5. R10–R16, when their gates open or a consumer needs them.

Completing R1–R8 makes the RHI ready for a shipped desktop product. R9–R13 decide whether it can serve as a general-purpose engine RHI.

## 9. Review follow-up and remaining work

Status: 2026-10-02. A review of the section 5 and section 8 changes found 11 defects. All 11 are fixed (9.1). Everything in 9.2 is not done.

### 9.1 Fixed review findings

| ID | Status | Finding | Fix | Regression coverage |
| --- | --- | --- | --- | --- |
| F1 | Done | R5 made `VERIFY_EXPR` release-fatal, so a 32-bit NameID hash collision between two distinct names aborted the process. | `NameRegistry::GenerateId` probes successive keys from the name's hash; every name keeps a stable probe position and its own ID. | `NameIDTest.*` with known FNV-1a collision pairs |
| F2 | Done | Under budget pressure, `BeginFrame` called `TrimPool(true)` every frame and rebuilt the whole working set. | `RDGResourceManager::TrimIdlePoolEntries` retires only entries the newest pooled build did not use. | `RenderCoreTest.IdlePoolTrimKeepsTheNewestBuildWorkingSet` |
| F3 | Done | `VulkanViewport::PrepareForPresent` recorded the copy without checking the recording status. | The copy and signal are recorded only after `EnsureRecording()`; the acquire wait stays ahead of the copy's command buffer, in the same workload. | Existing presentation and failure suites; no dedicated fault-injection test |
| F4 | Done | A missing presentation command list reported the frame as fatal and blocked submissions, although nothing was submitted. | `ExecuteGroups` reports it as rejected unless earlier work in the batch was accepted. | `RHIExecutorTest.MissingPresentationListRejectsTheFrameWithoutBlocking` |
| F5 | Done | Cleanup can exceed the RHI queue capacity, and the worker signaled free space after every pop, so a waiting `Dispatch` busy-spun. | The worker signals only once the queue is below capacity. | Existing RHI thread tests; the spin itself has no test |
| F6 | Done | Teardown leak checks aborted Release builds on exit. | `RHIOptions::SetStrictTeardownChecks` (Debug default) keeps the fatal path; otherwise teardown logs and continues. Ownership tests enable strict checks. | `VulkanProductionFailureTest.ShutdownRejectsOutstandingResourcesAndCommandContexts`; `NonStrictShutdownLogsOutstandingResourcesAndContinues` (Release only) |
| F7 | Done | A persistently failing pipeline was recompiled on every request, every frame. | RenderDevice keeps a bounded failure record per key: the first failure retries at once, later ones wait a doubling number of frames, up to 256. | `RenderCoreTest.RepeatedPipelineFailureDefersRetriesAcrossFrames`; existing retry tests unchanged |
| F8 | Done | `RHICommandListBase::Execute` made a virtual call and copied an `RHIError` per command. | Contexts bind their error record; `IRHICommandContext::HasRecordingError()` is a non-virtual check. | Existing recording-failure suites |
| F9 | Done | Bindless parameter validation was O(n²) and took the manager lock twice per parameter. | `CanRegisterBindlessResources` validates a group under one lock and finds conflicting slot claims by sorting. | Existing bindless and descriptor suites |
| F10 | Done | The breadcrumb fill path added two full-pipeline barriers per marker and serialized pass boundaries. | Each fill keeps only the barrier that waits for earlier work; `End()` provides host visibility. | Existing diagnostics tests with synchronization validation |
| F11 | Done | CI cloned the full SwiftShader history and rebuilt it on every run. | Shallow fetch of the pinned revision and a cache keyed by it. | Not run; see 9.2 (R8) |

Verification: `tools/verify_rhi_production.py --smoke` on Debug and `--smoke --diagnostics` on Release, on the RX 7900 XT with synchronization validation. Both configurations: `VulkanRHITest` 45 passed, `CommonTest` 107 passed and 1 skipped, `RenderCoreTest` 540 passed. `VulkanRHIIntegrationTest` passed 329 tests in Debug and 330 in Release (the extra one is the Release-only F6 test), each with 6 capability skips. All 12 smoke combinations (modes 1–3, `--rhi-thread=0/1`, `--async-compute=0/1`) passed in each configuration. No validation or synchronization-validation messages. Only the AMD paths ran; R8 still applies to these fixes.

### 9.2 Remaining TODOs

| Item | Status | Remaining work |
| --- | --- | --- |
| R7 acceptance | Not done | Run a deliberate device-loss reproduction on a dedicated test machine with diagnostics enabled and keep the stderr and driver report. Simulated failures do not close it. |
| R8 acceptance | Not done | Run both suites and the section 6 smoke matrix on NVIDIA and Intel; cover the paths skipped on the RX 7900 XT (swapchain-maintenance present fences, surface maintenance, D24S8). Run the SwiftShader CI workflow for the first time; it has never run, including F11's cache. Validate macOS or keep it unsupported. |
| R10 | Not done (gated) | Measure cold-driver-cache pipeline-creation spikes before choosing warmup or asynchronous compilation. |
| R11 | Not done (gated) | Profile a heavier scene in which recording or `rhi_execution_ms` dominates before changing recording concurrency. |
| R13 | Not done (gated) | Add each missing API feature with the renderer change that needs it. |
| R14 | Not done (gated) | Implement a separate presentation queue only for a supported platform that needs one. |
| 5.5 | Not done | RenderCore fix for the per-frame descriptor miss: a larger transient-pool budget and a screen-sized G-buffer ([RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md)). |
| 5.10 remainder | Not done | The rest of [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md): structured errors through the executor, typed acquire and present results, typed admission results. `PrepareForPresent` and `Present` still throw. |
| 5.3 | Not done | Synchronization2, together with render-graph Phase 4. |
| 5.9 | Not done | Global singletons; no concrete need yet. |
| 5.1 option B, 5.2, 5.4 | Not done (gates closed) | Revisit only if the workload changes, for example frames with several submission groups. |
| Housekeeping | Not done | Two nodiscard warnings in `VulkanSwapchainIntegrationTests.cpp` (lines 499 and 766). Commit the change. |
