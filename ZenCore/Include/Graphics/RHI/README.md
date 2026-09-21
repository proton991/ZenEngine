# RHI binding and lifetime contracts

## Resources and recorded commands

Raw RHI resource arguments are borrowed. Callers must keep buffers, textures, samplers, pipelines, and their dependencies alive from command recording until every GPU use completes, or until unsubmitted work is definitively discarded. `Destroy*`/`ReleaseReference` releases a reference immediately and does not wait for GPU work. A submitted command list reset does not imply GPU completion. RenderCore's deferred retirement provides this protection for its managed resources; direct RHI callers must provide it themselves.

`RHICommandList::Create(context)` transfers ownership of the context to the command list. Destroying that list destroys its context. Rendering-layout pointers passed to recorded `BeginRendering` commands must remain valid and unchanged until command execution. Parameter bytes and copied region arrays are owned by recorded commands; resource pointers remain borrowed.

`RHIResource::GetStableId()` identifies a resource object throughout its lifetime. Reusing the same pooled allocation preserves its ID; replacing the object produces a new ID. Descriptor and pipeline cache keys use this identity. Resource replacement must create a new RHI object and retain the old one through its final GPU use.

`RHITexture::CreateView`, `RHITexture::GetDefaultView`, and `CreateTextureView` return borrowed views owned by the base texture. Do not release the texture-owned reference. If a caller takes an additional view reference, it must also keep the base texture alive. Keeping only the view alive does not retain the image. The base texture releases its owned views before destroying the image.

Descriptor-pool retention is independent of resource retention. The shared Vulkan lifetime tracker protects ordinary descriptor pools from recording through completion, but this does not retain the buffers, textures, or samplers described by their sets.

`RHIBuffer::SetTexelFormat(format)` creates one buffer-owned texel view over the buffer's declared byte size, excluding allocation padding. The buffer must have the appropriate texel-buffer usage, and its size and format must meet the device's texel-view requirements. Repeating the same format after successful creation is a no-op; changing it throws and preserves the existing view, including recorded descriptors that reference it. Native creation failure throws without publishing a view or fixing its format, so the caller may retry. The buffer destroys its view when released under the ordinary resource lifetime contract.

## Vulkan queue sharing

RHI-created buffers and textures permit use on both graphics and async-compute queue families. Resources with transfer-source or transfer-destination usage also permit the transfer family. Creation uses concurrent sharing across distinct families, or exclusive sharing when every permitted queue belongs to the same family. Texture views inherit the base image's sharing. This policy does not apply to native swapchain images or externally created Vulkan resources.

Sharing removes the need for queue-family ownership transfers; it does not order accesses. Dependent work on different queues still requires synchronization, and image layouts and access/stage masks must match the operations and queue capabilities. Concurrent sharing can have a device-dependent performance cost; there is currently no per-resource sharing-mode selection in the RHI.

`RHICommandList::AddSubmissionDependency({producerQueue, serial})` records a dependency on already-submitted work. Command-list finalization prepares all dependencies before translating the batch. Vulkan waits on the producer queue's timeline semaphore at `ALL_COMMANDS`, coalescing repeated waits and omitting waits between contexts on the same native queue. Queue-owned semaphores remain alive through device shutdown. Devices without timelines use submission-completion waits. An invalid/unsubmitted serial or failed dependency wait blocks submission; the consumer must never execute without its dependency. Raw native contexts can use `PrepareSubmissionDependencies` before recording their work. The existing `AddSignalSemaphore`/`AddWaitSemaphore` API remains available; callers own those semaphore lifetimes and must successfully submit a binary-semaphore producer before its consumer.

`RHISubmissionDependency::kLatestSubmitted` resolves on the RHI thread to the producer queue's most recent accepted serial, after earlier CPU batches have executed. It never refers to future work or to another producer in the same unsubmitted batch. Command-list detach, reset, and rollback include the recorded dependencies. Neither a GPU dependency nor submission acceptance is CPU permission to reuse staging memory or destroy resources.

