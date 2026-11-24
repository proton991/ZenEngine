# RHI and VulkanRHI Production-Readiness Review, 2026-11-24

Review date: 2026-11-24. Scope: `ZenRHI` Core, Public API, CMake packaging, and `VulkanRHI` (`ZenRHI` submodule at `7b8ccd1`, tag `v0.2.0`, pinned by `ZenEngine` `5309ec20`). Metal backend files and Metal-specific findings are excluded from this document.

## Summary

**Verdict:** As a Windows desktop Vulkan RHI for ZenEngine's current renderer, `ZenRHI` has strong architectural foundations (zero-allocation `RHIError`, unified `VulkanLifetimeTracker`, content-hashed descriptor caching keyed by monotonic resource IDs, proportional bindless capacity fitting, calibrated GPU frame timing, and ~280 Vulkan integration tests), but it is **not yet ready for production** without closing the concrete correctness/concurrency defects below and finishing open operational acceptance items.

This review identifies **16 concrete code defects (`F1`–`F16`)** across `ZenRHI::Core` and `ZenRHI::Vulkan` suitable for targeted PR fixes, followed by **broader structural and operational readiness gaps** tracked outside a single bug-fix PR.

| ID | Priority | Component | Issue | Location |
| --- | --- | --- | --- | --- |
| F1 | P1 | VulkanRHI | `ProcessPendingWorkloads` publishes future `m_lastCompletedSerial` before retiring skipped workloads | [VulkanQueue.cpp](../ZenRHI/src/Vulkan/VulkanQueue.cpp#L683-L727) |
| F2 | P1 | VulkanRHI | Uncommitted bindless transactions flush to live `VkDescriptorSet` before rollback and release refs under `m_mutex` | [VulkanDescriptorPool.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1496-L1543) |
| F3 | P1 | Core RHI | Presentation failure marks already-accepted rendering failed and permanently blocks the executor | [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1049-L1102) |
| F4 | P2 | Core RHI | `RHIThread::Push` resets `m_space` while stopping, defeating `Stop()` producer wake-ups | [RHIThread.cpp](../ZenRHI/src/Core/RHIThread.cpp#L333-L364) |
| F5 | P2 | Core + VulkanRHI | Cross-thread data races on shared backend, viewport, frame-state, and fault-diagnostic fields | [VulkanRHI.h](../ZenRHI/src/Vulkan/VulkanRHI.h#L320-L324) |
| F6 | P2 | VulkanRHI | `VulkanFenceManager` recycled-fence state desync and hard abort on device-loss teardown | [VulkanSynchronization.cpp](../ZenRHI/src/Vulkan/VulkanSynchronization.cpp#L46-L61) |
| F7 | P2 | VulkanRHI | Transient swapchain recreation failures permanently block all submissions | [VulkanViewport.cpp](../ZenRHI/src/Vulkan/VulkanViewport.cpp#L335-L368) |
| F8 | P2 | VulkanRHI | `VulkanLifetimeTracker::Destroy()` hard-aborts on leaked entries, ignoring `StrictTeardownChecks` | [VulkanLifetimeTracker.cpp](../ZenRHI/src/Vulkan/VulkanLifetimeTracker.cpp#L220-L247) |
| F9 | P3 | VulkanRHI | Incompatible `vkCmdPushConstants` disturbs bound descriptors without invalidating command-buffer cache | [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L2082-L2099) |
| F10 | P3 | Core RHI | `ExecuteGroups` overwrites presentation-prep cause with `eInvalidArgument`; late submission after `Destroy()` indexes OOB | [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L878-L934) |
| F11 | P3 | VulkanRHI | `VulkanAccelerationStructure::RecordBuild` sets `m_buildRecorded` before submission | [VulkanAccelerationStructure.cpp](../ZenRHI/src/Vulkan/VulkanAccelerationStructure.cpp#L428-L440) |
| F12 | P3 | VulkanRHI | Partially-bound descriptor array rejects null at element 0 when higher elements are unbound | [VulkanDescriptorSetState.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorSetState.cpp#L82-L92) |
| F13 | P3 | VulkanRHI | Discarded command buffers leak GPU timing `VkQueryPool`s; `SetupNewCommandBuffer` missing `break` over-allocates | [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L1200-L1219) |
| F14 | P3 | Core + VulkanRHI | `vkGetDeviceFaultInfoEXT` prints bogus records on error; nullable `std::string_view` formatting hazard | [VulkanDevice.cpp](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L91-L108) |
| F15 | P3 | Core RHI | SPIR-V reflection mishandles matrix/array vertex inputs and hardcodes `binding = 0`; `BindlessHeap.glsl` omits cube heap | [ShaderReflection.h](../ZenRHI/src/Core/ShaderReflection.h#L44-L54) |
| F16 | P3 | Core + Packaging | `ZenRHIConfig.cmake.in` fails `COMPONENTS Core\|Vulkan\|ZenRHI`; `EndGPUFrameTiming` and `InvokeChecked` wrappers lose errors | [ZenRHIConfig.cmake.in](../ZenRHI/cmake/ZenRHIConfig.cmake.in#L6-L16) |

Priorities follow [RHIFullReview20261106.md](RHIFullReview20261106.md): **P1** causes corruption, leaks, or contract violations reachable in normal or failure-recovery paths; **P2** causes hangs, races, or unrecoverable crashes under teardown/resize/contention; **P3** causes validation errors, diagnostic loss, resource waste, or packaging failures in edge cases.

---

## High-Impact Correctness & Lifetime Findings (P1–P2)

### F1: `VulkanQueue::ProcessPendingWorkloads` publishes future `m_lastCompletedSerial` before retiring workloads (P1)

**Location:** [VulkanQueue.cpp](../ZenRHI/src/Vulkan/VulkanQueue.cpp#L683-L727), [VulkanQueue.cpp](../ZenRHI/src/Vulkan/VulkanQueue.cpp#L780-L803).

**Cause.** On timeline-semaphore devices, `ProcessPendingWorkloads(timeToWaitNS, maxSubmissionSerial)` reads the GPU's full `completedValue` once via `m_pTimelineSemaphore->GetCounterValue(completedValue)` and immediately stores `m_lastCompletedSerial = std::max(m_lastCompletedSerial.load(), completedValue)` on line 726—before popping workloads above `maxSubmissionSerial` (and before calling `SetCompleted()` / `ReleaseWorkload()` on the current workload).

**Trigger.** Submit timeline workloads `1`, `2`, and `3`. The GPU completes all three. A caller invokes `WaitForCompletion(1, ...)`, which calls `ProcessPendingWorkloads(..., maxSubmissionSerial = 1)`. Iteration 1 sets `m_lastCompletedSerial = 3` and exits the loop at line 683 (`2 > 1`).

**Effect.** A subsequent `WaitForCompletion(3, ...)` hits the fast path at line 791 (`m_lastCompletedSerial >= submissionSerial`) and returns `true` without calling `ProcessPendingWorkloads`. Workloads `2` and `3` remain stuck in `m_workloadsPendingProcess`, skipping `FVulkanCommandBuffer::SetCompleted()`, leaving their GPU timings unpublished, and delaying retirement of retained pools/resources.

**Fix direction.** Advance `m_lastCompletedSerial` per retired workload (using `pWorkload->m_submissionSerial`) only after `SetCompleted()` / `ReleaseWorkload()` complete, and keep the raw `completedValue` local to the loop.

**Regression test.** Submit 3 timeline workloads with GPU timing scopes, wait on GPU idle, call `WaitForCompletion(1)`, then `WaitForCompletion(3)`; verify workloads 2 and 3 are popped, their command buffers transition out of `eSubmitted`, and their timing results publish `eAvailable`.

---

### F2: Uncommitted bindless transactions flush to the live `VkDescriptorSet` before rollback (P1)

**Location:** [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L2381-L2461), [VulkanDescriptorSetState.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorSetState.cpp#L327-L357), [VulkanDescriptorPool.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1496-L1543), [VulkanDescriptorPool.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1851-L1914).

**Cause.** During `FinalizeCommandLists`, `commands->Execute()` registers shader-parameter bindless entries under an uncommitted `transaction`, queuing them into `m_pendingWrites`. The very next draw or dispatch in the same command list calls `FlushPendingDescriptorWrites()` → `VulkanBindlessDescriptorPoolManager::Flush()` → `WriteDescriptorSetBatch()`, which executes `vkUpdateDescriptorSets` directly on live `m_vkSet` without filtering out uncommitted transactions and clears `m_pendingWrites`. In addition, `ResolveTransaction(..., false)` calls `resource->ReleaseReference()` and `texture->ReleaseReference()` while still holding `m_mutex` ([line 1498](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1498), [lines 1531–1536](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1531-L1536)).

**Trigger.** Record a command list with a new bindless parameter registration followed by a bindless draw, then trigger a recording error on a later command in the same list/batch (or `eRejected` in `FlushAllGPUCommands`).

**Effect.** `ResolveTransaction(owner, false)` finds `m_pendingWrites` already empty, zeroes the CPU slot state, and releases the resource references—potentially destroying `VkImageView`/`VkSampler` handles that were already written into the live global `m_vkSet`. Holding `m_mutex` during `ReleaseReference()` also risks re-entrancy deadlock in `RHIExecutionMode::eInline` when `Destroy()` calls `GDynamicRHI->NotifyResourceDestroyed`.

**Fix direction.** Defer writing transaction-owned entries to `m_vkSet` until `ResolveTransaction(transaction, true)` commits (e.g., hold per-transaction staged writes until commit, then merge into `m_pendingWrites` and flush before queue submit). Collect released resource pointers into a local list and call `ReleaseReference()` after releasing `m_mutex` (apply the same unlock-before-release pattern at [lines 1276 and 1683](../ZenRHI/src/Vulkan/VulkanDescriptorPool.cpp#L1276)).

**Regression test.** Record a list registering a new texture via shader parameters, issue a draw, then force a recording error on a second command; destroy the texture and verify no `vkUpdateDescriptorSets` wrote the rolled-back slot into `m_vkSet`, and verify inline rollback does not deadlock.

---

### F3: Presentation failure marks already-accepted rendering failed and bricks the executor (P1)

**Location:** [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1049-L1102), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1137-L1145).

**Cause.** After `ExecuteGroups(*batch)` succeeds and accepts every rendering group on the GPU (`batch.submissionState->Accept(...)`), `ExecuteFrame` calls `batch->viewport->PresentChecked()`. If `result.presentation.error.IsFailure()`, lines 1057–1100 set `result.submission = eFatal`, skip `m_backend->EndFrame()`, call `batch->submissionState->Fail()` (turning already-accepted producer groups into `eFailed`), and call `PublishTerminalError(result.cause)`.

**Trigger.** Any non-device-loss presentation failure (e.g., transient WSI error on one viewport in a multi-viewport application such as `ZenEditor`).

**Effect.** Directly contradicts [ZenRHI/include/ZenRHI/README.md](../ZenRHI/include/ZenRHI/README.md#L17) (*"Already accepted rendering remains accepted even when presentation fails"*), corrupts `RenderSubmissionHistory` for accepted GPU work, skips `EndFrame()`, and permanently blocks submissions for all healthy viewports.

**Fix direction.** Keep `result.submission` reflecting GPU rendering acceptance when all render groups succeeded, surface presentation errors solely via `result.presentation` / `result.presented`, call `EndFrame()` for accepted frames, and escalate to `PublishTerminalError()` only on `RHIErrorCode::eDeviceLost` or uncertain synchronization failure.

**Regression test.** Inject a non-device-loss failure in `PresentChecked()` after an accepted graphics group; verify `batch->submissionState->Resolve(0)` remains accepted, `EndFrame()` ran, `result.presentation.error` reports the cause, and subsequent submissions on another viewport are not blocked.

---

### F4: `RHIThread::Push` resets `m_space` while stopping, defeating `Stop()` wake-ups (P2)

**Location:** [RHIThread.cpp](../ZenRHI/src/Core/RHIThread.cpp#L175), [RHIThread.cpp](../ZenRHI/src/Core/RHIThread.cpp#L333-L364).

**Cause.** When the queue is full (`m_tasks.Size() >= m_capacity`), a blocked producer unlocks `m_mutex` at line 335 before calling `m_space.Wait()` at line 337. `Stop()` sets `m_stopping = true` and signals `m_space` (line 175) to wake blocked producers. However, `DispatchCleanup` bypasses capacity and stays admitted during draining; when a cleanup task pushes into `m_tasks`, line 360 checks `if (m_tasks.Size() >= m_capacity && !m_taskFailed)` without checking `!m_stopping`, calling `m_space.Reset()` at line 362.

**Trigger.** Queue hits capacity with a producer waiting to enter `m_space.Wait()`, `Stop()` runs concurrently with a resource release enqueuing `DispatchCleanup`.

**Effect.** The producer re-enters `m_space.Wait()` on a reset event and blocks for the entire drain—deadlocking if any draining/cleanup task waits on or shares a lock with that producer.

**Fix direction.** Change line 360 to `if (m_tasks.Size() >= m_capacity && !m_stopping && !m_taskFailed)`.

**Regression test.** Fill `RHIThread` to capacity, block a producer in `DispatchChecked`, enqueue a `DispatchCleanup` while calling `Stop()`, and verify the producer wakes promptly with `eClosed`.

---

### F5: Cross-thread data races on shared backend, viewport, frame-state, and fault-diagnostic fields (P2)

**Location:** [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L376), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L970), [RHIResource.cpp](../ZenRHI/src/Core/RHIResource.cpp#L13-L15), [VulkanRHI.h](../ZenRHI/src/Vulkan/VulkanRHI.h#L320-L324), [VulkanViewport.cpp](../ZenRHI/src/Vulkan/VulkanViewport.cpp#L597-L600), [VulkanSwapchain.h](../ZenRHI/src/Vulkan/VulkanSwapchain.h#L162), [VulkanDevice.cpp](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L45-L68).

**Cause.** Several fields shared between the render/window thread and the RHI worker thread are plain non-atomic members accessed without synchronization:
- `GRHIFrameState` (`m_frameNumber` / frame slot) is advanced on the RHI worker in `EndFrame()`, while `QueueBatch` rejection runs `CancelBatch` → `PublishTerminalError()` on the render thread ([RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L376)).
- `GDynamicRHI` is a non-atomic raw pointer read in `~RHIResource()` on the RHI worker ([RHIResource.cpp](../ZenRHI/src/Core/RHIResource.cpp#L13)) while callers mutate it on the render thread.
- `VulkanRHI::m_submissionBlocked`, `m_deviceLost`, and `m_terminalError` ([VulkanRHI.h](../ZenRHI/src/Vulkan/VulkanRHI.h#L320-L324)) are written on the RHI thread and read on the render/window thread (`CreateViewport`, `AreSubmissionsBlocked`, `GetTerminalError`).
- `VulkanSwapchain::m_lastResult` ([VulkanSwapchain.h](../ZenRHI/src/Vulkan/VulkanSwapchain.h#L162)) is written on the RHI thread during acquire/present and read via `VulkanViewport::NeedsRecreation()` ([VulkanViewport.cpp](../ZenRHI/src/Vulkan/VulkanViewport.cpp#L599)).
- `VulkanDevice::m_diagnosticBuffers` and `m_faultReported` ([VulkanDevice.cpp](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L45-L68)) have no lock or atomic guard.

**Effect.** Undefined behavior under the C++ memory model; torn reads of `RHIError` structs or concurrent vector mutation if device-loss reporting overlaps command-buffer registration/destruction.

**Fix direction.** Make `m_submissionBlocked`, `m_deviceLost`, `m_lastResult`, `m_faultReported`, and `GRHIFrameState` counters atomic (or guard `m_terminalError` and `m_diagnosticBuffers` with a dedicated mutex).

**Regression test.** Run `ZenRHICoreTests` and threaded `ZenRHIVulkanIntegrationTests` under ThreadSanitizer (TSan) / stress-test concurrent `NeedsRecreation()` + `CancelBatch()` + `ReportDeviceLoss()`.

---

### F6: `VulkanFenceManager` recycled-fence state desync and hard abort on device-loss teardown (P2)

**Location:** [VulkanSynchronization.cpp](../ZenRHI/src/Vulkan/VulkanSynchronization.cpp#L46-L61), [VulkanSynchronization.cpp](../ZenRHI/src/Vulkan/VulkanSynchronization.cpp#L143-L152).

**Cause.** Three related defects in `VulkanFenceManager`:
1. `CreateFence(createSignaled = true)` pops a reset (unsignaled) fence from `m_freeFences` and assigns `pFence->m_state = State::eSignaled` (line 56), leaving CPU state out of sync with the native `VkFence`.
2. Releasing a fence that became signaled on the GPU after `vkDeviceWaitIdle` (without polling `GetFenceStatus`/`WaitForFence`) leaves `pFence->m_state == State::eInitial`, so `ResetFence` skips `vkResetFences` and returns a signaled `VkFence` to `m_freeFences` (`VUID-vkQueueSubmit-fence-00064` on reuse).
3. `ResetFence` calls `ZEN_RHI_VERIFY_MSG_F(vkResetFences(...) == VK_SUCCESS, ...)` (line 147), hard-aborting during `DestroyWorkload` → `ReleaseFence` if `vkResetFences` returns `VK_ERROR_DEVICE_LOST` or another error during teardown.

**Fix direction.** Only recycle fences from `m_freeFences` when `!createSignaled`; track whether a fence was submitted (or unconditionally reset on release unless device loss is active); on `vkResetFences != VK_SUCCESS`, call `ReportVulkanDeviceLoss` and destroy the fence instead of returning it to `m_freeFences` or aborting.

**Regression test.** Unit/integration test for `CreateFence(true)` after recycling a released fence, fence reuse after `vkDeviceWaitIdle`, and `ScopedVulkanCall` returning `VK_ERROR_DEVICE_LOST` from `vkResetFences` during teardown.

---

### F7: Transient swapchain recreation failures permanently block all submissions (P2)

**Location:** [VulkanViewport.cpp](../ZenRHI/src/Vulkan/VulkanViewport.cpp#L335-L368).

**Cause.** `VulkanViewport::FinishResize` only sets `replaceSurface = true` when `m_recreationError.nativeCode == VK_ERROR_SURFACE_LOST_KHR` (line 351). For any other recreation error—including `VK_ERROR_OUT_OF_DATE_KHR` returned by `vkCreateSwapchainKHR` during a rapid resize race, or a transient backbuffer OOM under `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT`—line 355 calls `GVulkanRHI->BlockSubmissions(...)`.

**Trigger.** Rapid window resizing where the surface extent changes between `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` and `vkCreateSwapchainKHR`, or a brief desktop VRAM budget dip during resize.

**Effect.** Permanently halts all GPU rendering for the lifetime of the device instead of leaving that viewport suspended for retry on the next resize/frame.

**Fix direction.** Keep `m_suspended = true` and retryable for `VK_ERROR_OUT_OF_DATE_KHR` (and recoverable surface-extent races), reserving `BlockSubmissions` for confirmed `VK_ERROR_DEVICE_LOST` or unrecoverable fatal errors.

**Regression test.** Use `ScopedVulkanCall` on `vkCreateSwapchainKHR` to return `VK_ERROR_OUT_OF_DATE_KHR` once during `Resize()`; verify `AreSubmissionsBlocked()` remains `false`, viewport remains suspended (`NeedsRecreation() == true`), and a subsequent resize recovers cleanly.

---

### F8: `VulkanLifetimeTracker::Destroy()` hard-aborts on leaked entries, ignoring `StrictTeardownChecks` (P2)

**Location:** [VulkanLifetimeTracker.cpp](../ZenRHI/src/Vulkan/VulkanLifetimeTracker.cpp#L220-L247), [RHIError.h](../ZenRHI/include/ZenRHI/RHIError.h#L192-L205).

**Cause.** `VulkanLifetimeTracker::Destroy()` asserts `ZEN_RHI_VERIFY(entry.retired)` unconditionally at line 233 rather than routing through `VerifyTeardownOwnership(...)`.

**Effect.** In Release builds where `RHIOptions::StrictTeardownChecks()` defaults to `false`, any unretired lifetime entry during shutdown triggers `std::abort()` instead of logging the violation and completing teardown as documented in [ZenRHI/include/ZenRHI/README.md](../ZenRHI/include/ZenRHI/README.md#L290-L292).

**Fix direction.** Replace `ZEN_RHI_VERIFY(entry.retired)` with `VerifyTeardownOwnership(entry.retired, "Vulkan lifetime tracker entries outlive backend", __FILE__, __LINE__)`.

**Regression test.** Create a tracked lifetime entry without retiring it, set `SetStrictTeardownChecks(false)`, destroy `VulkanLifetimeTracker`, and verify it logs without aborting; repeat with `SetStrictTeardownChecks(true)` in a death test.

---

## Validation, Error Propagation & Packaging Findings (P3)

### F9: Incompatible `vkCmdPushConstants` disturbs bound descriptor sets without invalidating cache (P3)

**Location:** [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L2082-L2099).

**Cause.** Per Vulkan specification (Pipeline Layout Compatibility), calling `vkCmdPushConstants` with a pipeline layout whose push-constant ranges are incompatible with the previously bound layout disturbs all bound descriptor sets across bind points. `FVulkanCommandListContext::RHISetPushConstants` calls `vkCmdPushConstants` directly without invalidating `FVulkanCommandBuffer::m_boundStates[...].descriptorLayout`.

**Trigger.** Bind pipeline P1 (layout L1) and draw; push constants using pipeline P2 (push-constant-incompatible layout L2); re-bind P1 with unchanged descriptor sets and draw.

**Effect.** `BindPipelineAndDescriptorSets` sees a cached match and skips `vkCmdBindDescriptorSets`, triggering a Vulkan validation error / undefined shader descriptors.

**Fix direction.** Track the last push-constant pipeline layout (or invalidate cached descriptor layouts on `FVulkanCommandBuffer` whenever `pipeline->GetVkPipelineLayout()` differs from the cached bound layout).

**Regression test.** Bind two pipelines with incompatible push-constant layouts on the same command buffer, interleave `SetPushConstants`, and draw under Vulkan validation layers.

---

### F10: `ExecuteGroups` overwrites presentation-prep cause; late submissions after `Destroy()` index OOB (P3)

**Location:** [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L293-L307), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L478), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L878-L934).

**Cause.**
1. In `ExecuteGroups`, when `PrepareForPresentChecked` fails (lines 884–891) or `AcquirePresentCommandList()` returns `nullptr` (lines 880, 897), `ready` becomes `false`. Line 905 then unconditionally constructs `MakeRHIError(RHIErrorCode::eInvalidArgument, "RHI group preparation", ...)` and assigns that synthetic cause to every `batch.result.groups[i].cause` (line 932), masking the true `prepared.error` (e.g., `eDeviceLost` or OOM).
2. In `ExecuteDestroy()`, line 301 calls `m_recordingCommandLists.clear()` (shrinking the vector from `kQueueCount` to `0`) without setting `m_blocked = true`. Any submission attempted after `Destroy()` passes `!AreSubmissionsBlocked()` and indexes `m_recordingCommandLists[size_t(queue)]` out of bounds at line 478.
3. Also, `SubmitGroups` when blocked ([lines 674–679](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L674-L679)) leaves `result.cause` empty instead of copying `GetTerminalError()`.

**Fix direction.** Preserve `batch.result.cause` (`RHIErrorOr(batch.result.cause, ...)`) when populating `group.cause`; set `m_blocked.store(true, std::memory_order_release)` in `Destroy()`/`ExecuteDestroy()`; populate `result.cause = GetTerminalError()` on blocked `SubmitGroups`.

**Regression test.** Inject `eOutOfDeviceMemory` from `PrepareForPresentChecked` and assert `group.cause.code == RHIErrorCode::eOutOfDeviceMemory`; call `SubmitGroups` after `executor.Destroy()` and verify clean rejection without crashing.

---

### F11: `VulkanAccelerationStructure::RecordBuild` mutates `m_buildRecorded` before submission (P3)

**Location:** [VulkanAccelerationStructure.cpp](../ZenRHI/src/Vulkan/VulkanAccelerationStructure.cpp#L326-L343), [VulkanAccelerationStructure.cpp](../ZenRHI/src/Vulkan/VulkanAccelerationStructure.cpp#L428-L440).

**Cause.** `RecordBuild` sets `m_buildRecorded = true` and overwrites `m_geometries` / `m_instanceCount` during command recording rather than upon accepted queue submission.

**Trigger.** Record a BLAS/TLAS build, discard the recording due to a later recording error or rejected submission, then record an incremental update (`pSource = this`).

**Effect.** `IsUpdateCompatible` returns `true` and emits `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR` against an uninitialized native acceleration structure.

**Fix direction.** Defer committing `m_buildRecorded` and geometry metadata until the containing `VulkanWorkload` is accepted on the queue (or roll back recorded AS builds when a workload is discarded).

**Regression test.** Record a BLAS build, discard the command list before submission, then attempt an in-place update on that BLAS and assert recording rejects it.

---

### F12: Partially-bound descriptor array rejects null element 0 when higher elements are unbound (P3)

**Location:** [VulkanDescriptorSetState.cpp](../ZenRHI/src/Vulkan/VulkanDescriptorSetState.cpp#L82-L92).

**Cause.** `ResolveDescriptorBinding` decides whether null array elements may be skipped via `count > 1`, where `count = binding.resources.size() / stride`. Because `binding.resources` grows lazily to `arrayIndex + 1`, binding `{index = 0, resource = nullptr}` on a partially-bound array (e.g., `uTextures[16]`) produces `count == 1` and fails descriptor validation.

**Fix direction.** Pass the reflected `descriptor->arraySize` into `ResolveDescriptorBinding` and test `descriptor->arraySize > 1` instead of `count > 1`.

**Regression test.** Bind `{arrayIndex = 0, pResource = nullptr}` on a shader declaring `texture2D uTextures[4]` and verify it resolves cleanly as a partially-bound array (or if intentionally disallowed, document and test consistently for element 0 vs element 1).

---

### F13: GPU timing `VkQueryPool` leak on discard and command-buffer over-allocation in `SetupNewCommandBuffer` (P3)

**Location:** [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L366-L382), [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L701-L762), [VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L1200-L1219).

**Cause.**
1. `FVulkanCommandBuffer::DiscardGPUTimings()` clears `m_timingScopes` without calling `ReleaseGPUTimingPool()`. Because `ResolveGPUTimings()` only releases `m_timestampPool` when `!m_timingScopes.empty()` (line 701), a discarded command buffer holds onto its `VkQueryPool` (from the global 256-pool cap) until reused with another timing scope or evicted after 10 s idle.
2. `SetupNewCommandBuffer` loops over `m_pCmdBufferPool->m_cmdBuffersInUse` without `break;` when finding a reusable or `eNotAllocated` command buffer (lines 1200–1219), calling `AllocMemory()` on *every* `eNotAllocated` entry in the vector and returning only the last one.

**Fix direction.** Call `ReleaseGPUTimingPool()` inside `DiscardGPUTimings()` (when safe on unsubmitted/discarded buffers); add `break;` in both reusable branches of `SetupNewCommandBuffer`.

**Regression test.** Begin a GPU timing scope and discard the command buffer repeatedly (over `kMaxGPUTimingPools` iterations) and verify subsequent timing scopes still succeed; verify `SetupNewCommandBuffer` allocates at most one command buffer per request.

---

### F14: `vkGetDeviceFaultInfoEXT` bogus error-path output and nullable `std::string_view` formatting hazard (P3)

**Location:** [VulkanDevice.cpp](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L91-L108), [RHIGlobals.cpp](../ZenRHI/src/Core/RHIGlobals.cpp#L30).

**Cause.**
1. `VulkanDevice::ReportDeviceLoss` initializes `counts.addressInfoCount = 64` and `counts.vendorInfoCount = 16`, calls `vkGetDeviceFaultInfoEXT`, and loops over `std::min(counts.*, ...)` even when `result != VK_SUCCESS && result != VK_INCOMPLETE`, printing 64 zeroed address records and 16 empty vendor records on failure.
2. `ReportRHIMessage` passes `message.data()` directly to `std::fprintf("%.*s")` without guarding default-constructed empty `std::string_view` (`data() == nullptr`).

**Fix direction.** Gate the fault-info print loops on `result == VK_SUCCESS || result == VK_INCOMPLETE`; pass `message.empty() ? "" : message.data()` in `ReportRHIMessage`.

---

### F15: SPIR-V vertex-input reflection defects and missing cube heap in `BindlessHeap.glsl` (P3)

**Location:** [ShaderReflection.h](../ZenRHI/src/Core/ShaderReflection.h#L31-L54), [ShaderReflection.h](../ZenRHI/src/Core/ShaderReflection.h#L424-L447), [BindlessHeap.glsl](../ZenRHI/include/ZenRHI/Shader/BindlessHeap.glsl).

**Cause.**
1. `ParseSpvVertexInputs` hardcodes `attribute.binding = 0`, computes `vertexBindingStride += scalar.width / 8 * max(1u, component_count)` (incorrect for matrix/array vertex attributes, which span multiple columns/locations), and casts `input->format` directly to `DataFormat` without checking validity.
2. `MergeOrAddSRDs` checks only `(set, binding)` compatibility, silently ignoring conflicting variable names across stages and not rejecting duplicate descriptor names across distinct `(set, binding)` locations at reflection time.
3. [BindlessHeap.glsl](../ZenRHI/include/ZenRHI/Shader/BindlessHeap.glsl) declares bindings 0, 2, 3, and 4 (`uTexture2DHeap`, `uSamplerHeap`, `uTexture2DArrayHeap`, `uTextureCubeArrayHeap`), omitting binding 1 (`ZEN_BINDLESS_HEAP_BINDING_TEXTURECUBE` / `uTextureCubeHeap`) defined in [Bindless.h](../ZenRHI/include/ZenRHI/Shader/Bindless.h) and [README.md](../ZenRHI/include/ZenRHI/README.md#L142).

**Fix direction.** Reject unsupported matrix/array vertex inputs (or expand per column/element) and validate `DataFormat` conversion; add cube heap declaration/helper to `BindlessHeap.glsl`.

---

### F16: CMake `COMPONENTS` resolution broken; `EndGPUFrameTiming` and `InvokeChecked` wrappers drop errors (P3)

**Location:** [ZenRHIConfig.cmake.in](../ZenRHI/cmake/ZenRHIConfig.cmake.in#L6-L16), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1201-L1204), [RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1372-L1515), [RHIThread.h](../ZenRHI/include/ZenRHI/RHIThread.h#L131-L176).

**Cause.**
1. `ZenRHIConfig.cmake.in` sets only `ZenRHI_TestSupport_FOUND TRUE` before `check_required_components(ZenRHI)`, so `find_package(ZenRHI CONFIG REQUIRED COMPONENTS Core)` or `COMPONENTS Vulkan` fails. (Additionally, [BackendDependencies.cmake](../ZenRHI/cmake/BackendDependencies.cmake#L2-L35) links `PRIVATE` pre-existing `spirv-reflect`/`volk`/`vma` targets into static libraries, which breaks `install(EXPORT ZenRHITargets)` if a superproject defines those targets without exporting them.)
2. `RHICommandListExecutor::EndGPUFrameTiming` ignores the `bool` returned by `DispatchCleanup(...)`; if cleanup admission is closed, `timing` stays stuck in `RHIGPUTimingStatus::ePending` forever.
3. All 12 resource/query `InvokeChecked` wrappers in `RHICommandListExecutor` discard `result.GetError()`, leaving callers unable to distinguish OOM, invalid input, or a stopped worker.

**Fix direction.** Set `ZenRHI_Core_FOUND`, `ZenRHI_Vulkan_FOUND`, and `ZenRHI_ZenRHI_FOUND` in `ZenRHIConfig.cmake.in`; seal `timing` as `eError` when `DispatchCleanup` returns `false` in `EndGPUFrameTiming`.

---

## Structural & Operational Gaps (Tracked Beyond Immediate Bug-Fix PR)

These do not all need to land in a bug-fix PR, but they remain open before `ZenRHI` (Core + VulkanRHI) can be considered production-ready:

1. **Hot-path overhead & memory hygiene (Core + Vulkan):**
   - `RHIThread::InvokeChecked` ([RHIThread.h](../ZenRHI/include/ZenRHI/RHIThread.h#L131-L176)) lacks an inline/same-thread fast path and allocates `SharedPtr<std::promise>`, `RHIThreadEvent` (a Win32 kernel Event `HANDLE`), and two `std::function`s on every call.
   - `RHICommandListExecutor::ReleaseWhenComplete` ([RHICommandListExecutor.cpp](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L392-L398)) uses blocking `Invoke` instead of async FIFO `DispatchCleanup`, stalling the render thread on deferred releases.
   - `RHIBeginDebugLabel` ([VulkanCommandList.cpp](../ZenRHI/src/Vulkan/VulkanCommandList.cpp#L1743)) and `RHICommandDebugLabel` ([RHICommandList.h](../ZenRHI/include/ZenRHI/RHICommandList.h#L714-L734)) unconditionally intern strings into permanent mutex-locked `NameRegistry` storage even when diagnostics and debug utils are off.
   - `noexcept` worker lambdas in [RHIThread.h](../ZenRHI/include/ZenRHI/RHIThread.h#L143-L197) call `std::promise::set_value` with heap-allocating `RHIBatchResult` (`std::vector`/`std::string`), risking `std::terminate()` on `std::bad_alloc`.
2. **Error resilience & diagnostics:**
   - Vulkan initialization still hard-aborts via `ZEN_RHI_VERIFY*` in `VulkanContext`/`VulkanDevice`/`VulkanExtension`/`VulkanMemory`/`VulkanQueue` instead of returning `nullptr` cleanly through `CreateVulkanRHI`.
   - `R24` ([RHIStallWatchdogPlan.md](RHIStallWatchdogPlan.md)) is not implemented: [RHIThread.cpp](../ZenRHI/src/Core/RHIThread.cpp#L37), [line 112](../ZenRHI/src/Core/RHIThread.cpp#L112) wait with `INFINITE` timeout.
   - `ZenApp` and `ZenEditor` call `DynamicRHI::Create()` before `RenderDevice` installs `RHIMessageCallback`, sending startup diagnostics to raw `stderr` instead of `spdlog`.
   - `VulkanMemory.cpp` unconditionally passes `VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT` on mandatory allocations (backbuffers, UBOs, staging) without fallback retry.
   - Without `VK_KHR_swapchain_maintenance1`, continuous resize faster than image re-acquisition can accumulate `m_retiredSwapchains` without cap ([VulkanSwapchain.cpp](../ZenRHI/src/Vulkan/VulkanSwapchain.cpp#L717-L730), [lines 832–875](../ZenRHI/src/Vulkan/VulkanSwapchain.cpp#L832-L875)).
3. **Performance & modern Vulkan architecture:**
   - `VkPipelineCache` is not persisted to disk ([VulkanDevice.cpp](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L584-L593)), and PSO creation runs synchronously on the single RHI thread.
   - `VK_SHARING_MODE_CONCURRENT` is forced on every buffer and render target when `computeFamily != graphicsFamily` ([VulkanResourceSharing.h](../ZenRHI/src/Vulkan/VulkanResourceSharing.h#L11-L35)), and barriers use legacy Sync1 `vkCmdPipelineBarrier` ([VulkanSynchronization.cpp](../ZenRHI/src/Vulkan/VulkanSynchronization.cpp#L352-L437)).
   - `VulkanViewport` uses a single non-ring-buffered color/depth backbuffer, full-resolution `vkCmdCopyImage` per frame, and `vkDeviceWaitIdle` on resize; separate presentation queues and HDR/10-bit swapchains are unsupported.
   - `VK_KHR_opacity_micromap` + `VK_KHR_device_address_commands` are implemented instead of shipping `VK_EXT_opacity_micromap`; TLAS builds allocate a fresh `VkBuffer` per build ([VulkanAccelerationStructure.cpp](../ZenRHI/src/Vulkan/VulkanAccelerationStructure.cpp#L453-L480)).
   - Process-wide singletons (`GDynamicRHI`, `GVulkanRHI`, `GRHIFrameState`, `GetRHIThread()`, `RHIOptions`), single-threaded recording (no secondary command buffers), and missing Linux WSI support block general-purpose standalone adoption.
4. **Testing & CI acceptance:**
   - [.github/workflows/rhi-software.yml](../.github/workflows/rhi-software.yml) builds SwiftShader but runs `--unit-only` (never running `ZenRHIVulkanIntegrationTests`); [ZenRHI/.github/workflows/windows.yml](../ZenRHI/.github/workflows/windows.yml) runs only `-L "unit|policy"` (skipping `ZenRHIRelocatedConsumer` and integration suites).
   - Open acceptance blockers from [RHIProductionTODO.md](RHIProductionTODO.md): real hardware device-loss reproduction (`R7`), Intel GPU acceptance (`R8`), and post-refactor native Vulkan AS/ray-query verification on desktop Vulkan hardware.

---

## Verification & Limits of This Review

- **Executed checks (local debug build `build/arm64-apple-clang-debug`):** `ZenRHICoreTests`, `ZenRHIVulkanTests`, `ZenRHIPublicAPI`, `ZenRHIPublicAPIChecker`, and `NoEngineExceptions` all passed (`5/5`).
- **Static analysis only for defects:** None of `F1`–`F16` were reproduced with runtime tests, ThreadSanitizer, or live Vulkan GPU runs during this review (hardware GPU / MoltenVK ICD access was unavailable in the sandbox). Every cited location and code path in `F1`–`F16` was verified directly against the current source files before inclusion.
- **Recommended PR validation:** When implementing fixes for `F1`–`F16`, add the listed regression tests and run `ZenRHICoreTests`, `ZenRHIVulkanTests`, `ZenRHIVulkanIntegrationTests`, `RenderCoreTest`, and `VulkanRHIIntegrationTest` in Debug and Release on Windows desktop Vulkan with validation and synchronization validation enabled.
