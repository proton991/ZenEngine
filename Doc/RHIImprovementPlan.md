# RHI correctness and performance improvement plan

Status: proposed, 2026-10-01. Based on revision `d22e0641`. The findings come from a static review of the active `Graphics/RHI` and `Graphics/VulkanRHI` code. Nothing below has been measured yet. This document defines future work; it does not implement it.

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

- **Presentation without the backbuffer copy.** Render the final pass into the acquired swapchain image, or at least append the copy to the last graphics submission. This needs acquisition before group execution and a new answer for the current guarantee that a rejected copy submission preserves the acquired image for retry.
- **One `vkQueueSubmit` per queue per frame.** Timeline semaphores allow a wait to be submitted before its signal, so serials could be reserved up front. This changes `ResolvePredecessors` and per-group acceptance; do it only if Phase 0 shows submission cost matters.
- **Synchronization2 migration.** It was excluded by the synchronization simplification plan.
- **Per-frame command pools** reset with `vkResetCommandPool`.
- **Bindless-first materials or push descriptors** in place of most cached descriptor sets.
- **Persistent `VkPipelineCache` on disk**, with header validation.
- **Dynamic uniform-buffer limit audit.** Every uniform buffer is dynamic and some devices allow only 8 per pipeline layout.
- **VSync present-mode policy.** VSync on currently prefers MAILBOX over FIFO; this is a product decision.
- **Replacing global singletons** (`GVulkanRHI`, `GVkMemAllocator`, `GDynamicRHI`, the `RHIOptions` singleton) with an explicit device context.
- **Error and failure handling.** Deferred and not executed here; covered by [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md). This includes the unchecked native results and invalid-object factories found in the same review.

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