RenderDevice owns `RenderSubmissionHistory`, which tracks exact writers/layout transitions and concurrent readers per physical resource. An overwrite depends on the relevant readers, and cross-graph consumers use exact accepted queue/serial points or pending batch-group references. History preparation is transactional. Frame history commits at CPU handoff and is confirmed by the batch result; standalone graphs commit after successful synchronous CPU processing. Failed queued work invalidates speculative history and blocks dependent work. RenderGraph/RDGExecutor consume supplied capabilities and producer provenance to plan dependencies and barriers; they never query native progress or submit work. Explicit external-state invalidation still requires the caller to arrange completion and visibility of external work.

`RHICommandContextType` is the common logical queue vocabulary. `RHIQueueCapabilities` is the authoritative snapshot for native queue equivalence and async dependency support. Equal native IDs allow shared barrier history, but each logical queue retains its own serial timeline. All graph submissions, including a single group, use the grouped executor path. Standalone submission processes synchronously without ending the native frame. Frame adapters own presentation and the successful frame boundary; the legacy inline presentation adapter preserves retryable copy rejection.

`RHISubmissionState` exposes read-only inspection of exact producer points and successful CPU processing through `Resolve()` / `IsSubmissionFinished()`. Its `Queue()`, `Accept()`, `FinishSubmission()`, and `Fail()` methods are private to `RHICommandListExecutor`; a test-only friend accessor supports focused fault injection. Groups can be accepted before the whole batch finishes; a later fatal failure invalidates their producer state. `RHISubmissionTicket::Wait()` waits for the CPU result, not GPU completion, and returns a reference borrowed from a live ticket. `RHIBatchResult::requiredSerials` contains submitted serials required for retirement. `RHICompletionSet` is the shared per-queue value type.

Progress access distinguishes cached reads, queries, refresh requests, and waits:

| API | Behavior |
| --- | --- |
| Executor `GetCachedSubmittedSerials()` / `GetCachedCompletedSerials()` | Read published values only in either CPU mode. No backend calls or worker waits. RenderDevice's `GetCachedCompletedSerials()` preserves this contract for resource reuse checks. |
| RHI `QueryLastCompletedSerial(queue)` / `QueryCompletedSerials()` | Use the facade's existing query policy: raw Vulkan and inline executor queries poll native progress without waiting for GPU completion; threaded executor queries read published values without a worker round trip. They do not guarantee fresh GPU progress in threaded mode. |
| Executor `PollGPUProgress()` | Request the existing coalesced progress/retirement sweep. It executes inline in inline mode and is queued in threaded mode; a request does not promise that the next cached read already sees it. |
| RHI `WaitForCompletion(queue, serial)` | Wait for a required GPU serial, subject to the supplied timeout. |

`GetSubmittedSerials()` / `GetLastSubmittedSerial()` read known acceptance values without polling GPU completion. Raw `VulkanQueue::GetLastCompletedSerial()` remains its existing cached accessor; `VulkanRHI::QueryLastCompletedSerial()` performs the native poll before reading it. These names preserve the polling/worker behavior while making cached-only callers explicit. None of the getters authorize reuse until the required serials and pending submission conditions are satisfied.

RenderCore's `ResourceRetirement` combines required serials with one optional pending frame ticket. RenderDevice drains the previous pending frame before another handoff and blocks resource reuse if that invariant is violated. Pool readiness is nonblocking; frame reuse may wait for the CPU result and required GPU serials. Resolving a ticket folds its serials into the requirement, while fatal/uncertain work remains protected. The general RHI executor can still queue multiple batches. Staging uploads already establish native acceptance and use `RHICompletionSet` directly. Vulkan recording epochs and lifetime tracking remain backend-local.

On timeline-capable devices, upload/transfer flush returns after native submission without waiting for GPU completion. Staging blocks and destination references retire against their completion serials. Pool pressure, frame-slot reuse, and shutdown can still wait. CPU staging copies and immediate native submission through `RHIThread::Invoke` remain synchronous. Transfer-incompatible work, including mipmap blits and transitions involving graphics-only stages, continues on the graphics queue; timeline-disabled devices retain the synchronous transfer fallback. Automatic queue assignment is implemented for eligible passes requesting async compute. It remains opt-in (`async_compute=auto` or `--async-compute=1`) and falls back to graphics when policy, queue availability, dependency support, or pass/resource legality prevents compute placement. GPU overlap/performance acceptance remains open; see [the async implementation plan](../../../../Doc/AsyncComputeImplementationPlan.md).

