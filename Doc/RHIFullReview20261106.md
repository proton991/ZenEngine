# RHI Full Review, 2026-11-06

Review date: 2026-11-06. Source revision: `eb8c0956`.

## Summary

Fix status: all 14 findings are addressed and verified in Debug and Release. No confirmed finding from this report remains open. A later review of the fixes found 10 further items; see [Follow-up review](#follow-up-review).

The review found 14 issues in the RHI and Vulkan backend, and one is serious. F1 reads freed resources during UI-only frames and can stop the renderer.

The original recommendation was to fix F1 first, followed by F2 and F3 because they can end the process or renderer. The original review was static. The user subsequently authorized fixes; the implementation and verification record below describes that follow-up separately.

The review covers `ZenCore/Include/Graphics/{RHI,VulkanRHI}` and `ZenCore/Source/Graphics/{RHI,VulkanRHI}`, about 28,000 lines, at commit `eb8c0956` on `hybrid-gi`.

| ID | Priority | Issue | Location |
| --- | --- | --- | --- |
| F1 | P1 | Descriptor state reads freed resources | `VulkanDescriptorSetState.cpp:224` |
| F2 | P2 | Packed uniform block over 4 MiB aborts in Release | `VulkanBuffer.cpp:339` |
| F3 | P2 | Surface loss during recreation stops the renderer | `VulkanSwapchain.cpp:301` |
| F4 | P3 | Bindless limit check counts samplers as stage resources | `VulkanDescriptorPool.cpp:1028` |
| F5 | P3 | Graphics pipeline validation skips optional features | `VulkanPipeline.cpp:789` |
| F6 | P3 | Push constants are not checked against the pipeline range | `VulkanCommandList.cpp:1953` |
| F7 | P3 | Specialization override type is ignored | `VulkanPipeline.cpp:428` |
| F8 | P3 | Descriptor allocation failures are reported as invalid arguments | `VulkanCommandList.cpp:1438` |
| F9 | P3 | Single-layer array views need padded textures | `VulkanTypes.cpp:193` |
| F10 | P3 | TLAS build validation is quadratic | `VulkanAccelerationStructure.cpp:262` |
| F11 | P3 | Dirty descriptor sets do their work twice | `VulkanDescriptorSetState.cpp:542` |
| F12 | P4 | Device property hooks are overwritten | `VulkanDevice.cpp:361` |
| F13 | P4 | Command argument copies duplicate `CopyCmdData` | `RHICommandList.cpp:357` |
| F14 | P4 | Unused RHI state | `VulkanCommandList.h:584` |

Priorities are my judgement. P1 is reachable in ordinary use. P2 ends the process or renderer under rare conditions. P3 needs a less capable device or an invalid request, or costs performance or clarity. P4 is cleanup.

## F1: Descriptor state reads freed resources (P1)

`VulkanDescriptorSetState` keeps raw resource pointers between frames and reads them after their owners have released them. On a frame submission, that stops the renderer.

**Cause.** `BindingState::srb.resources` holds raw `RHIResource*` values. Contexts stay with their pooled command lists, so this state survives from frame to frame. `SetPipeline` clears it only when the pipeline changes (line 224), although direct context calls only borrow resources for the call.

**Trigger.** `ui.frag` declares `uTextures[16]` and `uSamplers[16]`, and `UIRenderer` binds only the slots it uses. No other graphics pipeline may run on that context between the two UI passes, which holds in UI-only frames.

1. Frame N draws UI with 16 texture slots through `UIRenderSP`.
2. Frame N+1 binds 3 slots with the same pipeline, so elements 3 to 15 keep their old pointers.
3. One of those textures is released and destroyed on the RHI thread.
4. A later UI frame dirties the set, and `ValidateSetState` reads the freed pointer through `ResolveDescriptorBinding`.

**Effect.** The outcome depends on whether the freed memory has been reused.

- **Not reused:** `VulkanTextureView::Destroy` nulled the handle, so validation fails with "Invalid Vulkan descriptor binding resources". `SubmitFrame` batches are asynchronous, so the rejection reaches `PublishTerminalError` and rendering stops.
- **Reused:** `PagedAllocator` shares memory across resource types, so an unrelated resource can be read, or written into the descriptor, without being retained.

The same stale state weakens another check. A scalar binding left over from an earlier frame satisfies the `any_of` test in `ValidateSetState`, so a missing binding is no longer rejected as the README promises.

**Fix direction.** Clear per-recording bindings before `commands->Execute()` in `FinalizeCommandLists`, or make `SetShaderParameters` replace each binding it touches in full. Either way, first confirm that no pass relies on bindings carried over from an earlier list.

**Regression test.** Bind 16 elements, then 3 with the same pipeline, release a dropped resource and draw again. The draw must succeed without reading the released resource.

## Other correctness findings

These three need rarer conditions than F1. F2 and F3 still end the process or the renderer when they occur.

### F2: Packed uniform block over 4 MiB aborts in Release (P2)

Shader creation accepts a uniform block up to `maxUniformBufferRange` (`VulkanPipeline.cpp:193`), but the allocator's blocks are 4 MiB (`VulkanContext.cpp:906`). A packed block above 4 MiB therefore reaches `Alloc`, and `VERIFY_EXPR(size <= m_blockSize)` aborts because `VERIFY_EXPR` stays active in Release.

The guard on line 343, which would return the invalid allocation the README promises, can never run. This depends on the GPU, since a device with a 64 KiB limit rejects the shader first. I did not check whether any current shader has a block that large.

**Fix direction.** Drop that `VERIFY_EXPR` so the failure becomes a recording error, or reject such blocks at shader creation.

### F3: Surface loss during recreation stops the renderer (P2)

The README says surface loss replaces the surface, but that only happens when acquire or present reported it. `BeginResize` keeps the surface unless `GetLastResult()` already holds `VK_ERROR_SURFACE_LOST_KHR`.

Suppose acquire returns `VK_ERROR_OUT_OF_DATE_KHR`, and the surface is lost before the swapchain is rebuilt. `vkGetPhysicalDeviceSurfaceCapabilitiesKHR` or `vkCreateSwapchainKHR` then returns the loss, and `FinishResize` calls `BlockSubmissions`. Rendering stops even though the viewport could have recovered.

**Fix direction.** Treat surface loss from swapchain construction like loss from acquire: replace the surface and retry the recreation.

### F4: Bindless limit check counts samplers as stage resources (P3)

`BindlessHeapsFitLimits` adds separate samplers to the count it compares with `maxPerStageUpdateAfterBindResources`, a limit that excludes them. `DescriptorCounts::ResourceCount` documents that exclusion, so the two checks disagree.

With the default heaps of 2,112 image slots and 128 samplers, a device reporting a limit from 2,112 to 2,239 could hold them. Device selection rejects it anyway. A larger requested sampler heap widens that range.

**Fix direction.** Compare only `sampledImageCount` with that limit.

## Validation and error handling

Each of these lets a bad request reach Vulkan or hides why something failed. None was shown to fire in the shipped workloads. RenderCore's own checks catch part of F6, and only the wireframe case of F5 is gated.

### F5: Graphics pipeline validation skips optional features (P3)

`ValidateGraphicsPipeline` does not compare pipeline state with the features enabled on the device. It also accepts a shader with no vertex stage, or a compute shader, unlike the compute path's stage check.

These requests reach `vkCreateGraphicsPipelines` with invalid create info on a device without the feature:

- `wireframe`, `enableDepthClamp` or `enableDepthBoundsTest`
- `enableAlphaToOne`, `enableSampleShading` or `enableLogicOp`
- A nonzero `depthBiasClamp`, or `lineWidth` other than 1
- Attachments with different blend state, without `independentBlend`

Only `MeshPreviewRenderer` gates one of these, through `supportFillModeNonSolid`. `RHIDrawIndexedIndirect` has the matching gap: `drawCount` above 1 is not checked against `multiDrawIndirect`, and nothing reads `supportMultiDrawIndirect`. This is the same class as RHI20261105 F4.

**Fix direction.** Reject these requests in `ValidateGraphicsPipeline`, and require a vertex stage.

### F6: Push constants are not checked against the pipeline range (P3)

`RHISetPushConstants` passes offset and size straight to `vkCmdPushConstants`, although the README states both must fit the declared range. `RDGPassCmdEncoder::SetPushConstants` checks only `size % 4` and a zero offset.

A pass that pushes to a shader with no push-constant block sends a `stageFlags` of 0, and an oversized push overruns the range. `RHICommandList::SetPushConstants` also records uninitialized arena bytes if `pData` is null and the size is nonzero. None of these reaches the recording-error path.

**Fix direction.** Check range, alignment and stage flags in `RHISetPushConstants`, and latch a recording error on failure.

### F7: Specialization override type is ignored (P3)

`VulkanShader::Init` copies override bits without comparing the value's type with the reflected constant's type. Overriding a float constant with `{id, 1}` stores bits `0x00000001`, which the shader reads as about 1.4e-45 rather than 1.0.

The shader builds and renders with the wrong value and no diagnostic. Current shaders declare only int, uint and bool constants, so this stays latent until a float constant is overridden.

**Fix direction.** Reject a type mismatch before native creation.

### F8: Descriptor allocation failures are reported as invalid arguments (P3)

`PreDraw` and `PreDispatch` latch `{eInvalidArgument, 0, "Prepare shader descriptors"}` for every preparation failure. That includes a uniform block that cannot grow and a descriptor pool that fails with `VK_ERROR_OUT_OF_DEVICE_MEMORY`.

`RHIBatchResult::cause` then blames the caller, although the README says the original cause is kept. Terminal diagnostics point at shader bindings when the real problem is memory.

**Fix direction.** Return the failure from descriptor preparation, using `GetLastCreationError()` where it applies, and latch that.

## Structure and efficiency

These three do not break rendering today. The efficiency costs in F10 and F11 are structural and were not measured.

### F9: Single-layer array views need padded textures (P3)

`MakeVkImageViewCreateInfo` infers an array view from `layerCount > 1`, so a one-layer view is always `VK_IMAGE_VIEW_TYPE_2D`. A shader declaring `sampler2DArray` or `image2DArray` cannot use it.

Callers work around this by allocating an extra layer. `SceneShadowRenderer.cpp:226` forces at least two layers, and its comment gives this reason. `VoxelGIRenderer.cpp:395` allocates a two-layer 1x1 texture for the same purpose.

**Fix direction.** Let `RHITextureViewCreateInfo` request an array view explicitly, then remove the padding.

### F10: TLAS build validation is quadratic (P3)

`ValidateBuild` runs a linear `std::find` over the referenced BLASes for every instance. `SceneRayQuery` passes one reference per mesh, so each TLAS build costs instances × meshes pointer comparisons on the RHI thread.

Validation also adds a `dynamic_cast` per instance, and a `vkGetAccelerationStructureBuildSizesKHR` call per recorded build that the caller has already made.

**Fix direction.** Build a sorted list or set once when the TLAS is created, and reuse the caller's sizes where possible.

### F11: Dirty descriptor sets do their work twice (P3)

A dirty set resolves every binding twice: once in `ValidateSetState`, then again to produce writes on a cache miss. `AcquireSetHandle` also builds a `ContentKey` (line 488), and `ResolveSet` rebuilds the same key (line 542).

Each key allocates its binding order and key storage again, and `DescriptorWriteBatch` adds six vector allocations per miss. This sits on the per-draw path whenever parameters change.

**Fix direction.** Return the key from `AcquireSetHandle`, and resolve once into the write batch.

## Cleanup

These three described inactive or duplicated code in the reviewed baseline.

### F12: Device property hooks are overwritten (P4)

`VulkanDevice::Init` runs the `BeforePhysicalDeviceProperties` hooks, then assigns `physicalDeviceProperties2.pNext = &m_descriptorIndexingProperties` (line 361). Any chain a hook built is dropped, so `AfterPhysicalDeviceProperties` would read a zeroed struct.

No extension overrides either hook, which is why `m_accelerationStructureProperties` has its own query. Remove both hooks, or chain the indexing struct through `AddToPNext`.

### F13: Command argument copies duplicate `CopyCmdData` (P4)

`CopyTexture`, `BlitTexture`, `CopyTextureToBuffer`, `CopyBufferToTexture`, `BindVertexBuffers` and `AddTransitions` each hand-write the allocate, null-check and copy sequence. `RHICommandListBase::CopyCmdData` already does this, and only it enforces trivially copyable payloads.

### F14: Unused RHI state (P4)

These symbols imply behaviour that does not exist.

| Symbol | State |
| --- | --- |
| `VulkanComputeState::m_useAutomaticDescriptorSets` | No other references |
| `VK_WAIT_FENCE_TIME_NS` | No other references |
| `VulkanBuffer::GetOffset` | No callers |
| `FVulkanCommandBuffer::m_lastRenderingFlags` | Written, never read |
| `VulkanViewport::m_presentCount` | Written, never read |
| `numDestroyedContainers` in `TickPoolSetContainers` | Written, never read |
| `m_entryHighWaterMark`, `m_numSlotRetires` | Never reported |
| `RHICommandListBase::WaitUntilCompleted` | Not called; the context method is reached only from tests |
| `VulkanMemoryAllocator::MapBuffer`, `UnmapBuffer` | No production callers |

`BuildDescriptorPoolSizes` also special-cases `eUniformBuffer`, although `ShaderResourceTypeToVkDescriptorType` already returns the dynamic type.

## Limits of the original review

No finding has been reproduced at runtime. No build, test, validation run or capture was performed, and nothing was measured.

- **Coverage:** I read the RHI and Vulkan sources and their main headers, and followed callers into RenderCore and ZenUI where a finding depended on them. Some small headers were only skimmed, and the macOS platform files were not reviewed.
- **Verification:** the review ran without a separate verification pass. I checked the main findings against their callers, but no one else has reviewed them.
- **Hardware:** F2 to F5 depend on device limits or timing that were not exercised on any GPU.
- **Earlier review:** the eight findings in `Doc/RHIFullReview20261105.md` are not repeated here, and their fixes were not re-verified one by one.
- **Conventions:** no `CLAUDE.md` governs this code, so that check returned nothing. The two `std::queue` members in `VulkanSynchronization.h` do break your rule to prefer project containers.


## Fix record

Fix work began on checkout `36053f3e89bb4b91d29f8d17877245fcc2a10f75`. The repository had moved tests to the nested `ZenTest` repository since the reviewed revision. The original findings above are preserved as baseline evidence; the following records the resulting behavior.

| ID | Correction | Verification |
| --- | --- | --- |
| F1 | Reset cached native pipeline/descriptor state before executing each independent recorded command list, preserving raw-context continuity within a recording. | Recorded-list reuse and missing-binding regression; passed in Debug and Release. |
| F2 | Remove the fatal upper-size assertion. An oversized packed allocation returns invalid with an `eUnsupported` cause and does not grow the allocator; mapping failures retain their cause. | Allocation above 4 MiB followed by a valid allocation; passed in Debug and Release. |
| F3 | Surface loss during swapchain construction returns to the window thread for a new surface and one immediate retry. Repeated loss stays suspended/pending rather than looping or blocking the backend. | Capability-query/create loss injection, repeated loss, recovery/presentation; passed in Debug and Release. |
| F4 | Count sampled images alone against the per-stage resource budget; separate samplers still count against their sampler and pool budgets. | Exact-limit/one-over boundary; passed in Debug and Release. |
| F5 | Validate enabled graphics features, effective blending and shader stages before native creation. Indirect draw counts and dynamic wide-line/depth-bias-clamp requests are also gated. | Feature rejection matrix, compatible dynamic/default state, indirect/dynamic command checks; passed in Debug and Release. |
| F6 | Validate nonnull payload, nonzero aligned size/offset, nonzero stage flags and the reflected push-constant range. Null recorded data is preserved as invalid input instead of copying uninitialized arena bytes. | Misalignment, overflow, null, absent range, exact-end valid writes and recorded null payload; passed in Debug and Release. |
| F7 | Reject specialization overrides whose bool/int/float type differs from reflection before any native object creation. | Mismatch and valid override cases; passed in Debug and Release. |
| F8 | Descriptor preparation returns an explicit status. Draw/dispatch preserve allocation/native failure causes while invalid bindings remain argument errors. | Controlled pool/uniform allocation failure and cause assertions; passed in Debug and Release. |
| F9 | Add explicit array-view selection through RHI and RenderCore, validate native type/feature compatibility, and use texture-owned single-layer array views in shadow/GI callers. Remove padding and correct the shadow memory estimate. | Native sampled/storage array-view readback and shape checks; passed in Debug and Release. |
| F10 | Sort immutable retained BLAS references once and use binary lookup before dereferencing instance pointers. Retained references were validated at creation, so the per-instance backend cast is unnecessary. Cache TLAS size-query results by instance count/build flags. | Repeated validation queries once, changed count queries again, unretained instance rejection; passed in Debug and Release. |
| F11 | Build one descriptor content key per resolution and resolve resource payloads once, retaining cache-hit validation of offsets/usages. Consolidate write payload storage. | Existing descriptor/cache tests plus new invalid/cache-hit regressions; passed in Debug and Release. |
| F12 | Remove the unused before/after device-property hooks; required property queries remain explicit. | Build and capability tests; passed in Debug and Release. |
| F13 | Route texture/buffer copy regions, vertex arrays and transitions through `CopyCmdData`, copying borrowed inputs into owned command storage with the existing trivially-copyable constraint. | Existing copied-command tests and build; passed in Debug and Release. |
| F14 | Remove the listed unused fields, macro, getter, wrapper and mapping methods. Mapping test uses the existing persistent allocation pointer. Remove redundant UBO mapping and use project `Queue` for the two free-object queues noted in the conventions section. | Builds, existing allocation/synchronization tests, reference and formatting checks; passed in Debug and Release. |

Regression implementation is in [command validation tests](../ZenTest/CommonTest/VulkanCommandValidationIntegrationTests.cpp), [binding tests](../ZenTest/CommonTest/VulkanBindingIntegrationTests.cpp), [window/surface tests](../ZenTest/CommonTest/RHIWindowThreadingTests.cpp), [array-view tests](../ZenTest/CommonTest/VulkanAttachmentIntegrationTests.cpp), and [shader layout tests](../ZenTest/CommonTest/VulkanShaderLayoutIntegrationTests.cpp).

F10 is a structural reduction, not a measured frame-time improvement. The first validation still obtains native sizes; caller-supplied allocation sizes are not trusted as proof of build/scratch compatibility. Subsequent TLAS builds with the same count and flags reuse the result; changing either invalidates that cache. This follows the [native size-query contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetAccelerationStructureBuildSizesKHR.html).

## Post-fix verification

Debug and Release builds succeeded for `CommonTest`, `RenderCoreTest`, `VulkanRHITest`, `VulkanRHIIntegrationTest`, `RayQueryIntegrationTest`, `zen_editor`, `EditorModelTest`, `EditorRenderingTest`, `EditorPreviewTest`, `UIRenderingTest` and `RuntimeUIIntegrationTest`.

| Suite | Debug passed | Release passed | Skipped per configuration |
| --- | ---: | ---: | ---: |
| CommonTest | 50 | 50 | 0 |
| RenderCoreTest | 771 | 771 | 0 |
| VulkanRHITest | 74 | 74 | 0 |
| VulkanRHIIntegrationTest | 478 | 479 | 5 |
| RayQueryIntegrationTest | 92 | 92 | 4 |
| EditorModelTest | 56 | 56 | 0 |
| EditorRenderingTest | 79 | 79 | 0 |
| EditorPreviewTest | 18 | 18 | 0 |
| UIRenderingTest | 5 | 5 | 0 |
| RuntimeUIIntegrationTest | 10 | 10 | 0 |
| Total | 1633 | 1634 | 9 |

All selected suites passed with zero Vulkan VUID or synchronization-hazard errors. Counts exclude disabled tests; the nine skips per configuration remain explicitly accounted for above. The table uses the final clean runs after correcting test setup and view-count expectations exposed during validation. Native runs use synchronization validation and the existing `rtss_runner.py` executable alias, serially; no overlay or firewall settings were changed. Generated logs and JSON results are under ignored `build/rhi-fixes-20261106/`.

All 45 changed C++/shader files pass targeted `clang-format --dry-run --Werror`. The repository-wide format check finds a pre-existing spacing violation in untouched `Data/Shaders/VoxelGI/hybrid_trace.glsl:712`; that file was left unchanged. `tools/check_no_exceptions.py` passes across 544 owned C++ files. Root and nested `ZenTest` whitespace checks pass.

Verification is Windows x64 on NVIDIA GeForce RTX 5080. Feature/limit/failure cases use controlled injection; this does not claim physical device loss or multi-vendor coverage. F9's one-layer view behavior is tested through actual sampled/storage shader readback. F1's recorded-list regression releases a dropped resource after the first submission completes, reuses the list with fewer array entries, and reads back the result.

## Follow-up review

A review of the fixes found 10 further items, all lower severity than F1. Two were code defects worth fixing with this work:

- **Vertex buffers:** `ResetShaderBindings` now also clears the cached `VkBuffer` handles, the same kind of stale state as F1. A draw whose pipeline declares vertex inputs now fails with "Bind vertex buffers" unless its list bound them.
- **Dual-source blending:** validation required `dualSrcBlend`, but the device never enabled it, so every dual-source pipeline was rejected. The device now enables it where supported, and validation enforces `maxFragmentDualSrcAttachments`. Rejected independent and dual-source blend states now log the missing feature.

These items stay open:

| Item | Location | When it matters |
| --- | --- | --- |
| Surface retry has no backoff | `VulkanViewport::Resize` | A surface stays lost. Interactive apps retry every frame without stopping. |
| Views are not checked against the shader's array dimension | `RHIShaderResourceDescriptor` | A single-layer default view is bound to a `sampler2DArray`. The RHI reports nothing. |
| Oversized packed uniforms pass shader creation | `VulkanUniformBufferAllocator::Alloc` | A GPU's `maxUniformBufferRange` exceeds the packed block size. The failure appears at first draw. |
| Bindless heap accepts array-typed 2D views | `GetBindlessHeapType` | No current caller does this. |
| RDG views cannot request `arrayView` | `RDGResourceManager::MaterializeView` | No current pass needs this. |

After these two fixes, all listed suites passed on the same RTX 5080: 1636 in Debug and 1637 in Release, with 9 skips per configuration and zero Vulkan VUID or synchronization-hazard errors. The three new tests are `RecordedListCannotInheritVertexBuffers`, `DualSourceBlendingReadsTheSecondFragmentOutput` and `DualSourceBlendingIsEnabledWithinItsColorTargetLimit`.

No hardware acceleration-structure comparison has been run, so the frame-time effect of this work is unmeasured. `tools/benchmark_voxel_gi.py --backend hardware --workload dynamic` is the check to run.
