# RHI binding and lifetime contracts

## Resources and recorded commands

Raw RHI resource arguments are borrowed. Callers must keep buffers, textures, samplers, pipelines, and their dependencies alive from command recording until every GPU use completes, or until unsubmitted work is definitively discarded. `Destroy*`/`ReleaseReference` releases a reference immediately and does not wait for GPU work. A submitted command list reset does not imply GPU completion. RenderCore's deferred retirement provides this protection for its managed resources; direct RHI callers must provide it themselves.

`RHICommandList::Create(context)` transfers ownership of the context to the command list. Destroying that list destroys its context. Rendering-layout pointers passed to recorded `BeginRendering` commands must remain valid and unchanged until command execution. Parameter bytes and copied region arrays are owned by recorded commands; resource pointers remain borrowed.

`RHITexture::CreateView`, `RHITexture::GetDefaultView`, and `CreateTextureView` return borrowed views owned by the base texture. Do not release the texture-owned reference. If a caller takes an additional view reference, it must also keep the base texture alive. Keeping only the view alive does not retain the image. The base texture releases its owned views before destroying the image.

Descriptor-pool retention is independent of resource retention. Vulkan workloads retain ordinary descriptor pools from recording through completion, but this does not retain the buffers, textures, or samplers described by their sets.

`RHIBuffer::SetTexelFormat(format)` creates one buffer-owned texel view over the buffer's declared byte size, excluding allocation padding. The buffer must have the appropriate texel-buffer usage, and its size and format must meet the device's texel-view requirements. Repeating the same format after successful creation is a no-op; changing it throws and preserves the existing view, including recorded descriptors that reference it. Native creation failure throws without publishing a view or fixing its format, so the caller may retry. The buffer destroys its view when released under the ordinary resource lifetime contract.

## Vulkan queue sharing

RHI-created buffers and textures permit use on both graphics and async-compute queue families. Resources with transfer-source or transfer-destination usage also permit the transfer family. Creation uses concurrent sharing across distinct families, or exclusive sharing when every permitted queue belongs to the same family. Texture views inherit the base image's sharing. This policy does not apply to native swapchain images or externally created Vulkan resources.

Sharing removes the need for queue-family ownership transfers; it does not order accesses. Dependent work on different queues still requires synchronization, and image layouts and access/stage masks must match the operations and queue capabilities. Vulkan contexts provide `AddSignalSemaphore` and `AddWaitSemaphore` for GPU dependencies; submit the producer successfully before submitting a binary-semaphore consumer, and retain the semaphore and resources through completion. The portable RHI currently has submission-completion waits but no GPU cross-queue dependency API or automatic async-compute scheduling. Concurrent sharing can have a device-dependent performance cost; there is currently no per-resource queue selection in the RHI.

## Global bindless heap

The Vulkan global bindless set occupies set 0. Its texture-2D, cube-texture, and sampler bindings follow `RHIBindlessHeapType`. Shaders using it bind set 0 even when they have no ordinary resource parameters. Ordinary descriptor sets may follow it, including unused set-number gaps.

`DynamicRHI::RegisterBindlessResource(resource, optionalSlot)` returns an `RHIBindlessHandle` containing the heap, slot index, and a registration generation. An invalid handle means the resource/slot is invalid, occupied by a different registration, or the heap is full. Automatic allocation searches free slots and reuses reclaimed holes. Re-registering the same resource and resource generation in an active explicit slot returns the same handle without another reference or descriptor write. Registration through shader parameters remains supported; use explicit registration when a caller needs to retain a retirement handle.

Successful registration retains the texture/view/sampler. A view registration also retains its base texture. Texture registrations require sampled-image usage and resolve to their default native image view. Published slots cannot be overwritten. `IsBindlessResourceRegistered(handle)` validates an active registration; stale generations cannot retire or identify its replacement, even if the resource pointer or slot index is reused.