## Vulkan lifetime tracking and counters

`VulkanRHI` owns one `VulkanLifetimeTracker` for uniform-buffer storage, bindless recording epochs, and ordinary descriptor pools. Owners hold integer lifetime IDs; native workloads carry a single deduplicated ID list. The tracker owns all pending recording counts and the latest accepted serial for each referencing queue. Merging preserves the count contributed by each source workload. Queue acceptance and definite discard each update that list through one shared path. Resource owners retain control of allocation, reuse, and eviction policy.

An owner can retire its ID with a cleanup callback. The tracker releases its record and runs cleanup after all recordings and submissions finish; callbacks run outside its lock. This also lets descriptor pools survive their manager's destruction. Uncertain submissions keep their recordings until terminal queue/device teardown. Completed retired entries are removed during collection, so tracking state follows live demand. Device teardown drains deferred cleanup before destroying native memory allocators and the device.

The remaining counters describe different events:

| Value | Meaning |
| --- | --- |
| Queue submission serial | Accepted work on one queue; completion comes from its timeline semaphore or fence. Serials from different queues are not comparable. |
| Lifetime ID / bindless epoch | A unique tracker entry. Ordered bindless IDs divide earlier recordings from resources retired later; they do not imply GPU completion. |
| Uniform allocation and descriptor-slot generation | Identity/content validity after reuse. Descriptor-cache revision invalidates cached lookup results. |
| Frame, reuse count, and idle ticks | Eviction age; these do not establish safe GPU reuse. |
| Semaphore signal generation | Distinguishes an accepted signal from an earlier use of the same semaphore object. |
| Swapchain present serial | Identifies a presentation operation whose completion is established by present fences or image reacquisition. Queue submission completion alone does not prove presentation completion. |

## Global bindless heap

The Vulkan global bindless set occupies set 0. Its texture-2D, cube-texture, and sampler bindings follow `RHIBindlessHeapType`. Shaders using it bind set 0 even when they have no ordinary resource parameters. Ordinary descriptor sets may follow it, including unused set-number gaps.

`DynamicRHI::RegisterBindlessResource(resource, optionalSlot)` returns an `RHIBindlessHandle` containing the heap, slot index, and a registration generation. An invalid handle means the resource/slot is invalid, occupied by a different registration, or the heap is full. Automatic allocation searches free slots and reuses reclaimed holes. Re-registering the same resource in an active explicit slot returns the same handle without another reference or descriptor write. Registration through shader parameters remains supported; use explicit registration when a caller needs to retain a retirement handle.

Successful registration retains the texture/view/sampler. A view registration also retains its base texture. Texture registrations require sampled-image usage and resolve to their default native image view. Published slots cannot be overwritten. `IsBindlessResourceRegistered(handle)` validates an active registration; stale generations cannot retire or identify its replacement, even if the resource pointer or slot index is reused.

Before `UnregisterBindlessResource(handle)`, stop publishing that index to new commands and remove it from material/indirect data used by future work. A successful call retires the registration; it does not promise immediate release. The resource and slot remain protected until all earlier retained uses are completed or discarded. Repeated/stale retirement returns false. A retired slot cannot be reactivated by ordinary registration. Old recorded parameter commands can replay an identical registration under their captured usage interval without reactivating it.

The backend cannot discover arbitrary indices read from shader buffers. It therefore tracks conservative intervals of global-heap use. `RHICommandList` draws, dispatches (including indirect forms), and bindless parameter commands capture an interval during recording. Native bindless draws/dispatches retain an interval in their workload. Earlier command lists retain their protection until reset, rollback, or destruction, including if they remain replayable after GPU completion. The bindless manager owns retirement boundaries; their recording counts and per-queue serials live in the shared lifetime tracker. Successful submission transfers native recording counts to those serials, independently of replayable CPU commands. Native workload protection survives finalization, merging, submission rejection, and GPU execution on every queue. Definitively discarded work releases protection; uncertain failed submissions retain it until shutdown. Work recorded after retirement does not by itself extend the older retirement interval. Raw non-bindless resource lifetime requirements remain unchanged.

