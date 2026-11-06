# RHI full review

> Historical demo workflow: the standalone demo and its dedicated runners were removed on 2026-11-06. Commands using them apply to the original revision. Current application rendering and profiling use [ZenApp](ZenAppMigration.md).

Review date: 2026-11-05. Source revision: `259a21413d3a8d7aef4e914a15b6500463c924f7`.

This review covers the current RHI interfaces and Vulkan backend, with relevant RenderCore callers and tests. The working tree was clean at the start. The findings below describe behavior at the revision above; their evidence locations refer to that baseline. Following the review, the user authorized fixing all eight findings and recording the corrections in this same document.

Status: review complete; all eight confirmed findings (one P1 and seven P2) are corrected and verified within the limits listed under Fix verification. Three parallel reviewers covered separate areas, and the primary reviewer reconciled their results against current code. Additional overlapping challenge passes found no further distinct confirmed defect.

| Finding | Priority | Trigger category | Result |
| --- | --- | --- | --- |
| F1 Shared native queue ordering | P1 | Valid scheduled work on an aliased queue topology | Consumer can precede producer |
| F2 Failed surface recreation | P2 | Runtime native creation failure | Process abort bypasses terminal error handling |
| F3 Timestamp readback device loss | P2 | Device loss reported by an optional query | Backend remains open to further work |
| F4 Tessellation pipeline state | P2 | Exposed capability path | Invalid native pipeline creation |
| F5 Zero extent and same-size restoration | P2 | Native extent changes during recreation | Viewport remains suspended |
| F6 Incomplete shader layout limits | P2 | Shader exceeds applicable device limits | Invalid native layout creation |
| F7 Descriptor usage and range checks | P2 | Incompatible resource binding | Invalid descriptor writes enter the cache |
| F8 Descriptor array bounds | P2 | Malformed array index | CPU out-of-bounds write in Release |

## Confirmed findings

### RHI20261105 F1 P1 Shared native queues can reverse producer and consumer submissions

**Trigger:** timeline submission is enabled, transfer and compute refer to the same native `VkQueue`, and consecutive submission groups contain a transfer producer followed by a compute consumer. Queue selection explicitly supports this topology when no dedicated transfer family exists.

**Evidence:**

1. [SelectVulkanQueues](../ZenCore/Include/Graphics/VulkanRHI/VulkanQueueSelection.h), lines 82–84, assigns transfer to compute when a dedicated transfer family is unavailable.
2. [VulkanDevice::SetupDevice](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), lines 578–583, creates distinct logical `VulkanQueue` wrappers even when their native handles match.
3. [RHICommandListExecutor::ExecuteGroups](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp), lines 763–767, combines consecutive groups sharing a native queue. `ResolvePredecessors`, lines 719–737, skips their internal dependency waits because it relies on native submission order and recorded barriers.
4. [SubmitPlatformCommandLists and FlushAllGPUCommands](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), lines 2337–2389, distribute workloads to the separate wrappers and drain them in graphics, compute, transfer enum order. Deduplication compares wrapper pointers, so it does not reunite aliased native queues or preserve the original interleaving.

For an upload into an intermediate buffer followed by a compute-queue copy or dispatch consuming it, this submits the consumer and its barrier before the producer. The barrier cannot make the later upload visible to the earlier read. Results can contain stale or uninitialized data. Mixed logical runs also lose the single-native-call acceptance behavior assumed by the executor.

