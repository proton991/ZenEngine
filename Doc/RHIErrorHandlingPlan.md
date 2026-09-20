# Unified RHI error handling implementation plan

Status: proposed, 2026-09-20. Based on revision `b28c1f0d3925d1ceedc8c58901421bc3d13c81a4`. This document defines future work; it does not implement the changes or certify production readiness.

Unify failure reporting across RHI with a backend-neutral error record, checked results at operation boundaries, and a latched failure inside command recording. Preserve separate information about what failed, whether GPU work was accepted, and whether the renderer can continue. The first delivery must prevent execution with invalid resources or command buffers and terminate failed frame processing predictably.

Scope: the active `Graphics/RHI`, `Graphics/VulkanRHI`, and `Graphics/RenderCore/V2` paths, their application callers, and their tests. Preserve the existing inline/threaded executor and graphics/transfer behavior. The [async-compute plan](AsyncComputeImplementationPlan.md) is separate proposed work; this plan must not depend on its scheduler being implemented.

## 1. Current gaps and reusable infrastructure

| Area | Current behavior | Planned change |
| --- | --- | --- |
| [VulkanCommon.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanCommon.h) | `VKCHECK` discards the boolean from `CheckVkResult`; logging and assertions do not prevent release builds from continuing. | Replace ignored checks with checked native-result conversion and explicit propagation. |
| [VulkanCommandList.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp) | Command-buffer reset/end results are unchecked; begin/end state can advance without confirmed success. | Advance state only after success; latch failure and exclude invalid buffers from submission. |
| [VulkanDescriptorSetState.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp) | Descriptor resolution can fail while the outer preparation API returns `void`; draw/dispatch callers continue. | Propagate preparation failure and gate the native draw/dispatch in the same command. |
| [VulkanDescriptorPool.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp) | Bindless registration during parameter preparation retains resources and occupies slots; releasing a recording epoch does not unregister those slots. | Track new registrations by transaction and undo failed, unsubmitted publication without disturbing existing registrations or accepted users. |
| [VulkanMemory.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanMemory.cpp), [VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [VulkanTexture.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp) | Resource initialization mixes ignored checks, unchecked calls, and partial-construction cleanup. | Return validated objects or structured errors; unwind partial construction consistently. |
| [DynamicRHI.h](../ZenCore/Include/Graphics/RHI/DynamicRHI.h) | Factories return raw pointers, finalization is `void`, waits use `bool`, and completion getters expose no error directly. | Introduce checked creation, finalization, progress, and wait boundaries. |
| [RHICommon.h](../ZenCore/Include/Graphics/RHI/RHICommon.h), [VulkanQueue.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanQueue.cpp) | `RHISubmissionResult` already distinguishes success, rejection, and fatal/uncertain submission. | Retain this distinction and attach the cause and accepted submission receipts. |
| [RHICommandListExecutor.h](../ZenCore/Include/Graphics/RHI/RHICommandListExecutor.h), [implementation](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp) | Tickets, retained batches, queue serial snapshots, and terminal blocking already exist; failures also use exceptions and strings. | Carry structured errors end to end, preserve the original cause, and complete all affected tickets. |
| [RHIThread.h](../ZenCore/Include/Graphics/RHI/RHIThread.h), [implementation](../ZenCore/Source/Graphics/RHI/RHIThread.cpp) | Queued jobs are opaque functions; `Invoke` signals its event only inside the execution callback. Resource/context destruction also uses `Invoke`. | Add checked admission and explicit cancellation completion; keep ordered cleanup available after normal work is blocked. |
| [VulkanSwapchain.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp) | Presentation tracks pending synchronization separately for enqueued WSI outcomes and allocation rejection, but exposes a boolean outcome. | Preserve that distinction in typed presentation results and image/semaphore/fence state transitions. |
| [RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) | Scheduled resource history and submission retirement already distinguish CPU handoff from GPU completion. | Preserve those guarantees when recording, submission, or presentation fails. |

Local review probes reproduced three containment failures: an unsuccessful `vkEndCommandBuffer` still reached queue submission; an undersized uniform buffer still reached dispatch; and forced descriptor allocation failure still reached dispatch. Promote these scenarios into permanent regression coverage. Their temporary local review files are not dependencies of this plan.

## 2. Required contracts

1. A failed operation cannot publish a usable resource, executable command buffer, or successful completion value.
2. A failed native preparation step prevents the dependent native operation immediately. Checking only at the end of a frame is insufficient.
3. CPU queue acceptance, native submission acceptance, GPU completion, presentation queue acceptance, and presentation resource retirement remain distinct events.
4. Submission serials advance only for accepted native submissions. Completion advances only from trustworthy completion evidence. Serial values from different queues are not interchangeable.
5. Work already accepted by a queue retains its resources, command storage, descriptor ownership, and bindless epochs until valid retirement or terminal teardown. A later error does not undo submission.
6. Errors propagate without deliberate C++ exceptions. Assertions diagnose programming mistakes; release builds must also reject invalid work or use an explicit fatal path.
7. The first causal error remains available. A later device-loss or uncertain-state observation still escalates the terminal state, even if a less severe error was recorded first.
8. Every accepted CPU job completes its ticket/event exactly once, including jobs cancelled because an earlier job failed. Admission rejection returns directly and cannot leave a caller waiting for a job that was never queued.
9. Expected conditions such as timeout, not-ready, and swapchain recreation requests do not automatically poison the device.
10. Discard accounts for recording side effects as well as command storage. A failed unsubmitted transaction cannot strand a new bindless registration or release a registration still needed by accepted work.
11. Terminal failure blocks normal GPU work while permitting the ordered cleanup and diagnostics needed for safe teardown. The RHI worker remains available until those obligations are resolved.

These contracts apply equally to direct backend callers, the inline executor, and the threaded executor.

## 3. Common error and result model

Add `ZenCore/Include/Graphics/RHI/RHIError.h` for the shared vocabulary and result helpers. Keep Vulkan types out of this header.

| Proposed type | Responsibility |
| --- | --- |
| `RHIErrorCode` | Start with `None`, `InvalidArgument`, `Unsupported`, `OutOfHostMemory`, `OutOfDeviceMemory`, `DeviceLost`, `BackendFailure`, and `Cancelled`. Extend only when a caller needs a distinct handling decision. |
| `RHIError` | Fixed-size cause record: error code, native API identifier when applicable, signed native code, operation identifier/static name, source location, and optional stable resource ID. |
| `RHIStatus` | `[[nodiscard]]` success/error result for an operation with no value. |
| `RHIResult<T>` | `[[nodiscard]]` tagged value/error result with explicit checked access and support for move-only values. Failure contains no usable `T`; do not require a default-constructed resource. |
| Submission receipt/result | Existing `RHISubmissionResult`, structured cause, and accepted serials for the affected queues. The outcome describes acceptance; the error describes the cause. |
| Wait/progress results | Typed completion, timeout/not-ready, or failure; progress carries valid serial snapshots separately from its status. |
| Acquire result | Typed acquired, incomplete, recreation, surface-loss, or failure outcome; a valid image is present only after successful acquisition. |
| Present result | Typed surface outcome and cause, independently carrying queue acceptance: `NotAttempted`, `Rejected`, `Enqueued`, or `Uncertain`. Pending image/semaphore/fence ownership stays with the swapchain. |
| CPU job admission result | Distinguish accepted work, queue-full for nonblocking dispatch, and lifecycle rejection with its cause. Admission is separate from execution completion and native submission. |

Use a small C++20-compatible implementation consistent with existing project types; do not require a language-standard upgrade or a new dependency solely for `std::expected`. Error access must not throw. Invalid result access is an invariant violation with a release-active failure path.

Capture an error without allocating or formatting a message. Source/operation strings must have static lifetime; do not retain temporary strings or resource pointers. Attach frame, batch, queue, and submission metadata in the owning batch/diagnostic record. Format a readable message at the reporting boundary. This guarantees allocation-free capture, not that logging, promises, or all CPU allocation paths survive host-memory exhaustion.

Do not add a process-global mutable `lastError`. Return synchronous errors to the caller; keep recording errors on their context; publish terminal errors through the executor's synchronized state. Optional resource failure must not overwrite an unrelated batch or terminal error.

## 4. Classify native results by operation

Create one Vulkan error converter and reuse it throughout the backend. Keep operation-specific handling adjacent to the native call: the sign of `VkResult` alone cannot decide whether to continue, retry, recreate, or stop. A helper may capture call-site information, but must not hide an early return or silently discard the result.

| Condition | Immediate handling | Scope/policy |
| --- | --- | --- |
| Invalid argument or unsupported request | Return a structured error before issuing invalid native work. | Caller chooses a supported alternative; assert separately for internal invariant violations. |
| Resource allocation/map/pipeline failure | Clean up the failed construction and return no usable output. | Optional callers may use an existing fallback. Required initialization or frame work fails explicitly. |
| Descriptor pool exhausted/fragmented | Use the pool manager's bounded growth/retry policy where valid. Propagate failure if growth fails. | Do not confuse local pool capacity with device-wide memory exhaustion. |
| Command reset/begin/end or descriptor preparation failure | Mark recording failed and discard the unsubmitted transaction. | No further dependent recording or submission. |
| Queue submit rejects before any work is accepted | Return `eRejected` only when the native contract establishes no accepted work and no uncertain GPU state. | Release/discard unsubmitted work correctly; no implicit replay. |
| Queue failure after an accepted prefix, or uncertain native state | Preserve accepted receipts and return `eFatal`. | Stop new submissions; retain possibly in-flight ownership. |
| `VK_TIMEOUT` / `VK_NOT_READY` | Report incomplete/timeout. | Never retire or reuse resources on this basis. |
| Acquire/present `VK_SUBOPTIMAL_KHR` | Preserve the successful operation and request recreation. | An acquired image remains valid for its defined lifecycle. |
| `VK_ERROR_OUT_OF_DATE_KHR` | Request swapchain recreation; do not invent an acquired image. | Track acquire and present separately; previously submitted rendering remains accepted. |
| `VK_ERROR_SURFACE_LOST_KHR` | Suspend the viewport and route surface reconstruction through the window-owning thread. | Preserve device health unless a separate terminal error is observed. |
| Present queue acceptance | Apply the presentation transition table in section 5 independently of the surface outcome. | A WSI error can still enqueue synchronization work; presentation rejection does not undo rendering. |
| `VK_ERROR_DEVICE_LOST` | Publish terminal device failure once, stop normal GPU work, and initiate controlled teardown. | First delivery does not attempt transparent recovery. |
| Other native failure | Preserve native code and operation; apply the relevant API's output/state guarantees. | Default to stopping the affected transaction; escalate when device or submission state is uncertain. |

The [Vulkan result-code documentation](https://docs.vulkan.org/refpages/latest/refpages/source/VkResult.html) distinguishes successful status codes from errors and warns that output values generally cannot be used after failure. In particular, an unsuccessful [command-buffer end](https://docs.vulkan.org/refpages/latest/refpages/source/vkEndCommandBuffer.html) leaves the buffer invalid. These are correctness requirements, independent of whether validation is enabled.

## 5. Recording, construction, and submission boundaries

### Recording

Keep the public draw/copy/bind command API lightweight. Add a sticky error to the native command context and checked results to internal helpers such as `PreDraw`, `PreDispatch`, descriptor resolution, uniform upload preparation, and command-buffer finalization.

- After a command fails, stop executing subsequent recorded commands against that context. Also guard subsequent native calls inside the command that failed; an outer executor check cannot prevent its dispatch by itself.
- Finalize every list required by a transaction successfully before starting that transaction's native submissions. On finalization failure, discard all of its unsubmitted platform lists, including earlier successfully finalized lists, and current/finalized workloads still owned by its contexts.
- Change reset/begin/end state only after the corresponding native call succeeds. A failed buffer cannot enter the executable/submitted cache.
- Discard recorded resource references and bindless epochs through the existing lifetime machinery, together with the registration rollback below. Keep ownership for any earlier accepted work separate.
- Reset the sticky error only after discard/reset clears the transaction's pending dependencies and invalidates cached pipeline, descriptor, and uniform state that could refer to released resources. Detached lists can share a context; clearing an error when merely acquiring another CPU list is unsafe.
- Define how command destruction/arena recycling proceeds for unexecuted commands so stopping execution cannot strand CPU-owned payloads.

### Bindless publication and rollback

Give each native recording transaction a side-effect journal shared by its lists and contexts. In `SetShaderParameters`, validate the complete bindless parameter group before mutating registrations, then journal every newly created registration using its slot, generation, and transaction owner. Prevalidation does not replace rollback: later preparation, finalization, or submission can still fail. Reserve journal and retirement bookkeeping before mutation so rollback does not need a fresh allocation to release an unpublished registration.

New implicit registrations remain provisional until their transaction succeeds. Other transactions and public registration queries must not adopt them while provisional; existing registrations are borrowed and never entered as newly owned journal entries. Native descriptor writes may already occur during preparation, so provisional ownership must also cover pending writes and retained resource/texture-owner references.

| Transaction outcome | Registration handling |
| --- | --- |
| Successful transaction | Commit new registrations to the manager's existing persistent-registration contract. Keep accepted workload/epoch lifetime tracking when GPU work exists; a successful transaction with no native work resolves publication without creating a serial. |
| Discard or rejection with no accepted work | Remove only journal-owned registrations whose generations still match. Remove their pending writes, invalidate descriptor state, and release references/slots through valid retirement. Releasing the recording epoch alone is insufficient. |
| Accepted prefix or uncertain submission | Preserve registrations and references potentially used by that work. The first implementation may conservatively retain the whole journal until safe terminal teardown. |

Explicit caller-owned registrations made before recording remain caller-owned. Rollback must not unregister them, restore stale generations, or overwrite descriptors that accepted users may observe. End transaction ownership only after commit, rollback, or transfer to retained terminal ownership; neither CPU-list reset nor acquiring another shared-context list creates that boundary.

### Resource construction

Make buffer, texture/view, sampler, shader, pipeline, synchronization object, and viewport initialization return checked results. Initialize handles to null and track only successfully constructed objects. Clean up in reverse ownership order, including any partial outputs the specific native API requires the caller to destroy.

Factories return a validated owning resource result, using the project's resource-reference types. Document whether each wrapper adopts or increments an initial reference; do not change this implicitly during migration. Publish resources to caches, descriptor tables, bindless slots, or renderer state only after successful initialization. Preserve existing borrowed-resource lifetime contracts for command recording.

Migrate each factory with its direct callers and fake implementations. Temporary adapters are acceptable only if they preserve explicit failure handling; an adapter that logs and returns an invalid object is not acceptable. For an optional asset, the caller owns fallback selection. The backend must not silently replace failed resources or change their format/size.

### Submission and frame aggregation

Make dependency preparation, `FinalizeCommandLists`, platform-list enqueueing where fallible, and `FlushAllGPUCommands` propagate the common error vocabulary. Preserve the existing flush contract: queued work is consumed on rejection and is not implicitly retried.

Clarify `eRejected`: no native work was accepted in the specified transaction. CPU recording state may already have changed and must be discarded or restored. `eFatal` covers partial submission or uncertainty; it does not necessarily mean physical device loss.

Return explicit per-queue acceptance receipts, including an accepted prefix on failure. Keep lifetime watermarks separate from receipts if existing snapshots include earlier batches. An empty successful transaction must not manufacture a serial.

Extend `RHIBatchResult` to carry structured errors and a typed presentation outcome. Aggregate the main render submission and the separate presentation submission: if rendering was accepted and presentation preparation/submission fails, the frame must retain the rendering receipts and cannot claim that nothing was submitted. A normal out-of-date presentation requests recreation without erasing accepted rendering or automatically declaring device loss.

Preserve RenderDevice's scheduled-versus-confirmed state model. A rejected asynchronously queued frame invalidates dependent scheduled history; the initial policy remains to stop the executor and cancel dependent work. Do not attempt to replay frames or roll back GPU-visible work automatically. A synchronous, entirely unsubmitted operation may be retried only after explicit discard/rollback and a deliberate caller decision.

### Presentation acceptance and synchronization ownership

Track the rendering submission, presentation-copy submission, and `vkQueuePresentKHR` outcome separately. Presentation acceptance does not create a graphics submission serial or prove presentation resource retirement. Use the following transitions for the active single-swapchain path; recreation/suspension policy remains separate.

| Native present outcome | Queue acceptance | Image and synchronization handling |
| --- | --- | --- |
| Call skipped, including acquisition timeout or failed preparation | `NotAttempted` | Preserve any acquisition already held and its outstanding synchronization obligations. |
| `VK_SUCCESS` / `VK_SUBOPTIMAL_KHR` | `Enqueued` | Hand off the acquired image; retain wait semaphores and track any supplied presentation fence as pending. Suboptimal requests recreation. |
| `VK_ERROR_OUT_OF_DATE_KHR` / `VK_ERROR_SURFACE_LOST_KHR` | `Enqueued` | Hand off the acquired image; semaphore waits still execute and any supplied presentation fence is pending. Retain synchronization while recreating/suspending the viewport. |
| `VK_ERROR_OUT_OF_HOST_MEMORY` / `VK_ERROR_OUT_OF_DEVICE_MEMORY` | `Rejected` | The call leaves referenced state unchanged. Preserve the acquired image and rendering-complete semaphore obligation; do not mark the supplied presentation fence pending. |
| Device loss or failure without a proven rejection guarantee | `Uncertain` | Quarantine affected ownership for terminal handling; infer neither semaphore consumption nor fence completion. |

These acceptance rules follow [vkQueuePresentKHR](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html). A supplied [presentation fence](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html) provides resource-retirement evidence, not proof that the image has finished displaying. Without presentation fences, preserve the existing reacquisition-based retirement and terminal WSI teardown policy; graphics completion alone cannot recycle presentation wait semaphores.

For the first delivery, present allocation rejection after accepted rendering terminates the frame transaction; there is no automatic retry or second signal of its unconsumed binary semaphore. Teardown must distinguish the accepted rendering signal from the rejected presentation wait, avoid waiting on a fence that was never enqueued, and destroy retained objects only after their actual users are safe. Keep these transitions inside the swapchain; callers must not clear acquisition state merely because presentation returned an error.

## 6. Executor, progress, and terminal handling

Use an explicit executor lifecycle: running, blocked by a terminal frame/backend failure, shutting down, destroyed. Keep viewport suspension/recreation separate from this device/executor lifecycle.

The RHI thread owns backend mutation. Publish a coherent terminal snapshot through the executor's existing synchronization model; do not expose a concurrently mutated error struct alongside an atomic flag. Preserve the first causal error and record a later terminal escalation separately when needed.

### Job admission, cancellation, and teardown access

Replace execution-only queue entries with owned work items containing a job class, execution action, cancellation action, and shared completion state. Move promise/event ownership out of `Invoke`'s execution-only callback so cancellation can produce a checked `Cancelled` result linked to the terminal cause. Use one completion helper for execution and cancellation: publish the result before signaling the event, exactly once. Dropping a packaged task or relying on a broken promise is not a cancellation protocol.

Synchronize lifecycle checks, admission, and the claim to execute/cancel a queued item under the queue's synchronization model. Only an unstarted job may be cancelled; an executing job completes through its own checked path. Run actions and release payloads outside the queue mutex, on RHI when backend-owned references require it. Reentrant `Invoke` on RHI and inline execution obey the same lifecycle checks and execute permitted work directly without waiting on their own queue.

`Dispatch` returns checked admission. `Invoke` returns immediately on rejected admission instead of waiting on its completion event. An accepted item must run or cancel; a blocking producer awakened by terminal failure or shutdown must recheck admission. Wake queue-capacity and work waiters on lifecycle transitions even if capacity is still full. Nonblocking maintenance may coalesce or skip a queue-full request, but must distinguish that from terminal rejection.

| Operation class | Running | Blocked by terminal failure | Shutting down | Destroyed |
| --- | --- | --- | --- | --- |
| Normal rendering, creation, mapping, submission, acquisition/presentation | Admit | Reject new work; cancel unstarted normal jobs | Reject new work; drain accepted jobs if healthy, otherwise cancel unstarted jobs | Reject without backend access |
| CPU diagnostic/progress snapshots | Read published state | Read published state | Read published state | Read only retained CPU state |
| Completion polling and waits | Admit with checked outcomes | Only bounded maintenance needed for retirement/escalation | Only teardown coordination | Reject native access |
| Resource/context release, discard, and teardown control | Ordered RHI cleanup | Permit under lifetime gates | Permit until cleanup admission is closed | Backend final release is an invariant violation |

Internal cleanup classification is not a public way to bypass terminal blocking. Route `RHIResource::ReleaseReference`, `IRHICommandContext::OnFinalRelease`, unexecuted-command destruction, and viewport/backend teardown through this path. A release may transfer an owner to retained teardown storage when GPU use is uncertain; it must not report successful destruction or silently lose the owner. Cleanup failures remain secondary diagnostics.

Keep the worker alive while accepted jobs, cancellation payloads, and resource/context owners can still generate cleanup. Quiesce producers, drain permitted cleanup, resolve retained GPU ownership under the shutdown policy, and verify backend-child ownership before destroying the backend and closing cleanup admission. Only then stop/join the worker. Do not fall back to executing native destruction on a caller thread after worker shutdown. Late final releases use a release-active invariant path; tests must prove normal teardown leaves none.

### Terminal response and shutdown

When terminal failure is observed:

1. Close normal-job admission, wake blocked producers, and claim unstarted normal jobs for cancellation before native execution; retain permitted cleanup access.
2. Complete each affected promise/event through the common completion helper, with cancellation linked to the original terminal cause. Synchronous `Invoke` callers must also unblock with a checked outcome.
3. Retain accepted/uncertain GPU owners, discard definitely unsubmitted work, and invalidate speculative renderer history.
4. Publish one primary diagnostic containing operation, native code, frame/batch, queue, and relevant serials. Cleanup failures remain secondary diagnostics.
5. Let the application report failure and close cleanly through CPU/platform facilities; do not require the failed renderer to draw an error dialog.

Convert RHI-owned `throw` paths to explicit statuses or deliberate invariant handling. Existing catches around foreign/library exceptions may remain as a last-resort containment boundary during migration, but must not be the normal GPU-error path. Disabling exceptions throughout the engine or redesigning every CPU allocator is outside scope.

Progress polling must publish failure even when no frame ticket is pending. Replace ambiguous wait booleans with completed/timeout/error outcomes and make `WaitDeviceIdle` checked. Do not report a failed query as serial zero, a completed wait, or permission to recycle resources. Last known valid progress may remain readable alongside the failure status.

On shutdown, do not force-retire batches merely because an idle wait failed. For confirmed device loss, follow Vulkan's legal destruction rules; the lost logical device cannot be reset and its children still require cleanup. For uncertain state without a device-loss guarantee, preserve ownership until safe teardown or controlled process termination. Bound polling/retry loops after terminal failure; do not promise that an arbitrary unresponsive driver call can be interrupted. See [Vulkan device loss](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device).

Preserve the window-thread and message-pumping contracts in the [threading plan](RenderCoreRHIThreadingPlan.md), including surface creation, deferred resize callbacks, and Windows waits that service sent messages. Error paths must not introduce window/RHI thread deadlocks.

## 7. Implementation sequence

Each phase should leave the tree building and include its targeted tests. Prioritize recording containment before broad API migration.

| Phase | Work | Exit condition |
| --- | --- | --- |
| 1. Contracts and foundations | Inventory active native result checks, deliberate throws, factories, waits, and presentation paths. Add common result/error types and Vulkan conversion. Define submission/presentation acceptance, transaction journals, work-item completion, and lifecycle admission contracts. | Types support move-only outputs; capture is allocation-free; classification tests distinguish expected statuses and queue acceptance from errors. |
| 2. Submission plumbing and containment | Carry errors through dependency preparation, finalization, submission, executor tickets, and RenderDevice. Add the recording latch, bindless journal/rollback, and gates for reset/begin/end, descriptor preparation, direct/indirect draw, and dispatch. Implement checked job admission/cancellation and permitted cleanup dispatch. | The three review regressions pass; failed recording cannot reach native draw/dispatch/submit or strand a new registration; accepted prefixes remain owned; cancelled jobs unblock callers while cleanup remains available. |
| 3. Checked resource construction | Migrate memory/maps, buffers, textures/views, samplers, shaders/pipelines, descriptor/synchronization allocation, and callers. Update caches and optional fallbacks. | Failure injected at each construction stage produces no published invalid object, leaked partial native object, or double release. |
| 4. Progress, surfaces, and lifecycle | Migrate initialization, waits, progress queries, acquire/present/resize, shutdown, and remaining RHI-owned exception paths. Implement presentation ownership transitions and ordered cleanup/worker shutdown. Publish terminal diagnostics to the application. | Timeout/recreation remain nonterminal; device loss blocks normal GPU entry points while cleanup remains permitted; WSI acceptance is accurate; tickets and shutdown complete without fabricated GPU completion. |
| 5. Integration and cleanup | Remove ignored-check compatibility paths, convert affected exception-based tests, document contracts and diagnostic output, and run the validation matrix. | Active RHI call sites handle failures explicitly in Debug and `NDEBUG` builds; all acceptance criteria below pass. |

Implementation touchpoints beyond the table in section 1 include [RHICommandList.h](../ZenCore/Include/Graphics/RHI/RHICommandList.h), [RHIThread.h](../ZenCore/Include/Graphics/RHI/RHIThread.h), [RHIThread.cpp](../ZenCore/Source/Graphics/RHI/RHIThread.cpp), [VulkanDescriptorPool.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp), [VulkanContext.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp), [VulkanDevice.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), [VulkanViewport.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), and [VulkanSwapchain.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp).

Register new sources/tests in [ZenCore/CMakeLists.txt](../ZenCore/CMakeLists.txt) and [ZenSamples/CMakeLists.txt](../ZenSamples/CMakeLists.txt). Some RenderCore test targets compile RHI sources directly, so adding a new implementation file to ZenCore alone is insufficient.

Apply repository C++ conventions throughout: one return at the end of each function, explicit types except iterators, short necessary lambdas, project containers such as `HeapVector`, shared helpers for repeated logic, no exception-based error handling, and the project `.clang-format`.

## 8. Validation and acceptance

Use scoped native-call fault injection and the fake RHI backend for deterministic failures. Intercept invalid downstream work instead of deliberately sending it to the real GPU. Ordinary tests must not require exhausting real GPU memory, hanging a GPU, or removing a device.

| Coverage | Required checks |
| --- | --- |
| Common results | Value/error exclusivity, move-only ownership, source metadata, native-code preservation, no allocation during capture, and checked access behavior. |
| Recording/descriptors | Inject reset/begin/end and descriptor/uniform preparation failures. Count native calls to prove no dependent draw/dispatch or invalid submission occurs. Cover indirect commands and multi-list discard. |
| Recording side effects | A valid bindless parameter followed by an invalid one must leave no new registration. Also inject failure during registration and after a valid group, including later-command/finalization failure and native rejection. Verify journal-owned slots, writes, and references retire correctly; preexisting registrations survive. Cover duplicate bindings, shared contexts, success without native work, accepted prefixes, uncertainty, and reuse after explicit discard. |
| Resources | Fail each construction stage, including pipeline layout and partial outputs. Verify cleanup, cache exclusion, bindless rollback, and reference adoption. |
| Submission | Rejection before acceptance, failure after a prefix, multiple queues, and rendering accepted before presentation fails. Verify accurate outcomes/receipts and retained lifetimes. |
| Executor | Inline/threaded parity, original cause preservation, later device-loss escalation, and optional failure isolation. Exercise admission rejection, a full queue with blocked producers, queued synchronous `Invoke` cancellation, execution/cancellation races, and reentrant calls. Verify one result/event completion and one payload release per accepted job. |
| Progress/waits | Timeout/not-ready, query failure with no pending frames, and idle-wait failure. No false completion, recycling, or serial advancement. |
| Surfaces | Suboptimal, out-of-date at acquire and present, surface loss, resize, and initialization failure. Inject present host/device allocation rejection after accepted rendering. Test enqueued WSI errors versus rejection with and without presentation fences: no early semaphore reuse, second signal, invented receipt, or wait on a never-enqueued fence. Verify acquisition ownership, recreation signaling, and thread affinity. |
| Retirement/shutdown | Accepted command storage, upload staging, resource references, descriptor pools, and bindless epochs remain alive correctly. Release the last resource/context reference after terminal failure and during cancellation; verify ordered cleanup before backend destruction/worker stop, retained uncertain owners, no stranded caller, and window-message waits. |

Extend the existing `VulkanRHITest`, `RenderCoreTest`, and `VulkanRHIIntegrationTest` suites. Place deterministic executor tests with [RenderCoreTests.cpp](../ZenSamples/RenderCoreTest/RenderCoreTests.cpp) and [RHIThreadingTests.inl](../ZenSamples/RenderCoreTest/RHIThreadingTests.inl); reuse [ScopedVulkanCall.h](../ZenSamples/CommonTest/ScopedVulkanCall.h) for native fault injection where suitable. Preserve coverage when replacing `EXPECT_THROW` assertions with status assertions.

Run affected suites in Debug and RelWithDebInfo (`NDEBUG`) and exercise both timeline and fence submission paths. Retain real-GPU happy-path rendering/readback, upload, descriptor, swapchain, and synchronization-validation coverage. Record environment, modes, counts, failures, and any skipped capability-dependent cases in a companion verification document when implementation is complete.

The change is ready for production evaluation when:

- No active RHI error path relies on a debug-only assertion to stop invalid execution.
- Failed creation, recording, or submission cannot publish invalid objects or false success/completion.
- Discard rolls back new unsubmitted registrations and pending writes without invalidating preexisting or accepted users.
- Accepted and uncertain GPU work retains all required ownership until safe retirement/teardown.
- Presentation surface outcomes preserve the correct queue-acceptance and image/semaphore/fence ownership transitions.
- Optional failures and expected surface/wait conditions follow their defined policy without accidental global shutdown.
- Terminal failures stop dependent work, preserve actionable diagnostics, and unblock callers without repeated error storms.
- Admission rejection never waits for execution; cancellation completes once; cleanup remains available until backend ownership is resolved and the worker can stop safely.
- The validation matrix passes with no new synchronization-validation errors, and ignored native-result checks have been audited out of the active path.

## 9. Initial production policy and deferred work

For the first implementation, continue after explicitly supported local failures, recreate affected presentation surfaces when safe, and stop the renderer on device loss or an unrecoverable frame transaction. Automatic memory-budget adaptation, asset eviction/retry, device recreation, shader/pipeline fallback generation, and crash-dump SDK integration are follow-up projects with separate correctness requirements.

This conservative terminal policy follows a practice visible in mature engines: Unreal's [GPU crash documentation](https://dev.epicgames.com/documentation/unreal-engine/dealing-with-a-gpu-crash-when-using-unreal-engine) emphasizes crash diagnostics and termination, while [Godot's Vulkan backend](https://github.com/godotengine/godot/blob/master/drivers/vulkan/rendering_device_driver_vulkan.cpp) contains both recoverable resource-creation failure returns and a fatal device-loss path. These examples inform the policy; they do not imply that either engine uses the exact result types proposed here.

Design diagnostic metadata so later breadcrumbs, vendor crash dumps, and device-recreation work can attach to it. Do not make those integrations prerequisites for basic containment. Unrelated RHI feature bugs, validation build configuration, legacy `Graphics/Val` migration, and the proposed async-compute scheduler remain separate work.