`CollectRetiredBindlessResources()` polls queue completion without waiting and releases eligible retired registrations. `BeginFrame()` calls it automatically. Call collection from the submission thread, serialized with queue operations. Registration and retirement use the manager lock and can reclaim already-completed intervals without polling queues. Pending writes for a reclaimed slot are removed before releasing their resources; recycled slots receive a new registration generation and a new descriptor write. Unregistered descriptors must not be accessed until replaced; the partially-bound heap does not require null descriptors or a fallback image.

Shader-visible indices remain ordinary integers. CPU generation validation does not instrument shaders or detect stale indices left in GPU data. Calling raw native Vulkan commands with the global heap requires capturing an interval with `RHICaptureBindlessEpoch()` before retirement can race with recording, recording it in the associated workload with `RecordLifetime(epoch)`, and releasing the capture with `RHIReleaseBindlessEpoch(epoch)` once recording finishes. Device shutdown still requires completed/discarded work and destroyed external command lists.

## Uniform buffers and push constants

`RHIBatchedShaderParameters::AddResourceParam(srd, buffer, nullptr, arrayIndex, bufferOffset)` accepts a per-element byte offset for uniform buffers. The offset defaults to zero. Each offset must meet Vulkan's uniform-buffer alignment and leave the reflected block range within the buffer. Fixed UBO arrays emit one dynamic offset for every descriptor, in set, binding, then array-element order, including unpopulated elements. These offsets are bind-time state and do not change descriptor-cache identity.

Packed value parameters describe one uniform block and require a single UBO descriptor. Bind UBO arrays through resource parameters. Explicit buffer bindings replace pending packed values for that binding.

`VulkanUniformBufferAllocator` manages packed-uniform blocks within each frame slot. Storage grows in 4 MiB blocks as needed; the initial reservation of eight block records does not limit growth. At slot reuse, trailing blocks beyond recent demand plus one spare are released after 120 consecutive reuses without needing them. A completely idle slot retains one existing block; unused slots allocate nothing. Another demand peak restarts the cooldown. The policy counts reuses of each slot, not elapsed time or total rendered frames, and adds no GPU waits. Metadata capacity may remain reserved, but trimmed native buffers and their allocations are released.

Each uniform block has an ID in the shared lifetime tracker. Native workloads record that ID; merging preserves each recording's count. A successful submission transfers those counts to queue serials, while a rejected submission retains them for retry or discard. Uncertain submissions remain protected until teardown. A block cannot be reset or trimmed until all pending recordings are gone and every referencing queue has completed its serial. Slot reuse counts separately measure the 120-reuse cooldown, so submission batching does not change the trimming delay.

Cached packed values retain CPU bytes and a block location/generation rather than buffer ownership. Reusing a slot invalidates its allocation generations; the next descriptor flush re-uploads stale values before resolving native descriptors. Trimmed block locations may be reused with a new generation. This lets idle cached bindings survive recycling without keeping excess buffers alive. Direct allocator results are borrowed views, valid until slot reuse; native users register their block through `RecordUniformBufferBlock`. The allocator keeps all growth, reuse, and trimming decisions, with no capacity-failure status propagated through draw/dispatch. Callers still synchronize frame-slot reuse, and allocator shutdown requires GPU work to be idle and recordings to be discarded. `GetAllocatedBlockCount(slotIndex)` exposes retained block counts for diagnostics.

Recorded push-constant updates forward the requested byte offset to the backend. Vulkan offsets and sizes must be multiples of four and fit the pipeline's declared push-constant range. RenderCore V2's separate restriction to offset zero remains in place.

## Vulkan device and presentation requirements