Before `UnregisterBindlessResource(handle)`, stop publishing that index to new commands and remove it from material/indirect data used by future work. A successful call retires the registration; it does not promise immediate release. The resource and slot remain protected until all earlier retained uses are completed or discarded. Repeated/stale retirement returns false. A retired slot cannot be reactivated by ordinary registration. Old recorded parameter commands can replay an identical registration under their captured usage interval without reactivating it.

The backend cannot discover arbitrary indices read from shader buffers. It therefore tracks conservative intervals of global-heap use. `RHICommandList` draws, dispatches (including indirect forms), and bindless parameter commands capture an interval during recording. Native bindless draws/dispatches retain an interval in their workload. Earlier command lists retain their protection until reset, rollback, or destruction, including if they remain replayable after GPU completion. Native workload protection survives finalization, merging, submission rejection, and GPU execution on every queue. Definitively discarded work releases protection; uncertain failed submissions retain it until shutdown. Work recorded after retirement does not by itself extend the older retirement interval. Raw non-bindless resource lifetime requirements remain unchanged.

`CollectRetiredBindlessResources()` polls queue completion without waiting and releases eligible retired registrations. `BeginFrame()` calls it automatically. Call collection from the submission thread, serialized with queue operations. Registration and retirement use the manager lock and can reclaim already-completed intervals without polling queues. Pending writes for a reclaimed slot are removed before releasing their resources; recycled slots receive a new registration generation and a new descriptor write. Unregistered descriptors must not be accessed until replaced; the partially-bound heap does not require null descriptors or a fallback image.

Shader-visible indices remain ordinary integers. CPU generation validation does not instrument shaders or detect stale indices left in GPU data. Calling raw native Vulkan commands with the global heap requires capturing an interval with `RHICaptureBindlessUse()` before retirement can race with recording, and retaining it through the associated workload with `RetainBindlessUse()`. Device shutdown still requires completed/discarded work and destroyed external command lists.

## Uniform buffers and push constants

`RHIBatchedShaderParameters::AddResourceParam(srd, buffer, nullptr, arrayIndex, bufferOffset)` accepts a per-element byte offset for uniform buffers. The offset defaults to zero. Each offset must meet Vulkan's uniform-buffer alignment and leave the reflected block range within the buffer. Fixed UBO arrays emit one dynamic offset for every descriptor, in set, binding, then array-element order, including unpopulated elements. These offsets are bind-time state and do not change descriptor-cache identity.

Packed value parameters describe one uniform block and require a single UBO descriptor. Bind UBO arrays through resource parameters. Explicit buffer bindings replace pending packed values for that binding.

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

Each native command buffer caches its graphics and compute pipeline/descriptor bindings independently. Unchanged descriptor sets and dynamic offsets avoid another native bind, but descriptor resolution, cache-epoch checks, pending writes and workload pool retention still run before every draw/dispatch. Dynamic-offset scratch arrays retain their storage across draws. Vertex buffers and offsets also avoid repeated native binds when unchanged.

Beginning a command-buffer recording invalidates all cached native state, including when its Vulkan handle is reused. A graphics pipeline change invalidates cached dynamic values; the backend binds the pipeline first and then emits only the states it declares dynamic. Supported dynamic states remain viewport, scissor, line width and depth bias. Blend constants belong to `RHIGfxPipelineColorBlendState`; the current RHI has no dynamic blend-constant flag.

Code that records state-changing native Vulkan commands through `GetVkHandle()` must call `FVulkanCommandBuffer::InvalidateCachedState()` before resuming RHI draws/dispatches. This includes native pipeline, descriptor, vertex-buffer and dynamic-state changes, or executing secondary command buffers that invalidate the primary's state. RHI logical values remain authoritative on the next draw. State caches do not acquire ownership of referenced resources; the existing borrowed-resource lifetime contract still applies.

A device-owned `VkPipelineCache` retains native compilation data for graphics and compute pipeline creation until device destruction. It uses default internal synchronization and complements RenderCore's existing cache of complete pipeline objects. Cache creation failure falls back to `VK_NULL_HANDLE`. Cache data is not saved to disk; startup benefits and persistence/versioning policy require separate measurement. The legacy render-pass/framebuffer fixes remain deferred.