The current [ScheduledTransferComputeAliasesKeepSubmissionOrderAndBufferBarrier test](https://github.com/proton991/ZenTest/blob/main/RenderCoreTest/RDGGroupRecordingTests.inl), line 527, constructs this exact scheduling topology, but replays mocked contexts rather than checking native submission order. The local RTX 5080 reports three distinct native queues, so the passing GPU suites do not exercise this fallback.

**Correction direction:** preserve the submitted list order across logical wrappers sharing a native queue, while retaining separate logical completion serials. Alternatively, restrict grouped submission to one logical queue so each group is flushed in input order. Deduplicating wrappers by native handle alone would drop one wrapper's pending work and is insufficient.

**Regression coverage:** exercise transfer → compute and compute → transfer → compute on aliased queues, check native submission order and readback contents, and include a rejected submission to verify accepted-prefix behavior.

**Status:** corrected and verified; see F1 in the fix record below.

### RHI20261105 F2 P2 Failed surface recreation aborts instead of entering the terminal error path

**Trigger:** a viewport needs a new surface after `VK_ERROR_SURFACE_LOST_KHR`, or becomes nonzero in size after initially being created suspended, and the window backend fails to create that surface.

[VulkanViewport::Resize](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), lines 594–599, assigns the result of `CreateViewportSurface` and unconditionally invokes `FinishResize`. [CreateWindowSurface](../ZenCore/Source/Graphics/VulkanRHI/Platform/VulkanWindowBridge.cpp), lines 46–67, logs SDL/GLFW failure and returns a null handle. There is no intervening failure conversion. `FinishResize` calls `CreateSwapchain`, whose [constructor](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp), line 281, verifies that the surface is nonnull and aborts.

This bypasses the existing suspended/blocked recreation-failure handling in `FinishResize`, lines 335–350, and the [documented error contract](../ZenCore/Include/Graphics/RHI/README.md), line 249. A runtime surface allocation/creation failure terminates the process instead of returning through the renderer's terminal-error path. Initial backend invariants do not justify this runtime recreation path.

**Correction direction:** return a status alongside the surface, check it before constructing the swapchain, and publish the originating surface-creation failure on the RHI thread while suspending the viewport. Preserve SDL error text or the GLFW native result without claiming that SDL supplies a `VkResult` it does not expose.

**Regression coverage:** inject surface-creation failure after a lost surface and after a zero-size initial viewport; assert no native swapchain creation with a null surface, no process abort, and a suspended viewport with a terminal creation cause. Existing swapchain/backbuffer failure tests do not cover this earlier window-surface failure.

**Status:** corrected and verified; see F2 in the fix record below.

### RHI20261105 F3 P2 Timestamp result reads swallow device loss

**Trigger:** a timed workload has completed its fence/timeline, but `vkGetQueryPoolResults` subsequently returns `VK_ERROR_DEVICE_LOST`.

[FVulkanCommandBuffer::ResolveGPUTimings](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), lines 712–736, converts every unsuccessful query read into a timing error and releases the pool. It never reports the observed device loss through [ReportVulkanDeviceLoss](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), lines 19–29. Device loss is an explicitly permitted result of [vkGetQueryPoolResults](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html).

`SetCompleted` returns to `VulkanQueue::ProcessPendingWorkloads` after completion was already established. The backend therefore remains healthy and admits further work until another native operation reports the loss. If no workloads remain pending, a later progress poll need not perform another native completion call. The original query-read operation is also missing from terminal diagnostics. This does not invalidate the completion established before the loss; it violates the separate requirement to stop on observed device loss in the [RHI contract](../ZenCore/Include/Graphics/RHI/README.md), line 269.

**Correction direction:** report device loss from the query result while keeping ordinary optional-profiler failures nonfatal. The existing [QueryReadFailureDoesNotFailRendering test](https://github.com/proton991/ZenTest/blob/main/CommonTest/VulkanGPUTimingIntegrationTests.cpp), line 365, injects `VK_ERROR_OUT_OF_HOST_MEMORY`, not device loss, and should retain its current distinction.

**Regression coverage:** add a separate device-loss query hook and assert timing failure, `HasDeviceLoss`, blocked submission/admission, and preservation of the `vkGetQueryPoolResults` operation.

**Status:** corrected and verified; see F3 in the fix record below.

### RHI20261105 F4 P2 Tessellation pipelines reach Vulkan without required tessellation state

**Trigger:** the GPU advertises tessellation support and the caller supplies vertex, tessellation-control and tessellation-evaluation stages with `ePatchList` topology.

The RHI exposes the stages and topology in [RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h), lines 174–181 and 425. The device enables the feature in [VulkanDevice.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), line 537, and reports it in [VulkanContext.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp), line 845. Reflection and graphics-pipeline validation accept these stages. However, [VulkanPipeline::InitGraphics](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), lines 1004–1022, leaves `pTessellationState` null. The RHI has neither an input patch-control-point count nor the corresponding dynamic state.

Consequently, this accepted pipeline request reaches native creation with an invalid create-info rather than producing a usable tessellation pipeline or rejecting unsupported RHI functionality. [VkGraphicsPipelineCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkGraphicsPipelineCreateInfo.html), VUID `pStages-09022`, requires tessellation state for this case when patch control points are not dynamic.

**Correction direction:** either represent input patch size and supply `VkPipelineTessellationStateCreateInfo`, including validation and RenderCore cache identity, or explicitly gate this unsupported path before native creation and align the reported capability with the supported RHI behavior.

**Regression coverage:** on a capable device, create a three-control-point patch pipeline and verify native validation and drawing; if support is gated, assert clean rejection before the native call.

**Status:** corrected and verified; see F4 in the fix record below.

### RHI20261105 F5 P2 Zero extent during recreation prevents same size restoration

**Trigger:** native surface extent becomes zero between a nonzero window-size observation and the capability query during viewport recreation, then the window restores to the same requested dimensions. Initial creation with nonzero requested dimensions and zero native extent has the same state mismatch.

[VulkanViewport::BeginResize](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), lines 299–305, stores the requested nonzero dimensions and destroys the previous backbuffers. [VulkanSwapchain](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp), lines 320–328, correctly defers native image creation for a zero extent. However, the new object retains `VK_SUCCESS` as its last result. `VulkanViewport::CreateSwapchain`, lines 133–145, sets `m_suspended` when no images exist without updating the requested dimensions or exposing pending recreation.

[RenderDevice::ResizeViewport](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp), lines 1628–1651, clears its pending-recreation pointer after the call. A subsequent same-size restore matches the stored dimensions, and `NeedsRecreation` is false, so the guard skips rebuilding. Suspended acquisition also skips native WSI calls and cannot discover restoration. The normal window resize forwarding does not force recreation.

The viewport remains suspended without backbuffers after same-size restoration. A different-size resize or direct `viewport->Resize()` can recover it. The usual minimized-window early-out does not cover the race between observing a nonzero window extent and querying the surface.

**Correction direction:** preserve observable pending-recreation/readiness state when native creation is deferred, and make both RenderDevice and its resize callers respect it even when dimensions match.

**Regression coverage:** return zero native extent during a nonzero recreation, restore the same extent, and drive recovery through `RenderDevice::ResizeViewport` and the window callback path. Existing tests that directly call raw swapchain or viewport resize bypass the failing guard.

**Status:** corrected and verified; see F5 in the fix record below.

## Confirmed validation and capability findings

These findings require a shader or binding request outside the current device or descriptor contract. They describe missing rejection at existing RHI boundaries, not failures observed in the shipped rendering workloads. F4 above also concerns an exposed capability path without a shipped workload.

### RHI20261105 F6 P2 Shader layout validation checks only part of the native limits

**Trigger:** a shader has a fixed storage-buffer array larger than `maxPerStageDescriptorStorageBuffers`, or a push-constant block larger than `maxPushConstantsSize`. Ordinary descriptors combined with the always-present global heap can also exceed applicable update-after-bind and aggregate layout limits even when each part fits separately. Ordinary per-stage limits that explicitly exclude update-after-bind layouts must still be counted separately.

[VulkanShader::Init](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), lines 315–351, checks ordinary UBO and sampler counts before native creation. It does not check the corresponding sampled/storage-image, storage-buffer, input-attachment, acceleration-structure or aggregate descriptor limits. Global heap capacity validation in [BindlessHeapsFitLimits](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp) checks that heap in isolation. Later, `InitNativeObjects`, lines 590–600, copies the reflected push-constant size directly into a native range without checking the physical-device maximum.

For the concrete storage-buffer-array case, native layout creation violates [VkPipelineLayoutCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineLayoutCreateInfo.html), VUID `descriptorType-03018`. The push-constant case violates [VkPushConstantRange](https://docs.vulkan.org/refpages/latest/refpages/source/VkPushConstantRange.html), VUID `size-00298`. These are native valid-usage requirements; checking the return from `vkCreatePipelineLayout` does not guarantee rejection of invalid input. A shader usable on a device with higher limits can therefore reach invalid Vulkan calls on a lower-limit device.

**Correction direction:** complete device-limit validation of the reflected layout before native creation, including each descriptor class, per-stage and total limits, limits counting both ordinary sets and the global heap, and push constants. Return null with a useful diagnostic for unsupported layouts.

**Regression coverage:** inject low physical limits and test exact-limit/one-over-limit descriptor arrays and push constants; also test individually valid ordinary/global counts whose combined layout exceeds the applicable limit. Assert rejection before native object creation.

**Status:** corrected and verified; see F6 in the fix record below.

### RHI20261105 F7 P2 Descriptor preparation accepts incompatible usages and oversized ranges

**Trigger:** bind a transfer-only buffer as an SSBO, bind a sampled-only image as a storage image, or bind a storage buffer whose whole-buffer range exceeds `maxStorageBufferRange`.

[ResolveDescriptorBinding](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp), lines 128–169, checks image-view existence and buffer handle, extent and alignment, but omits the descriptor's required resource usage and native range limits. SSBO descriptors use the whole required buffer size. `ValidateSetState` checks reflected binding type/count but does not fill these gaps. `ResolveSet`, lines 457–489, subsequently writes and caches the invalid descriptors.

The [RHI contract](../ZenCore/Include/Graphics/RHI/README.md), line 123, promises invalid elements fail before native writes. These cases instead violate [VkWriteDescriptorSet](https://docs.vulkan.org/refpages/latest/refpages/source/VkWriteDescriptorSet.html): VUIDs `descriptorType-00330` / `00331` require uniform/storage buffer usage, `00332` / `00333` bound their ranges, and `00337`–`00339` require the matching image usage. Some RenderCore callers already check their own resource constraints, but direct RHI parameter submission reaches this unchecked path.

**Correction direction:** validate the actual buffer/image/view usage and descriptor range before adding a write or publishing a cached set. Apply corresponding texel-buffer usage checks rather than accepting any existing buffer view.

**Regression coverage:** wrong buffer/image usage and range-at-limit/over-limit cases must produce a recording error with no native descriptor update or cache insertion; a subsequent valid binding must remain usable.

**Status:** corrected and verified; see F7 in the fix record below.

### RHI20261105 F8 P2 Descriptor array bounds are checked after potentially overflowing writes

**Trigger:** a caller supplies an out-of-range ordinary descriptor `arrayIndex` through `RHIBatchedShaderParameters::AddResourceParam`; `UINT32_MAX` exposes the wraparound case.

[WriteResourceParameter](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp), lines 643–648, computes `arrayIndex + 1` before sizing the UBO offset vector, then indexes it with the original value. For `UINT32_MAX`, the requested size wraps to zero. Lines 667–691 similarly compute `baseIndex = arrayIndex * resourceStride` and `resourceCount = baseIndex + resourceStride` in `uint32_t`, then write at `baseIndex`. A stride-one index of `UINT32_MAX` or a stride-two index of `0x7fffffff` wraps the count without bounding the subsequent write.

The reflected array-size check in `ValidateSetState`, line 721, happens after this mutation. [HeapVector::operator[]](../ZenCore/Include/Templates/HeapVector.h), line 237, uses a Debug assertion rather than a Release bounds check. This can abort in Debug or access memory outside the container in Release, instead of rejecting the recording. Smaller oversized indices can also cause unnecessary large allocations before eventual validation.

**Correction direction:** resolve and validate the reflected set/binding/type/array bound before resizing or indexing any state. Use checked arithmetic for the resource stride and leave invalid parameter state unable to proceed to a draw/dispatch.

**Regression coverage:** test `arraySize`, `UINT32_MAX`, and stride-two overflow values for UBO and combined descriptors. Assert safe recording rejection without an allocation proportional to the invalid index or any native descriptor write.

**Status:** corrected and verified; see F8 in the fix record below.

## Review baseline verification

Both MSVC Debug and Release builds succeeded for `CommonTest`, `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, and `RayQueryIntegrationTest`.

| Suite | Debug passed | Release passed | Skipped per configuration |
| --- | ---: | ---: | ---: |
| CommonTest | 50 | 50 | 0 |
| RenderCoreTest | 758 | 758 | 0 |
| VulkanRHITest | 66 | 66 | 0 |
| VulkanRHIIntegrationTest | 420 | 421 | 5 |
| RayQueryIntegrationTest | 92 | 92 | 4 |
| Total | 1386 | 1387 | 9 |

The isolated runs contain no Vulkan VUID or synchronization-hazard errors. They use `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~`, `DISABLE_RTSS_LAYER=1`, and the repository's `rtss_runner.py` executable alias. An initial ordinary Debug run had an injected RTSS module and failed; the table uses the subsequent isolated runs. No overlay or firewall settings were changed.

`python tools/check_no_exceptions.py` passed across 538 owned C++ files. Documentation validation checked local link targets and whitespace. Source files remained unchanged during the review phase. This table applies to the baseline revision; post-fix results are recorded separately below.

Local generated evidence is under ignored `build/rhi-review-20261105/`: `verification.json`, the Debug/Release build logs, `debug-isolated/results.json`, `release-isolated/results.json`, per-suite logs, and `rayquery-debug.log` / `rayquery-release.log`. These local artifacts are not committed with this report.

Hardware execution covered Windows x64 on NVIDIA GeForce RTX 5080. This review does not close the multi-vendor or actual-device-loss acceptance gates in [RHIProductionTODO.md](RHIProductionTODO.md). No performance regression is claimed from static code alone.

## Coverage and stopping evidence

The source inventory contains 75 C++ headers/translation units and 27,945 lines under `ZenCore/Include/Graphics/{RHI,VulkanRHI}` and `ZenCore/Source/Graphics/{RHI,VulkanRHI}`. Relevant RenderCore, window, shader and test code was followed across those boundaries. The inventory is a scope measure, not a claim that every branch was executed.

| Review area | Main files and paths | Checks completed |
| --- | --- | --- |
| Resource creation and memory | `VulkanTexture`, `VulkanBuffer`, `VulkanMemory`, allocator/cast/sharing helpers, `RHIResource` | Partial creation, view/base ownership, mapping, uniform growth/reuse, buffer views, native memory lifetime |
| Bindings and shaders | `VulkanDescriptorPool`, `VulkanDescriptorSetState`, `VulkanPipeline`, `RHIShaderUtil`, `RHIShaderParameters` and associated headers | Reflection, layout limits, native state, descriptor resolution/cache publication, bindless registration/retirement, parameter mutation, uniform generations |
| Execution and lifetime | `RHIThread`, `RHICommandList`, `RHICommandListExecutor`, `VulkanCommandList`, `VulkanQueue`, `VulkanSynchronization`, `VulkanLifetimeTracker` | Admission/cancellation, copied recordings, resources through submission/completion, grouped ordering, native receipts, rejection/partial acceptance, reuse, rendering/draw/dispatch/copy translation |
| Device and presentation | `VulkanDevice`, `VulkanContext`, `VulkanExtension`, `VulkanSwapchain`, `VulkanViewport`, platform bridges, `RenderDevice` resize/presentation callers | Feature enablement, queue topology, initialization/destruction, acquire/present/retirement, surface recreation, window ownership and restore paths |
| Ray queries and diagnostics | `RHIAccelerationStructure`, `VulkanAccelerationStructure`, timing/frame-timing APIs, Vulkan debug/device-loss paths | Build/update validation, instance encoding/uploads, retained dependencies, query results, frame timing, breadcrumbs and failure propagation |
| Shared contracts | `DynamicRHI`, `RHIFactory`, `RHICommon`, `RHIError`, `RHIFrameState`, `RHIOptions`, counters/memory stats/debug APIs, `VulkanTypes`, RHI README | Capability/enum conversion, access/stage helpers, error semantics, configuration callers, command snapshots and layer ownership |

Each parallel reviewer completed an initial area pass and a failure/ownership/contract challenge pass. Resource review added a final targeted pass over the candidate findings. Two reviewers independently confirmed F1, F3 and F5. The primary reviewer re-read each reported defect and checked native API requirements. Final document audits challenged the combined-limit wording and bindless dimensionality claim, producing the qualifications below rather than extra unconditional defects.

The stopping condition was reached when the assigned areas and their relevant callers had been reviewed, each concrete candidate had a recorded disposition, and the final overlapping challenge passes produced no additional distinct confirmed issue. This is a finite code review, not a proof that no defects remain. The regression coverage listed with each original finding was proposed at review time. The fix record below identifies the implemented regressions and their post-fix results.

## Qualified candidates and rejected concerns

| Candidate | Disposition and evidence |
| --- | --- |
| Bindless array or multisample views in a 2D heap | Qualified compatibility gap, not an unconditional native-write defect. `GetBindlessHeapType` / `IsValidBindlessResource` in `VulkanDescriptorPool.cpp` classify by broad RHI texture type and sampled usage, while `VulkanTypes.cpp` can create array views. The shipped `Data/Shaders/Common/bindless_heap.glsl` uses `texture2D`. Accessing an array entry through that helper is incompatible, but a native sampled-image heap can legally contain different view types when shaders access compatible entries. No violating current registration caller was established. A test/explicit ownership contract is needed before treating registration itself as defective. |
| sRGB-only presentation causes double encoding | Rejected. `VulkanSwapchain::GetBackBufferFormat` maps the native sRGB surface to a UNORM rendering backbuffer; `SRGBOnlySurfaceUsesAnUnormRenderingBackbuffer` covers the intended copy path. |
| AS build metadata survives discarded recording | Rejected as a contract violation by the caller, not a backend rollback bug. The README requires discarding AS objects recorded by failed/unsubmitted work; the fields explicitly describe recording metadata. |
| Texture views fail to retain their base | Rejected under the documented view-owner contract. Recorded resource retention additionally retains the base texture. |
| Descriptor cache must own all bound resources | Rejected. Caller/recording ownership extends through GPU use; the content cache is not the owner. Native descriptor pools and uniform blocks use the lifetime tracker/generations. |
| Merged workloads drop resource ownership | Rejected. Roots retain merged child workloads, including their resource references, until retirement. |
| Recording rejection leaves stale pipeline cache state | Rejected. Virtual discard reaches the derived context path that clears cached state. |
| Optional timing failures must always stop rendering | Rejected. Ordinary allocation/read failures intentionally affect profiling only. F3 concerns the distinct device-loss result. |
| Recycled `CreateFence(true)` state | Inactive inconsistency: reusing a reset fence can mark only the CPU state signaled. No current caller requests `true`; no current execution defect is claimed. |
| Ordinary option setters race with RHI reads | Not established. Inspected production setters configure options before initialization; test changes are at controlled lifecycle boundaries. A concurrent runtime setter path was not found. |
| Matrix vertex-input reflection or additional SPIR-V capabilities | Unconfirmed. These need a targeted supported-shader fixture and a clearer stage/feature contract before asserting a separate defect. |
| Extended storage-image formats require an additional enabled bit | Rejected. Per-format support is checked; the Vulkan feature's enablement is not itself an additional usage gate. See the [feature contract](https://docs.vulkan.org/spec/latest/chapters/features.html#features-shaderStorageImageExtendedFormats). |

During the review phase, these findings relied on explicit control/data flow and the linked native contracts. The fix phase adds controlled failure, topology and limit regressions. Those checks do not represent actual device loss or testing on physical aliased-queue hardware.


## Fix record

Corrections are scoped to the eight confirmed findings. Qualified candidates above remain qualified; they were not silently promoted into defects or unrelated feature work.

| Finding | Correction | Regression evidence |
| --- | --- | --- |
| F1 | `ExecuteGroups` ends a submission run at each logical-queue change, even when native handles alias. Only predecessors in that same logical run skip acceptance resolution. Separate wrapper queues therefore cannot reorder mixed producer/consumer groups during their enum-order drain. Exact per-group serials and accepted-prefix failure handling are retained. | Aliased transfer → compute and compute → transfer → transfer → compute simulations reproduce the backend drain ordering and check copied data/serials. A rejected second logical run verifies the accepted prefix and suppressed suffix. `VulkanAliasedQueueIntegrationTest` hides dedicated transfer families during initialization, so the real backend shares one `VkQueue` between two logical wrappers. It checks native submission order through each wrapper's timeline, readback contents and the accepted prefix. All six cases fail when the pre-fix grouping is reinstated. |
| F2 | Window/platform surface creation returns `RHIResult<VkSurfaceKHR>`. Initial creation records failure and returns null; recreation forwards the error to `FinishResize`, which blocks and suspends before constructing a swapchain. GLFW preserves its native result. SDL preserves the operation and logs its diagnostic because it does not expose a `VkResult`. | Lost-surface and initially zero-size cases inject a surface error after real resize preparation, verify the original terminal cause, and observe no native swapchain creation. |
| F3 | Timestamp readback calls the existing device-loss reporter with `vkGetQueryPoolResults` as the operation. Observed loss blocks further work; ordinary optional timing failures remain nonfatal. | A device-loss query hook checks timing failure, loss/block state, preserved native cause and fatal next submission. The existing host-memory query failure regression continues to verify nonfatal profiling behavior. |
| F4 | Tessellation remains unsupported until the RHI represents patch-control-point state. The native feature is disabled, the reported capability is false, and tessellation shader stages or patch-list pipelines are rejected before native creation. | Capability and early-rejection tests observe no shader-module or graphics-pipeline call for unsupported requests. |
| F5 | Suspended/deferred viewport creation remains observable through `NeedsRecreation`. RenderDevice tracks pending recreation per viewport, so a waiting viewport declines only its own frames. `PrepareFrame` arms any presentation viewport without backbuffers, covering initial zero native extent, and retries until usable images exist. `RHIViewport::CanRecreate` queries the surface first, so a wait does not drain frames or rebuild the swapchain. Frames during the wait return `RenderFrameResult::eSuspended`, which interactive loops wait out without limit; scripted runs count it against their decline limit. | Controlled zero native extent drives same-size and initial-creation recovery through RendererServer, past the decline limit and without a device idle wait, followed by real presentation. A second real viewport keeps presenting during the wait. Mock viewports cover per-viewport declines and cheap polling; CPU cases cover the scripted budget and editor result handling. |
| F6 | A common reflected-layout checker validates descriptor classes, per-stage/layout totals, push constants and uniform ranges before native objects. Ordinary budgets exclude the global update-after-bind set; applicable combined budgets use the update-after-bind limits and include the heap's actual capacities even when the shader does not declare it. Graphics creation also accounts for fragment color attachments. | CPU boundary/one-over cases exercise each class and aggregate budgets; native creation observers verify rejection of unsupported shader layouts before Vulkan object creation. |
| F7 | Descriptor resolution checks the required buffer/image usage (including texel buffers and texture views), nonzero range and native uniform/storage range maximum alongside existing extent/alignment checks. Validation precedes allocation, native writes and content-cache publication. | Wrong UBO/SSBO, sampled/storage image and texel usages produce zero writes/inserts, then corrected bindings succeed. Lowered test limits reject a range one over the maximum and accept equality. |
| F8 | Ordinary parameter batches preflight the reflected set, binding, type and array bound before any state mutation. Resource-stride arithmetic uses 64 bits and must fit the eventual 32-bit indexing. Invalid input logs the shader, set, binding, element and reason, then latches recording failure and suppresses dispatch. | All six fixture descriptor classes reject `arraySize`, `0x7fffffff` and `UINT32_MAX`; observers see zero native writes, cache inserts or dispatches. Discard and a valid final-element binding recover. |

The directly affected RHI README now describes logical-queue submission runs, complete descriptor validation/limits, tessellation rejection and surface/deferred-extent behavior.

## Fix verification

Both MSVC Debug and Release builds succeeded for `CommonTest`, `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, `RayQueryIntegrationTest`, `EditorRenderingTest`, `EditorPreviewTest`, `zen_editor` and `scene_renderer_demo`.

| Suite | Debug passed | Release passed | Skipped per configuration |
| --- | ---: | ---: | ---: |
| CommonTest | 50 | 50 | 0 |
| RenderCoreTest | 771 | 771 | 0 |
| VulkanRHITest | 73 | 73 | 0 |
| VulkanRHIIntegrationTest | 457 | 458 | 5 |
| RayQueryIntegrationTest | 92 | 92 | 4 |
| EditorRenderingTest | 79 | 79 | 0 |
| EditorPreviewTest | 18 | 18 | 0 |
| Total | 1540 | 1541 | 9 |

The runs contain no Vulkan VUID or synchronization-hazard errors. They use the baseline's isolated environment and `rtss_runner.py` executable alias. The skip counts match the baseline. A Release `scene_renderer_demo` smoke run (mode 1, RHI thread, async compute, 44 frames, validation on) exited cleanly with no validation errors. `python tools/check_no_exceptions.py` passed across 541 owned C++ files, and the changed C++ hunks pass the repository `.clang-format`.

Two F4/F6 tests were corrected during verification: their valid-path pipelines had a zero-width viewport and reported `VUID-VkViewport-width-01770`, so they now set a real render area.

A review of the first F5 correction found three defects, all corrected in the F5 row above: one pending viewport declined every viewport's frames, the wait stopped at the eight-frame decline limit, and initial zero native extent had no retry outside the editor.

These results do not close the following gaps:

- No actual device loss or non-NVIDIA GPU was exercised. F1's native coverage forces aliasing on a GPU with a dedicated transfer family, not on hardware that lacks one. Hardware execution covered Windows x64 on NVIDIA GeForce RTX 5080, so the gates in [RHIProductionTODO.md](RHIProductionTODO.md) stay open.
- F5's zero extent is injected at the Vulkan boundary. No real window minimize was driven, so the editor and demo waits while suspended were built but not exercised at runtime.
- `tools/format_active.py --check` fails on existing `Data/Shaders/VoxelGI` shaders outside this change.

Local generated evidence is under ignored `build/rhi-review-20261105/`: `final-debug/`, `final-release/`, `f5-alias/` (including the reinstated-grouping run) and the `f5-build-*.log` files. These local artifacts are not committed with this report.