The backend requires Vulkan 1.2, `VK_KHR_swapchain`, `VK_KHR_dynamic_rendering` with its feature enabled, a queue supporting both graphics and compute, and the descriptor-indexing features used by its global heaps: runtime arrays, partially bound descriptors, variable counts, sampled-image update-after-bind, update-unused-while-pending, and sampled-image nonuniform indexing. Descriptor limits must accommodate the configured global heap capacities. Device selection rejects candidates missing this profile before logical-device or GPU-memory allocation. There is no automatic fallback to the legacy render-pass path.

Timeline semaphores and buffer device address are optional. Timeline-disabled devices use the existing fence path. Ray-tracing features are enabled only with their dependencies; VMA receives its buffer-device-address flag only when that feature was enabled. Vulkan 1.2 core features do not require their former extension names. Shader non-semantic info and debug-utils support are optional.

Presentation currently requires the selected graphics queue to support the actual window surface, a supported RGBA8/sRGB-nonlinear surface format, and transfer-destination image usage. Unsupported surfaces fail explicitly; separate presentation queues and a rendering-based alternative to the presentation copy are not implemented. VSync off prefers immediate, then mailbox, then FIFO; VSync on prefers mailbox, then FIFO. The swapchain follows the surface's fixed extent or clamps a variable extent, and viewport backbuffers use that actual size. Callers must refresh viewport-derived sizes and borrowed backbuffer pointers after resize/recreation.

Only successful or suboptimal acquisition publishes an image and semaphore. Timeout/not-ready skips presentation without recreation; out-of-date recreates the swapchain, and surface loss also replaces the surface. Suboptimal acquisition/presentation requests recreation after presentation. A rejected copy submission preserves the acquired image for a rebuilt submission; `Present` will not wait on a signal that was never submitted. Device loss blocks further submissions/presentation and is not recovered by resizing.

Presentation acceptance is stored on the image's render-complete semaphore. After native submission succeeds, the queue records its identity and submission serial on each signal semaphore and advances that semaphore's signal generation. Preparation snapshots the previous generation; presentation requires a new accepted graphics-queue signal. Workload merging already transfers signal entries to the submitted root. Context history, unrelated submissions, rejected work, and recycled workload objects cannot authorize the copy. Completion waits and context destruction preserve the acceptance state; returning a semaphore to its reusable pool clears its queue and serial while retaining its generation counter. The context's serial remains available for its existing lifetime waits.

Zero-sized resize suspends acquisition and preserves the last usable backbuffers until restoration. An initially zero-sized surface has no backbuffers until a usable resize; callers must skip rendering while unavailable. Recreation requires that callers have submitted or discarded recorded commands referencing the previous backbuffers. The viewport waits for device work before replacing them.

The swapchain owns acquire and render-complete semaphores sized to the complete driver-reported image count. Acquire fences prove acquisition signals have completed; accepted graphics submission serials prove their waits have completed. Recreation destroys these semaphores, including signaled acquisitions abandoned before submission, instead of returning uncertain binary state to the reusable pool.

`VK_KHR_swapchain_maintenance1` is enabled when its feature and instance dependencies are available, with `VK_EXT_swapchain_maintenance1` as the alternative. Per-image presentation fences prove completion before reusing a fence or destroying presentation semaphores and the old swapchain. Allocation failures and failed completion waits preserve valid cleanup ownership.

Without maintenance support, recreation carries old swapchains and pending presentation semaphores forward. A completed acquisition of a previously presented image on the replacement swapchain releases its predecessors, following the [Khronos recreation sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html). Counts return to a stable level after presentation progresses; continuous recreation before that proof can temporarily accumulate predecessors. A zero-extent surface preserves the old-swapchain link through restoration. Terminal teardown and surface loss still use the [Vulkan guide's unextended device-idle convention](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html), which lacks the stronger completion guarantee provided by presentation fences.


Dynamic rendering attachments

`RHIRenderingLayout` accepts either textures or explicit texture-owned views. Texture overloads select mip 0 and all array layers, reusing a cached single-mip attachment view when the default sampling view spans multiple mips. View overloads derive the attachment format and sample count from the selected view/image. `RHITextureViewCreateInfo.baseMipLevel`, `baseArrayLayer`, `mipLevels`, `arrayLayers`, and optional `aspect` describe the selection. Attachment views must select exactly one mip. Cube faces can be selected with an `e2D` view of a cube image.

```cpp
RHITextureViewCreateInfo viewInfo{};
viewInfo.format = texture->GetFormat();
viewInfo.type = RHITextureType::e2D;
viewInfo.baseMipLevel = 2;
viewInfo.baseArrayLayer = 3;
viewInfo.arrayLayers = 2;
RHITextureView* view = texture->CreateView(viewInfo); // borrowed from texture

RHIRenderingLayout layout{};
layout.SetRenderArea(0, 0, std::max(1u, texture->GetWidth() >> 2),
                            std::max(1u, texture->GetHeight() >> 2));
layout.AddColorRenderTarget(view, RHIRenderTargetLoadOp::eClear,
                          RHIRenderTargetStoreOp::eStore);
```

Adding attachments derives `numLayers` from the smallest selected array-layer count. A caller may lower it after adding attachments; it must remain nonzero and fit every view. The render area must fit each selected mip, including its offset; attachment extents need not be identical. All attachments must have the same sample count, and pipeline rasterization samples must match. Color/depth/stencil roles, image usage, declared formats, and image/view ownership are checked before recording rendering. Resetting attachment state returns the layout to zero attachments and one layer.

For a combined depth/stencil format, an empty view `aspect` retains the existing automatic behavior: depth sampling and both aspects for rendering. Set `aspect` explicitly to depth, stencil, or both to select rendering operations. Vulkan ignores an attachment view's native aspect mask, so the backend also selects the appropriate depth/stencil attachment pointers and pipeline formats. Pipeline cache keys include that aspect selection. An explicit both-aspect view is for rendering; sampled depth/stencil descriptors require a single aspect.

Callers still provide image transitions for the selected subresources and retain the owning texture until recorded/GPU work completes. Views and layouts do not add an image ownership reference. 3D-slice attachments are rejected. Explicit attachment views and layered rendering require dynamic rendering; legacy render-pass/framebuffer fixes remain deferred. RenderCore's existing RDG attachment and logical-view restrictions are unchanged; these new selection overloads are available to direct RHI callers.


## Vulkan command recording and pipeline compilation

`RHIGfxPipelineMultiSampleState::sampleMasks` is a 64-bit coverage mask: bit i enables sample i. It defaults to all samples enabled, including after changing `sampleCount`. Explicit zero or partial masks are preserved. Vulkan receives the low and high 32-bit words in sample order, and RenderCore pipeline keys include the full mask.

Each native command buffer caches its graphics and compute pipeline/descriptor bindings independently. Unchanged descriptor sets and dynamic offsets avoid another native bind, but descriptor resolution, cache-epoch checks, pending writes and workload pool retention still run before every draw/dispatch. Dynamic-offset scratch arrays retain their storage across draws. Vertex buffers and offsets also avoid repeated native binds when unchanged.

Beginning a command-buffer recording invalidates all cached native state, including when its Vulkan handle is reused. A graphics pipeline change invalidates cached dynamic values; the backend binds the pipeline first and then emits only the states it declares dynamic. Supported dynamic states remain viewport, scissor, line width and depth bias. Blend constants belong to `RHIGfxPipelineColorBlendState`; the current RHI has no dynamic blend-constant flag.

Code that records state-changing native Vulkan commands through `GetVkHandle()` must call `FVulkanCommandBuffer::InvalidateCachedState()` before resuming RHI draws/dispatches. This includes native pipeline, descriptor, vertex-buffer and dynamic-state changes, or executing secondary command buffers that invalidate the primary's state. RHI logical values remain authoritative on the next draw. State caches do not acquire ownership of referenced resources; the existing borrowed-resource lifetime contract still applies.

A device-owned `VkPipelineCache` retains native compilation data for graphics and compute pipeline creation until device destruction. It uses default internal synchronization and complements RenderCore's existing cache of complete pipeline objects. Cache creation failure falls back to `VK_NULL_HANDLE`. Cache data is not saved to disk; startup benefits and persistence/versioning policy require separate measurement. The legacy render-pass/framebuffer fixes remain deferred.
