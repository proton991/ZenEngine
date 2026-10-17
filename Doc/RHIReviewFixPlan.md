# RHI review fix and optimization plan

Status: proposed, 2026-10-17. The findings come from a full static review of the active `Graphics/RHI` and `Graphics/VulkanRHI` code at revision `7b49daa0` (branch `hybrid-gi`). File references describe that revision. Nothing in this plan has been implemented yet. Record execution results in `Doc/RHIReviewFixVerification.md`, created by Phase 0.

The review found no confirmed defect on the render paths the applications use by default. It found four medium issues, seven lower-severity edge cases, seven API or design problems, README drift, and a set of nits. This plan fixes every one of them, adds the optimizations the review identified, and orders the work so that the reachable correctness issues land first. Every performance change is gated by a before/after measurement, as in the [improvement plan](RHIImprovementPlan.md). A change that does not help is reverted.

Scope: `Graphics/RHI`, `Graphics/VulkanRHI`, and their `RenderCore/V2` callers where an RHI contract needs the caller to change (RV2 and RV17). Tests live in `ZenSamples/CommonTest` and `ZenSamples/RenderCoreTest`. Async-compute policy, Synchronization2, and new backends are out of scope.

## 1. Relationship to other plans

| Plan | Relationship |
| --- | --- |
| [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md) (implemented) | This plan reuses its error model unchanged: `RHIError`, `RHIStatus`/`RHIResult<T>`, latched recording errors, rejected versus fatal submission, release-active `VERIFY_EXPR` for invariants, and no exceptions. Do not add new result types unless a step below names one. |
| [RHIImprovementPlan.md](RHIImprovementPlan.md) (implemented) | Preserve its invariants. Its rule that performance work lands only with a measurement applies to Phase 7 here. Its Phase 7 (descriptor cache) failed its gate; this plan does not reopen it. Its G1 explicit feature list is where RV10 adds optional features. |
| [RHIProductionTODO.md](RHIProductionTODO.md) | Hardware/vendor acceptance (NVIDIA, Intel) stays open. This plan does not close it. |
| [AsyncComputeImplementationPlan.md](AsyncComputeImplementationPlan.md) | Behavior unchanged. Run the affected suites with `--async-compute=0` and `1`. |

## 2. Findings addressed

IDs `RV1`–`RV18` match the numbering of the review report. `N` items are nits from the review. `X` items were found while writing this plan. `O` items are optimizations.

| ID | Severity | Problem | Evidence | Phase |
| --- | --- | --- | --- | --- |
| RV1 | Medium (latent) | Runtime-sized descriptor arrays outside set 0 are broken. The binding is sized to the device's update-after-bind maximum (often about 10^6) and every set is allocated at that count. Pools multiply it by 32–512 sets. Such a layout gets `UPDATE_AFTER_BIND_POOL` even when it holds a dynamic UBO, which Vulkan forbids. Update-after-bind is enabled only for sampled images. `AddResourceParam` sends any runtime-array parameter to the global heap, so the set never receives it. No shader uses this today. | [VulkanPipeline.cpp:390-400](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [:422](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [VulkanDescriptorSetState.cpp:485-487](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp), [VulkanDescriptorPool.cpp:72-78](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorPool.cpp), [VulkanExtension.cpp:131-141](../ZenCore/Source/Graphics/VulkanRHI/VulkanExtension.cpp), [RHIShaderParameters.h:139](../ZenCore/Include/Graphics/RHI/RHIShaderParameters.h) | 4 |
| RV2 | Medium | The present copy assumes the color backbuffer is in `COLOR_ATTACHMENT_OPTIMAL`. Its barriers are derived from layouts, and the helpers map GENERAL/UNDEFINED to top-of-pipe with no access and SHADER_READ_ONLY to fragment only. RenderCore never transitions the backbuffer before presentation. A presented graph whose last backbuffer access is not a color attachment declares the wrong old layout. Example: `RendererServer::DispatchRenderWorkloads` with an off-screen view or `drawScene=false` and no overlay, where `WorkspaceClear` is the last access. The default demo and editor configurations always finish with a color-attachment pass. | [VulkanViewport.cpp:353-369](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), [:459-469](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), [VulkanSynchronization.cpp:454-570](../ZenCore/Source/Graphics/VulkanRHI/VulkanSynchronization.cpp), [RendererServer.cpp:105-110](../ZenCore/Source/Graphics/RenderCore/V2/RendererServer.cpp), [RenderDevice.cpp:340-343](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) | 1 |
| RV3 | Medium | `SubmitRecordedWorkloads` calls `BlockSubmissions()` on any failure, including an out-of-memory `eRejected` that the queue reports as having changed nothing. It also blocks with a generic cause. Its only caller is swapchain/backbuffer creation, so an allocation failure during viewport creation permanently stops the backend instead of returning null. | [VulkanCommandList.cpp:984-1008](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [VulkanViewport.cpp:186-207](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp), [:300-313](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp) | 2 |
| RV4 | Medium | Texture creation calls Vulkan without validating the description. It never checks format/usage support, zero extents, type-specific shape (1D height, cube width == height and layers % 6), sample/mip rules, or device limits. These are invalid usage (undefined behavior), not error codes. `SetTexelFormat` does not check texel-buffer usage, buffer format features, range alignment, or `maxTexelBufferElements`. | [VulkanTexture.cpp:140-160](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp), [:255-359](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp), [VulkanBuffer.cpp:117-163](../ZenCore/Source/Graphics/VulkanRHI/VulkanBuffer.cpp) | 3 |
| RV5 | Low | `VulkanDebug` calls `vkSetDebugUtilsObjectNameEXT` without the extension check that `VulkanDevice::SetObjectName` has. Without `VK_EXT_debug_utils` this is a null function-pointer call. | [VulkanDebug.cpp:28](../ZenCore/Source/Graphics/VulkanRHI/VulkanDebug.cpp), [:47](../ZenCore/Source/Graphics/VulkanRHI/VulkanDebug.cpp), [VulkanDevice.cpp:615-631](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp) | 5 |
| RV6 | Low | The debug messenger formats `pMessageIdName`, which the spec allows to be NULL. `LOGE` pre-formats with `fmt::format` outside spdlog's guarded path, so a null `const char*` raises fmt's format error inside a driver callback. | [VulkanContext.cpp:349-359](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp), [Errors.h:18](../ZenCore/Include/Utils/Errors.h) | 5 |
| RV7 | Low (test path) | `RHIThread::Stop()` without a finalizer has a race. The worker sets `m_cleanupClosed`, then Stop clears it before clearing `m_threaded`. In the first window, cleanup is rejected, which aborts under strict checks. In the second, cleanup is queued with no worker left; `Invoke` would wait forever. | [RHIThread.cpp:156-194](../ZenCore/Source/Graphics/RHI/RHIThread.cpp), [:423-430](../ZenCore/Source/Graphics/RHI/RHIThread.cpp), [:318-362](../ZenCore/Source/Graphics/RHI/RHIThread.cpp) | 5 |
| RV8 | Low | A GPU timing result can stay `ePending` forever. `RHICommandGPUTiming` marks itself executed even when `RHIBeginGPUTiming` returned early because `EnsureRecording()` failed inside that command, so neither the command nor the command buffer publishes a status. | [RHICommandList.h:709-737](../ZenCore/Include/Graphics/RHI/RHICommandList.h), [VulkanCommandList.cpp:1696-1716](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp) | 2 |
| RV9 | Low | `ResolveSet` ignores the result of `AppendDescriptorBindingWrites`, after `Insert` has already published the set in the content cache. Per-type validity is implemented twice, once in `ValidateBindingState` and once in `AppendDescriptorBindingWrites`. Today the first is stricter, but any divergence would cache and reuse a half-written set. | [VulkanDescriptorSetState.cpp:77-276](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp), [:524-580](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp), [:766-862](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp) | 4 |
| RV10 | Low (latent) | Some exposed API depends on device features that are never enabled. Cube textures with more than 6 layers get `CUBE_ARRAY` views without `imageCubeArray`. `eMirrorClampToEdge` is exposed without `samplerMirrorClampToEdge`. `fullDrawIndexUint32` is not enabled, so index values above 2^24−1 are invalid. Sampler `maxAnisotropy` is not clamped to `maxSamplerAnisotropy`. | [VulkanTypes.cpp:198-201](../ZenCore/Source/Graphics/VulkanRHI/VulkanTypes.cpp), [VulkanDevice.cpp:530-565](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), [VulkanTexture.cpp:69-72](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp), [RenderScene.cpp:168-170](../ZenCore/Source/Graphics/RenderCore/V2/RenderScene.cpp) | 3 |
| RV11 | Low | Clear colors are always written as float32. Integer color targets and `ClearTexture` on UINT/SINT formats therefore receive reinterpreted bits; a clear to `1.0f` becomes `0x3F800000`. `RHIRenderTargetClearValue()` leaves its union uninitialized. `RHIClearTexture` accepts depth formats, which `vkCmdClearColorImage` forbids. | [VulkanCommandList.cpp:1608](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [:2011-2028](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [RHICommon.h:1042-1079](../ZenCore/Include/Graphics/RHI/RHICommon.h), [VulkanTypes.cpp:311-324](../ZenCore/Source/Graphics/VulkanRHI/VulkanTypes.cpp), [:337-348](../ZenCore/Source/Graphics/VulkanRHI/VulkanTypes.cpp) | 3 |
| RV12 | API | `RHIBufferAllocateType::eMax` has the same value as `eCPUWriteGPURead` (4). The default `eNone` reaches VMA as `VMA_MEMORY_USAGE_UNKNOWN` with no flags, which can choose any memory type. | [RHICommon.h:771-779](../ZenCore/Include/Graphics/RHI/RHICommon.h), [VulkanMemory.cpp:253-283](../ZenCore/Source/Graphics/VulkanRHI/VulkanMemory.cpp), [RHIResource.h:287-293](../ZenCore/Include/Graphics/RHI/RHIResource.h) | 3 |
| RV13 | API | `BitField<T>` converts implicitly to and from `int64_t`, so a field of one enum type silently accepts another (for example, a stage mask assigned to an access mask). `HasFlags(a, b)` means "any of", not "all of"; its only caller, `RHITexture::IsRenderTarget`, has no callers. `SetFlags` narrows to `uint32_t`. | [BitField.h](../ZenCore/Include/Templates/BitField.h), [RHIResource.h:421-425](../ZenCore/Include/Graphics/RHI/RHIResource.h) | 6 |
| RV14 | API, performance | Every pipeline layout gets the global bindless layout at set 0, and `HasGlobalBindlessSet()` is true for every shader. Every draw and dispatch therefore records a bindless epoch, flushes the heap and binds set 0. A shader that declares its own set-0 bindings silently gets a layout that does not match. | [VulkanPipeline.cpp:200-203](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [:347-367](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [VulkanDescriptorSetState.cpp:366-373](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp) | 4, 7 |
| RV15 | API | `GetSRDByName` is keyed by the block type name for buffers and the variable name for images. Duplicate names overwrite each other silently. The only caller looks up uniform blocks. | [RHIShaderUtil.h:233-240](../ZenCore/Include/Graphics/RHI/RHIShaderUtil.h), [VulkanPipeline.cpp:205-213](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp), [RDGPassCompiler.cpp:830](../ZenCore/Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp) | 4 |
| RV16 | Performance, design | Shader and pipeline creation run synchronously on the RHI worker through `InvokeChecked`. A pipeline-cache miss on the render thread waits behind every queued frame and then the compile. | [RHICommandListExecutor.cpp:1224-1248](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp) | 7 |
| RV17 | API | There are two ref-count conventions: `RHIResource` starts at 1, `RefCounted` at 0. `RHIResource`'s unused default constructor starts at 0, so its first release underflows. A double release wraps silently instead of failing. `RHIResourcePtr<T>(created)` adds a second reference and leaks unless `Adopt` is used. Pipeline create-info pointers have no initializers. | [RHIResource.h:35-75](../ZenCore/Include/Graphics/RHI/RHIResource.h), [:830-841](../ZenCore/Include/Graphics/RHI/RHIResource.h), [RefCountPtr.h](../ZenCore/Include/Utils/RefCountPtr.h) | 6 |
| RV18 | Docs | The README has drifted from the code. It says recorded rendering-layout pointers must outlive execution, but the command copies the layout. It says resource arguments are borrowed, without saying `RHICommandList` retains them. It lists variable descriptor counts as required, but the global heap does not use them. | [RHI/README.md](../ZenCore/Include/Graphics/RHI/README.md), [RHICommandList.h:670-680](../ZenCore/Include/Graphics/RHI/RHICommandList.h), [RHICommandList.cpp:476-495](../ZenCore/Source/Graphics/RHI/RHICommandList.cpp) | 8 |
| N1 | Nit | `RHIShaderStageToString` and `RHIShader::GetSRDCountByType` index arrays without a bounds check. | [RHICommon.h:188-194](../ZenCore/Include/Graphics/RHI/RHICommon.h), [RHIResource.h:628-631](../ZenCore/Include/Graphics/RHI/RHIResource.h) | 6 |
| N2 | Nit | `CalculateTextureMipLevels(0)` evaluates `log2(0)`, and converting the result to an integer is undefined behavior. | [RHIResource.h:427-441](../ZenCore/Include/Graphics/RHI/RHIResource.h) | 6 |
| N3 | Nit | `RHIBatchedShaderParameters::GetValueBytes` computes the end offset in `uint32_t`, which can wrap, while `RHIShaderParameterView::GetValueBytes` uses `uint64_t`. The two functions duplicate each other. | [RHIShaderParameters.h:79](../ZenCore/Include/Graphics/RHI/RHIShaderParameters.h), [:187](../ZenCore/Include/Graphics/RHI/RHIShaderParameters.h) | 6 |
| N4 | Nit | `VersatileResourceTemplate` stores objects in `uint8_t data[N]` with no `alignas`. It is correct today only because every type's size happens to be a multiple of 8. | [VulkanResourceAllocator.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanResourceAllocator.h) | 6 |
| N5 | Nit | `VkValidationFeaturesEXT` is chained into instance creation without enabling `VK_EXT_validation_features`. | [VulkanContext.cpp:530-580](../ZenCore/Source/Graphics/VulkanRHI/VulkanContext.cpp) | 6 |
| N6 | Nit | `VulkanQueue::ReleaseWorkload` shadows `pWorkload` in its merged-workload loop. | [VulkanQueue.cpp:198-201](../ZenCore/Source/Graphics/VulkanRHI/VulkanQueue.cpp) | 6 |
| N7 | Nit | `VulkanTextureView::Destroy` compares a Vulkan handle with `nullptr`. | [VulkanTexture.cpp:425-432](../ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp) | 6 |
| N8 | Nit | `RHIShaderUtil.h` defines `static` free functions in a header, which gives every translation unit its own copy. | [RHIShaderUtil.h:18-452](../ZenCore/Include/Graphics/RHI/RHIShaderUtil.h) | 6 |
| X1 | Low | Several sites call `BlockSubmissions()` with its generic default cause instead of the real error: dependency preparation, `FinalizeCommandLists`, `FinishResize`, `SubmitRecordedWorkloads`, and `LatchError` on device loss. The terminal error then reports "Vulkan submissions blocked" instead of, for example, out-of-device-memory. | [VulkanDevice.cpp:845-850](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), [VulkanCommandList.cpp:1002](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [:1051-1054](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [:2177-2182](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp), [VulkanViewport.cpp:304-313](../ZenCore/Source/Graphics/VulkanRHI/VulkanViewport.cpp) | 2 |
| X2 | Low | `RHIError::operation` and `source` default to `nullptr`. Three sites format them, and the null guards are duplicated inconsistently between two of them (`PublishTerminalError`, `CancelBatch`); `LatchError` has none. | [RHICommandListExecutor.cpp:330-337](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp), [:838](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp), [VulkanCommandList.cpp:1048](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp) | 5 |
| X3 | Low (latent) | When a frame's last group runs on async compute, the executor presents in a separate graphics submission with no dependency on that group. This is safe only because no compute pass can write the backbuffer today. The RV2 fix routes the final transition through the RDG, which already plans cross-queue dependencies. | [RHICommandListExecutor.cpp:875-912](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp) | 1 |
| O1 | Performance | Same as RV16: move pipeline compilation off the hitch path. | see RV16 | 7 |
| O2 | Performance | Same as RV14: skip bindless flushing, epoch recording and the set-0 bind for pipelines that do not use the heap. | see RV14 | 7 |
| O3 | Performance | `RHICopyTexture`, `RHIBlitTexture`, `RHICopyTextureToBuffer` and `RHICopyBufferToTexture` allocate a `HeapVector` on every call to convert regions. | [VulkanCommandList.cpp:2030-2105](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp) | 7 |
| O4 | Performance | `RHIThread::Enqueue` takes the task by value and `Push` copies it again into the queue: two copies of two `std::function` objects per job. | [RHIThread.cpp:278-362](../ZenCore/Source/Graphics/RHI/RHIThread.cpp) | 7 |
| O5 | Performance | `VulkanRHI::PrepareSubmissionDependencies` adds a timeline wait even when the producer serial is already known to be complete. The generic `DynamicRHI` version skips it. | [VulkanDevice.cpp:807-853](../ZenCore/Source/Graphics/VulkanRHI/VulkanDevice.cpp), [DynamicRHI.h:268-290](../ZenCore/Include/Graphics/RHI/DynamicRHI.h) | 7 |
| O6 | Cleanup | `VulkanFenceManager::ReleaseFence` does a linear search and erase, and has an unused `int size` (fence path only). | [VulkanSynchronization.cpp:80-106](../ZenCore/Source/Graphics/VulkanRHI/VulkanSynchronization.cpp) | 6 |

## 3. Rules and invariants

### 3.1 Global C++ rules (mandatory for every changed line, including tests)

The source of these rules is `~/.codex/AGENTS.md`, mirrored in [.agents/skills/review-rendering-changes/SKILL.md](../.agents/skills/review-rendering-changes/SKILL.md). Re-read it before starting each phase in case it has been extended.

1. **One return statement per function, at the end.** No early returns. Compute a result variable and gate later work on it (`bool valid = ...; if (valid) {...}`), as the existing RHI code does.
2. **No `auto`** except for iterator types. Spell out every type.
3. **Lambdas only when necessary, and short.** Use named functions for anything longer than a few lines, for example lifetime-tracker cleanup callbacks and validation.
4. **Project containers.** Use `HeapVector`, `SmallVector`, `FlatHashMap` and `HashMap`, never `std::vector`/`std::unordered_map`.
5. **Format with the repository `.clang-format`:** `python tools/format_active.py`, and `python tools/format_active.py --check` before review.
6. **Remove duplication.** Several steps below exist to delete a duplicated switch or helper (RV2, RV9, RV11, N3, X2). Do not add a second implementation alongside an old one.
7. **No exceptions.** Do not write `throw`, `try` or `catch`, and do not depend on a dependency catching for us. In particular, never pass a possibly-null C string to `LOGE`/`LOGW`; sanitize it first. Handle failures explicitly:
   - **Recoverable creation/validation failure:** log once with `LOGE`, release partial native state, and return `nullptr`, `false`, an invalid handle, or an `RHIStatus`/`RHIResult<T>` with an `RHIError`.
   - **Recording failure:** `LatchError(RHIError)` on the context. The dependent native command is not emitted, and `RHICommandListBase::Execute` skips the rest.
   - **Submission failure:** return `RHISubmissionResult::eRejected` only when nothing was accepted, and `eFatal` otherwise. Only `eFatal` (or device loss) calls `BlockSubmissions(cause)`, always with the real cause.
   - **Violated internal invariant:** `VERIFY_EXPR`/`VERIFY_EXPR_MSG`, which are active in Debug and Release and abort. `ASSERT` is acceptable only for debug-only diagnostics that are also handled at runtime.
   - **Gate:** `python tools/check_no_exceptions.py` (CTest `NoEngineExceptions`) must pass after every phase.

Example of the expected shape (a new validation helper used by RV4):

```cpp
static bool IsTextureShapeValid(const RHITextureCreateInfo& info)
{
    const bool extents = info.width > 0 && info.height > 0 && info.depth > 0 && info.arrayLayers > 0 && info.mipmaps > 0;

    bool shape         = false;

    switch (info.type)
    {
        case RHITextureType::e1D: shape = info.height == 1 && info.depth == 1; break;
        case RHITextureType::e2D: shape = info.depth == 1; break;
        case RHITextureType::e3D: shape = info.arrayLayers == 1 && info.samples == SampleCount::e1; break;
        case RHITextureType::eCube:
            shape = info.width == info.height && info.depth == 1 && info.arrayLayers % 6 == 0;
            break;
        default: break;
    }

    const bool mips = extents && info.mipmaps <= RHITexture::CalculateTextureMipLevels(info.width, info.height, info.depth);

    return extents && shape && mips;
}
```

### 3.2 Contracts to preserve

1. The [RHI README](../ZenCore/Include/Graphics/RHI/README.md) contracts, as amended by Phase 8.
2. CPU handoff, native acceptance, GPU completion and presentation retirement remain distinct events. No step may treat one as another.
3. Inline and threaded execution behave the same. Native objects are created, recorded and destroyed on the RHI thread (`CheckOwnership`). The RHI thread never waits for RenderCore. Windows waits keep servicing sent messages.
4. Rejected work leaves no native or bindless side effect. Fatal or uncertain work keeps its ownership until terminal teardown. R17 option B is unchanged: a rejected required frame stops rendering.
5. A change on a hot path lands only with a comparable measurement showing improvement or neutrality (Phase 7), and with no new validation or synchronization-validation messages.
6. Fix the cause in the layer that owns it. RenderCore owns resource state and policy. The RHI owns native mechanism and validates at its API boundary.

## 4. Implementation phases

Each phase leaves the tree building with its affected suites passing, and can be reviewed and committed on its own. The phases are ordered by risk: reachable correctness first, then failure handling, validation, shader rules, threading, hygiene, measured optimization, and documentation. Each phase updates the README sections its contract changes affect. Phase 8 is the final consistency sweep, not the first place docs are touched.

Common verification commands (MSVC developer shell, repository root):

```powershell
cmake --build build/x64-windows-msvc-debug --target VulkanRHITest VulkanRHIIntegrationTest RenderCoreTest CommonTest
.\bin\VulkanRHITest.exe
$env:VK_LAYER_VALIDATE_SYNC = '1'
.\bin\VulkanRHIIntegrationTest.exe
.\bin\RenderCoreTest.exe
.\bin\CommonTest.exe
ctest --test-dir build/x64-windows-msvc-debug -R NoEngineExceptions
python tools/format_active.py --check
```

Repeat the build and suites with `x64-windows-msvc-release`. Validation-enabled demo smoke runs use `scene_renderer_demo --smoke-test` with `--rhi-thread=0|1` and `--async-compute=0|1`.

### Phase 0 — Baseline

1. Record the passing test list for `VulkanRHITest`, `VulkanRHIIntegrationTest`, `RenderCoreTest`, `CommonTest` and `NoEngineExceptions` in Debug and Release, including skipped cases and why they were skipped.
2. Record validation output for a smoke run in each demo mode (`--mode=1|2|3`), with `--rhi-thread=0|1`, with and without `--no-ui`. Expect zero messages. Any existing message is a baseline entry, not a regression.
3. Capture the Phase 7 performance baselines now, before any code moves. Use the `x64-windows-msvc-release` preset with `--frames=600 --warmup=60 --fixed-step --vsync=0 --disable-validation --rhi-counters=1 --profile=build/profiles/rvfix-baseline-<mode>-t<thread>`, `--mode=2|3`, and `--rhi-thread=0|1`. Also record the `PipelineCacheMetrics` miss count and the CPU frame-time spikes over the first 300 frames, for O1.
4. Create `Doc/RHIReviewFixVerification.md` with the environment (GPU, driver, Vulkan SDK, presets) and these results.

Exit: baseline recorded.

### Phase 1 — Presentation state (RV2, X3)

Goal: the RHI's present copy starts from a state RenderCore guarantees, and its barriers state that state explicitly instead of deriving it from a layout.

1. **Contract.** A presented color backbuffer must end the frame graph in `RHITextureUsage::eColorAttachment` with `RHIAccessMode::eReadWrite`, its writes visible at `COLOR_ATTACHMENT_OUTPUT`. This must hold on the graphics queue for the whole subresource range. The RHI copy leaves it in the same state, which [RenderDevice.cpp:340-343](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) and [:399-403](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) already record. Write the contract on `RHIViewport::PrepareForPresent` in [RHIResource.h](../ZenCore/Include/Graphics/RHI/RHIResource.h) and in the README presentation section.
2. **RenderCore enforcement (owner of resource state).** The graph emits the backbuffer's final transition, so the RHI never guesses. Pick the mechanism in this order:
   1. Check whether `RDGResourceManager::QueueTextureExtraction(backbuffer, RHITextureUsage::eColorAttachment)` ([RDGResourceManager.cpp:590-620](../ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp)) already produces a graphics-queue transition into color attachment. It appends an `RDG.Extract` transfer pass, so confirm that pass accepts a color-attachment final usage and stays on a graphics-capable queue under `RHIQueueSupportsStages`. If it does, use it from the frame/standalone presentation submission paths (`RenderDevice::SubmitRecordedGraph` callers that pass a viewport). Do not add a second mechanism.
   2. Otherwise, add a required final state to imported textures, mirroring the existing `RDGTextureImportState` initial state. The compiler emits the transition after the last access only when that access differs. It places the transition in the last graphics group, so presentation can still join that group's submission. If the last access was on another queue, it plans the usual cross-queue dependency, which closes X3.
   3. Either way, the RDG compiler fails a presenting graph through its existing `Fail(...)` path if the backbuffer's final state is not color attachment. It is correct by construction after this step; the check guards later changes. It must not present with an unknown layout.
3. **RHI barriers.** In `VulkanViewport::CopyBackBufferToSwapchainImage` and `CreateSwapchain`, replace the layout-derived `VulkanPipelineBarrier` calls with explicit access and stage masks that match the contract. Each row below is one image barrier:

   | Barrier | Old layout → new layout | Source stage / access | Destination stage / access |
   | --- | --- | --- | --- |
   | Backbuffer before copy | `COLOR_ATTACHMENT_OPTIMAL` → `TRANSFER_SRC_OPTIMAL` | `COLOR_ATTACHMENT_OUTPUT` / `COLOR_ATTACHMENT_WRITE` | `TRANSFER` / `TRANSFER_READ` |
   | Swapchain image before copy | `UNDEFINED` → `TRANSFER_DST_OPTIMAL` | `TRANSFER` (chains with the acquire wait at `TRANSFER`) / 0 | `TRANSFER` / `TRANSFER_WRITE` |
   | Backbuffer after copy | `TRANSFER_SRC_OPTIMAL` → `COLOR_ATTACHMENT_OPTIMAL` | `TRANSFER` / 0 | `COLOR_ATTACHMENT_OUTPUT` / `COLOR_ATTACHMENT_READ \| COLOR_ATTACHMENT_WRITE` |
   | Swapchain image after copy | `TRANSFER_DST_OPTIMAL` → `PRESENT_SRC_KHR` | `TRANSFER` / `TRANSFER_WRITE` | `BOTTOM_OF_PIPE` / 0 |
   | Creation, color | `UNDEFINED` → `COLOR_ATTACHMENT_OPTIMAL` | `TOP_OF_PIPE` / 0 | `COLOR_ATTACHMENT_OUTPUT` / `COLOR_ATTACHMENT_READ \| WRITE` |
   | Creation, depth | `UNDEFINED` → `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` | `TOP_OF_PIPE` / 0 | `EARLY_FRAGMENT_TESTS \| LATE_FRAGMENT_TESTS` / `DEPTH_STENCIL_ATTACHMENT_READ \| WRITE` |

   Emit the two before-copy barriers in one `vkCmdPipelineBarrier` (source stages `COLOR_ATTACHMENT_OUTPUT | TRANSFER`), and likewise the two after-copy barriers. Use the six-argument `AddImageBarrier` and `Execute(cmd, src, dst)`.
4. **Delete the layout-derived helpers** once they have no callers (rule 6): the four-argument `VulkanPipelineBarrier::AddImageBarrier`, `ExecuteImageBarriersOnly`, `VkLayoutToPipelineStageFlags` and `VkLayoutToAccessFlags`. They encode stage/access masks that are wrong for GENERAL, UNDEFINED and SHADER_READ_ONLY sources.
5. **Delete the dead blit branch** in `CopyBackBufferToSwapchainImage`. `CreateSwapchain` always sizes the backbuffer to the swapchain extent, and the existing `VERIFY_EXPR` already forbids a blit into an sRGB image. Replace it with a release-active `VERIFY_EXPR_MSG` that the extents match. Keep the format-compatible `vkCmdCopyImage`.

Tests:

- RenderCore (fake backend): a graph whose only backbuffer access is a transfer clear, submitted with a viewport, records a final transition to `eColorAttachment` on the graphics queue before handoff. A graph that already ends in color attachment records no extra transition. A graph whose last backbuffer access is on async compute gets a dependency before the transition (`--async-compute=1` path).
- Integration, real device with validation and `VK_LAYER_VALIDATE_SYNC=1`: present a clear-only graph through a real viewport (extend [VulkanSwapchainIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanSwapchainIntegrationTests.cpp)). Expect no layout or synchronization messages, and read back the clear color from the next frame's backbuffer.
- Demo smoke: all modes, `--no-ui` and default, `--rhi-thread=0|1`, zero new validation messages.

Exit: tests above pass; the layout-derived helpers are deleted; README presentation section updated.

### Phase 2 — Submission failure classification and causes (RV3, RV8, X1)

1. **`SubmitRecordedWorkloads` reports; the caller decides policy.**
   - At entry, clear the backend submission error (`GVulkanRHI->SetSubmissionError({})`), as `FlushAllGPUCommands` does.
   - Add `VulkanQueue::HasPendingSubmissions() const`. Before enqueuing, `VERIFY_EXPR_MSG(!m_pQueue->HasPendingSubmissions(), ...)`: an immediate submission must not interleave with a platform-list flush, so a discard can only affect this context's work.
   - On `eRejected`: `DiscardPendingWorkloads(false)`, resolve the bindless transaction as not committed (unchanged), return `eRejected`. Do **not** block.
   - On `eFatal`: `DiscardPendingWorkloads(true)` and `BlockSubmissions(GVulkanRHI->GetSubmissionFailureCause())`. `GetSubmissionFailureCause()` is the ternary that `FlushAllGPUCommands` already uses ([VulkanCommandList.cpp:2305-2307](../ZenCore/Source/Graphics/VulkanRHI/VulkanCommandList.cpp)), moved into one `VulkanRHI` member: the last submission error when it is a failure, otherwise a generic `eBackendFailure` "Vulkan submission" error. A fatal result that comes from `AreSubmissionsBlocked()` sets no submission error, so the fallback is needed. Both sites call it (rule 6).

   ```cpp
   if (result == RHISubmissionResult::eFatal)
   {
       m_pQueue->DiscardPendingWorkloads(true);

       GVulkanRHI->BlockSubmissions(GVulkanRHI->GetSubmissionFailureCause());
   }
   else if (result == RHISubmissionResult::eRejected)
   {
       m_pQueue->DiscardPendingWorkloads(false);
   }
   else
   {
       m_pQueue->ProcessPendingWorkloads(0);
   }
   ```

2. **Viewport policy.**
   - Initial `CreateViewport`: a rejected backbuffer initialization makes `CreateSwapchain` return false and the viewport factory return null. The caller's initialization failure path handles it; the backend stays usable.
   - `FinishResize`: keep recreation failure terminal, consistent with R17 option B, because a viewport without backbuffers cannot satisfy required frames. Store the actual cause on the viewport (`RHIError m_recreationError`, set by `CreateSwapchain` from the swapchain status, texture creation or submission error), and pass it to `BlockSubmissions(error)` instead of the default.
   - Decision recorded here: suspend-and-retry after an allocation failure during resize is a possible later extension. It is not part of this plan, because it needs a RenderCore retry policy.
3. **Real causes everywhere (X1).** Replace every argument-less `BlockSubmissions()` with the specific error:
   - `VulkanRHI::PrepareSubmissionDependencies`: `eInvalidArgument` for an unsubmitted or invalid serial, or the wait's native error.
   - `FinalizeCommandLists`: the dependency-preparation failure cause.
   - `FinishResize`: from step 2.
   - `LatchError` on device loss: pass the latched error.
   Then remove the default argument from `BlockSubmissions` so a new call site cannot omit the cause.
4. **GPU timing cannot stay pending (RV8).** In `FVulkanCommandListContext::RHIBeginGPUTiming`, if `EnsureRecording()` fails while the result is still `ePending`, publish `RHIGPUTimingStatus::eDiscarded`. This mirrors the base `IRHICommandContext` default, which publishes `eUnsupported`. `RHIEndGPUTiming` needs no change: a begun scope is published by `ResolveGPUTimings` or `DiscardGPUTimings`, and a begin that failed was published here.

Tests (extend [VulkanProductionFailureTests.cpp](../ZenSamples/CommonTest/VulkanProductionFailureTests.cpp) with `ScopedVulkanCall`):

- `vkQueueSubmit` returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` during `CreateViewport`: the result is null, `AreSubmissionsBlocked()` is false, and a following ordinary batch submits and completes. Run in both timeline and fence modes.
- The same injection during `Resize`: the backend is blocked, and `GetTerminalError().code == eOutOfDeviceMemory` with `operation == "vkQueueSubmit"`.
- `vkQueueSubmit` returns `VK_ERROR_DEVICE_LOST` during `CreateViewport`: the backend is blocked with `eDeviceLost`.
- `vkBeginCommandBuffer` fails for the first native buffer after `BeginGPUTiming` was recorded: after the batch is discarded, the result status is `eDiscarded`, never `ePending` (extend [VulkanGPUTimingIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanGPUTimingIntegrationTests.cpp)).
- Grep gate: no call to `BlockSubmissions()` without an argument remains.

Exit: tests pass in Debug and Release; README "Recoverable creation and recording failures" updated to say immediate backbuffer submission rejects without blocking.

### Phase 3 — Creation-time validation and device features (RV4, RV10, RV11, RV12)

1. **Texture descriptions (RV4).** Split `VulkanTexture::Init` into a pure `BuildImageCreateInfo(const RHITextureCreateInfo&)` and allocation. Validate in `VulkanTexture::CreateObject` *before* allocating the wrapper, and return null with one `LOGE` naming the tag and the failed rule. Checks:
   - `format != eUndefined` and usage not empty.
   - `IsTextureShapeValid` (section 3.1), using the fixed `CalculateTextureMipLevels` (N2).
   - `samples > 1` requires `e2D` and `mipmaps == 1`. `eCube` is the only type that sets `CUBE_COMPATIBLE`, so requiring `e2D` also excludes cube-compatible multisampled images.
   - Cube textures with more than 6 layers require `imageCubeArray` (RV10).
   - `vkGetPhysicalDeviceImageFormatProperties(gpu, format, imageType, OPTIMAL, usage, flags, &properties)` returns `VK_SUCCESS`. Extent, mip count, layer count and the sample bit are within the returned limits. This one call covers format/usage support, including storage on sRGB formats and attachment support.

   Keep this query uncached. Textures are created rarely and RDG pools reuse them. Add a cache only if Phase 0 counters show it on a hot path.
2. **Texture views.** Extend `CreateViewOnRHIThread` with the same cube-array rule for `eCube` views with `layerCount > 6`.
3. **Texel buffer views (RV4).** In `SetTexelFormatOnRHIThread`, before `vkCreateBufferView`, all of these must hold, or it returns false after one `LOGE`:
   - The buffer has `eTextureBuffer` or `eImageBuffer` usage.
   - `vkGetPhysicalDeviceFormatProperties(format).bufferFeatures` contains `UNIFORM_TEXEL_BUFFER_BIT` for `eTextureBuffer` and `STORAGE_TEXEL_BUFFER_BIT` for `eImageBuffer`.
   - `GetTextureFormatPixelSize(format) != 0` and `GetRequiredSize() % texelSize == 0`.
   - `GetRequiredSize() / texelSize <= limits.maxTexelBufferElements`.
4. **Samplers (RV10).**
   - With anisotropy enabled, clamp `maxAnisotropy` to `[1, limits.maxSamplerAnisotropy]`.
   - Reject `eMirrorClampToEdge` unless `VK_KHR_sampler_mirror_clamp_to_edge` is enabled. Add it as an optional simple device extension; enabling it satisfies the sampler VUID under Vulkan 1.2, and chaining `VkPhysicalDeviceVulkan12Features` is not allowed beside the individual feature structs.
   - Reject `unnormalizedUVW` unless its constraints hold: equal min/mag filters, nearest mip mode, `minLod == maxLod == 0`, clamp-to-edge or clamp-to-border address modes, no anisotropy and no compare.
   - Return null on rejection.
5. **Optional core features (RV10).** Through the explicit optional list in `VulkanDevice::SetupDevice` (improvement plan G1), enable `imageCubeArray` and `fullDrawIndexUint32` when supported. Publish `supportImageCubeArray`, `supportSamplerMirrorClampToEdge` and `maxDrawIndexedIndexValue` in `RHIGPUInfo`. Keep `GetUnsupportedReason` unchanged: these features stay optional.
6. **Clear values (RV11).**
   - Make `RHIRenderTargetClearValue()` zero-initialize its color.
   - Add `FormatIsUnsignedInteger` and `FormatIsSignedInteger` beside the existing `FormatIs*` helpers in [Format.h](../ZenCore/Include/Graphics/Common/Format.h).
   - Replace both `ToVkClearColor` overloads with one format-aware `VkClearColorValue ToVkClearColor(const Color& color, DataFormat format)`. Integer formats receive the component values converted numerically: clamped to the representable range, then `static_cast`, which avoids undefined float-to-integer conversion. Use it for attachment clears (the attachment format) and `RHIClearTexture` (the texture format).
   - `RHIClearTexture` latches `eInvalidArgument` for depth/stencil formats and emits nothing.
7. **Buffer allocation type (RV12).**
   - Set `eMax = 5` and keep the existing values.
   - Audit every `RHIBufferCreateInfo` construction site (`grep -rn "RHIBufferCreateInfo "`): 15 in ZenCore, ZenEditor and ZenUI, which all appear to set `allocateType`, and 37 in ZenSamples tests. Confirm each one sets it before the rejection below lands, so no test starts failing for an unrelated reason.
   - `VulkanBuffer::CreateObject` then rejects `eNone` and out-of-range values with `LOGE`.
   - Extract the VMA mapping from `AllocBuffer` into a pure `MakeVmaBufferAllocation(RHIBufferAllocateType, bool budget)` so CPU tests can cover it.

Tests:

- `VulkanRHITest` (CPU-only) covers the pure functions: `IsTextureShapeValid` and the sample/mip rules, `MakeVmaBufferAllocation` for every type, the integer clear conversion, and the clamped anisotropy.
- `VulkanRHIIntegrationTest` ([VulkanResourceCreationTests.cpp](../ZenSamples/CommonTest/VulkanResourceCreationTests.cpp) and the fixture's validation counter):
  - Each invalid description returns null with zero validation messages: storage on sRGB, a cube with width ≠ height, a 1D texture with height > 1, an MSAA texture with mips, a texel view on a buffer without texel usage, a misaligned texel range, `eNone` allocation.
  - The existing valid creation cases are unchanged.
- [VulkanAttachmentIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanAttachmentIntegrationTests.cpp): clear an `R32_UINT` color attachment and an `R32_UINT` texture to `Color(7, 0, 0, 0)` and read back `7`.
- [VulkanCapabilityIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanCapabilityIntegrationTests.cpp): the reported optional features match what was enabled. A cube-array texture is created when supported and rejected when the feature is reported off.
- Full RenderCore and demo smoke runs show no creation regressions: every scene texture and render target still creates.

Exit: tests pass; README "Allocation, features and scalar contracts" and "Recoverable creation" sections updated.

### Phase 4 — Shader layout rules and descriptor resolution (RV1, RV14 validation, RV15, RV9)

1. **One shader-layout validation pass.** Add `ValidateShaderLayout(const RHIShaderGroupInfo&)` to `VulkanShader::Init`, run before any native object is created. Shader creation returns null with a message naming the stage, set, binding and rule. It enforces:
   - **RV1:** no descriptor with `bindless == true` outside `kGlobalBindlessHeapIndex`. Runtime-sized descriptor arrays are supported only as the global heap.
   - **RV14:** every set-0 descriptor is one of the three heap bindings, with matching binding index and type (`eTexture` bindless at `ZEN_BINDLESS_HEAP_BINDING_TEXTURE2D`/`TEXTURECUBE`, `eSampler` bindless at `ZEN_BINDLESS_HEAP_BINDING_SAMPLER`).
   - **RV15:** names inserted into `m_namedSRDLut` are unique per shader. Skip empty names. A duplicate name with a different set or binding rejects the shader. Make `GetSRDByName` const, with a single `find`. Document the naming rule on the declaration: uniform and storage blocks use the block type name, other descriptors use the variable name.
2. **Corpus gate before step 3.** Add a test that reflects every shader in `Data/SpvShaders` and runs `ValidateShaderLayout`. If any shipped shader fails, stop and fix that shader or revisit the rule. Do not weaken the rule silently.
3. **Delete the dead variable-count/update-after-bind path (RV1, rule 6).** After step 1, ordinary sets can never have a variable count. Remove:
   - The `srd.bindless` branch and `VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT` in `InitNativeObjects`, `DescriptorSetInfo::variableCount`, `VulkanShader::GetDescriptorSetVariableCount`, and the `descriptor->bindless ? ... :` branches in `ValidateSetState` and the dynamic-offset slots.
   - The `updateAfterBind`/`variableCount` parameters of `VulkanDescriptorSetCache::Insert`, `VulkanDescriptorPoolSetContainer::Allocate` and `VulkanDescriptorPool`, `m_UABChainMap`, the update-after-bind clamp in `BuildDescriptorPoolSizes`, and `VulkanDevice::GetDescriptorSetUpdateAfterBindLimit` (no other callers).

   The global heap keeps its own update-after-bind pool and layout in `VulkanBindlessDescriptorPoolManager`.
4. **Usage flag (prepares O2).** In `InitNativeObjects`, set the existing `m_hasGlobalBindlessSet` to `!m_SRDTable[kGlobalBindlessHeapIndex].empty()` instead of `true` ([VulkanPipeline.cpp:364](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp)). The resize at :200-203 only appends empty sets, so this is true exactly when the shader declares heap bindings. Do not add a second flag. Every pipeline layout keeps the global layout at set 0, which preserves layout compatibility and fills set-number gaps. `BuildDescriptorSetList` already skips an unused set 0, so a pipeline that does not use the heap stops binding it. `RecordCurrentBindlessEpoch` and the heap `Flush()` in `FlushPendingDescriptorWrites` are already conditioned on `HasGlobalBindlessSet()` ([VulkanDescriptorSetState.cpp:366-373](../ZenCore/Source/Graphics/VulkanRHI/VulkanDescriptorSetState.cpp)), so this step also stops that per-draw work. Add `VERIFY_EXPR(setIdx != kGlobalBindlessHeapIndex || pShader->HasGlobalBindlessSet())` to the content-cache branch of `AcquireSetHandle`: set 0 uses the global update-after-bind layout and must never be allocated from an ordinary pool. Phase 7 (O2) measures the effect. If the measurement is neutral, keep this step anyway, because it is required for the set-0 validation to be meaningful.
5. **One per-element descriptor resolver (RV9, rule 6).** Merge the per-type switches of `ValidateBindingState` and `AppendDescriptorBindingWrites` into one function. It resolves each populated element into native descriptor info, either `VkDescriptorImageInfo`, `VkDescriptorBufferInfo`, `VkBufferView` or the acceleration-structure write, and returns whether every element is valid. It applies the stricter of today's two checks per type: a TLAS with a native handle; UBO offset alignment and the range within the buffer; a texel view present.
   - `ValidateSetState` keeps the layout-level checks: the binding exists, the type matches, the element count is within the array size, and required single bindings are present. It calls the resolver without writing.
   - Split `VulkanDescriptorSetCache::Insert` into `Allocate(...)`, which allocates from ring slot 0 (rotating as today) without publishing an entry, and `Publish(key, set, container)`, which is today's entry bookkeeping. `ResolveSet` allocates on a miss, resolves and writes, and publishes only if every write is valid. On failure it returns false: the set is not cached, `BuildDescriptorSetList` fails, and `PreDraw`/`PreDispatch` latch `eInvalidArgument` as today. An unpublished set stays in its pool until that container is reset.

Tests:

- `VulkanRHIIntegrationTest`: a shader with `layout(set=1, binding=0) uniform texture2D t[];` returns null with zero validation messages. So do a shader with a UBO at set 0 and a shader with two uniform blocks of the same type name at different bindings. Add these shaders under `ZenSamples/CommonTest/Shaders`.
- The corpus test from step 2.
- [VulkanDescriptorIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanDescriptorIntegrationTests.cpp) and [VulkanDescriptorLifetimeTests.cpp](../ZenSamples/CommonTest/VulkanDescriptorLifetimeTests.cpp), including the 2,049-dispatch suite, pass unchanged. Add negative cases, each failing at preparation with no `vkUpdateDescriptorSets` call for the rejected set: a texture where a sampler is expected, a null element in a single binding, a misaligned UBO offset, a BLAS where a TLAS is expected. A later valid draw with the same content key must still allocate and write a fresh set.
- [VulkanBindingIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanBindingIntegrationTests.cpp): a compute pipeline that does not use the heap issues no set-0 bind (observe `vkCmdBindDescriptorSets` through the fixture). Heap users are unchanged.

Exit: tests pass; README "Global bindless heap" states the set-0 and runtime-array rules.

### Phase 5 — Threading and diagnostics (RV5, RV6, RV7, X2)

1. **Debug names (RV5).** Implement `VulkanDebug::SetPipelineDebugName`/`SetTextureDebugName` by calling `GVulkanRHI->GetDevice()->SetObjectName(type, handle, name)`, which already checks `hasDebugUtils` and the function pointer. Skip null resources, using one result variable and no early return. Remove `CHECK_VK_ERROR` from this file.
2. **Null strings in diagnostics (RV6, X2).**
   - Add `inline const char* RHIErrorText(const char* text, const char* fallback)` to [RHIError.h](../ZenCore/Include/Graphics/RHI/RHIError.h). It returns the fallback for null.
   - Use it in `DebugUtilsMessengerCallback` for `pMessageIdName` and `pMessage`, and in `PublishTerminalError`, `CancelBatch` and `LatchError` for `operation`/`source`. This replaces their duplicated inline ternaries.
   - Do not add a `try`/`catch` around logging.
   - Audit the remaining `LOGE`/`LOGW` calls in `Graphics/RHI` and `Graphics/VulkanRHI` that pass a `const char*` field which can be null (`grep -n "LOG[EW](.*\.operation\|\.source"`), and route them through the helper.
3. **`RHIThread::Stop` without a finalizer (RV7).** Make the worker's exit and the admission change a single transition under `m_mutex`:
   - Add `bool m_workerRunning`, guarded by `m_mutex`. `Start` sets it before creating the thread.
   - In `Run`'s final branch, under the lock: `m_cleanupClosed = static_cast<bool>(m_finalizer)` (admission stays open without a finalizer), and `m_workerRunning = false`.
   - In `Push`, under the lock: when the task would be admitted (`task.cleanup ? !m_cleanupClosed : !m_stopping`) but `!m_workerRunning`, queue nothing and set a new `bool& outRunInline` parameter. `Enqueue` then executes the task on the caller, exactly as inline mode does. Do not add a value to the public `RHIJobAdmission` enum for this; it is an internal handoff between `Push` and `Enqueue`. A finalizer stop is unchanged: admission is closed and the task is rejected as before. Ordinary tasks are still rejected once `m_stopping` is set.
   - `Stop` no longer rewrites `m_cleanupClosed` after `join`; it only clears `m_threaded`.

   ```cpp
   if (finished)
   {
       m_cleanupClosed = static_cast<bool>(m_finalizer);

       m_workerRunning = false;

       task            = {std::move(m_finalizer), {}, true};
   }
   ```

   This removes the abort and the stranded task. It does not lift the existing rule that producers stop before `Stop`, so document that unchanged rule in `RHIThread.h`.

Tests:

- [RHIThreadingTests.inl](../ZenSamples/RenderCoreTest/RHIThreadingTests.inl): 1,000 iterations of `Start(eThreaded)`, then a second thread issuing `DispatchCleanup(++executed)` and `Invoke(no-op)` in a loop while the main thread calls `Stop()` without a finalizer, then joining the second thread. Under strict teardown checks: no abort, `Invoke` never hangs (bounded by a test timeout), and `executed` equals the number of accepted dispatches. A finalizer stop still rejects admissions after the finalizer starts (existing tests).
- `VulkanRHITest`: `DebugUtilsMessengerCallback` with null `pMessageIdName` and `pMessage` logs without aborting. `RHIErrorText(nullptr, "x")` returns `"x"`.
- `VulkanDebug` with debug utils reported unavailable is a no-op (unit test through `SetObjectName`'s flag check).

Exit: tests pass under `--rhi-thread=0|1`.

### Phase 6 — API hygiene (RV13, RV17, N1–N8, O6)

1. **`BitField` type safety (RV13).**
   - Add deleted cross-enum conversions:

     ```cpp
     template <typename U>
         requires(!std::is_same_v<U, T>)
     BitField(const BitField<U>&) = delete;

     template <typename U>
         requires(!std::is_same_v<U, T>)
     BitField& operator=(const BitField<U>&) = delete;
     ```

   - Keep the implicit `operator int64_t()`, which the Vulkan interop relies on.
   - Fix every compile error this exposes by inspecting it: a real mix-up gets fixed, an intentional reinterpretation gets an explicit `int64_t` cast with a comment.
   - Rename `HasFlags` to `HasAnyFlag` and fix `SetFlags` to accumulate in `int64_t`.
   - Delete `RHITexture::IsRenderTarget`, which has no callers.
   - Add `static_assert(!std::is_assignable_v<BitField<RHIAccessFlagBits>&, BitField<RHIPipelineStageFlagBits>>)` and the matching `is_constructible` check to `VulkanRHITest`.
2. **Ref counting (RV17).**
   - Delete `RHIResource() = default` (unused).
   - In `ReleaseReference`, verify the counter was nonzero before the decrement (`VERIFY_EXPR_MSG`, release-active), so a double release aborts instead of wrapping.
   - Document on `RHIResource` and `RHIResourcePtr`: factories return one owning reference; adopt it with `RHIResourcePtr<T>::Adopt(ptr)`; the pointer constructor adds a reference.
   - Audit the nine `RHIResourcePtr<...>` sites in RenderCore, ZenUI and ZenEditor (listed by `grep -rn "RHIResourcePtr<"`) for adopt-versus-add-reference intent, and fix any leak found.
   - Give `RHIGfxPipelineCreateInfo::pShader`, `pRenderingLayout` and `RHIComputePipelineCreateInfo::pShader` `{nullptr}` initializers.
   - Default `RHIGfxPipelineColorBlendState::Attachment::colorWriteMask` to RGBA only after auditing the `AddAttachment(Attachment)` callers; skip it if any caller relies on the zero mask.
   - Leave the depth/stencil defaults as they are, documented on the struct.
3. **Nits.**
   - **N1:** bounds-check `RHIShaderStageToString` (returns `"Unknown"`) and `GetSRDCountByType` (returns 0).
   - **N2:** the three `CalculateTextureMipLevels` overloads share one `constexpr` implementation, `std::bit_width(std::max(1u, maxDim))`, with no floating point. The two- and one-argument forms forward to it.
   - **N3:** `RHIBatchedShaderParameters::GetValueBytes` returns `GetView().GetValueBytes(param)`, which removes the duplicate and the 32-bit end offset.
   - **N4:** declare `alignas(RESOURCE_TYPES...) uint8_t data[...]`, and `static_assert` that `MAX_RESOURCE_SIZE` is a multiple of the strictest alignment.
   - **N5:** add `VK_EXT_validation_features` as an optional entry in `VulkanInstanceExtension::GetEnabledInstanceExtensions`, with a `hasValidationFeatures` instance flag. `SetupInstanceLayers` already flags every extension the validation layer reports, but only for entries in that list, and today there is none for this extension. Chain `VkValidationFeaturesEXT` only when Debug Printf is requested and the extension is enabled; otherwise chain the debug messenger directly, as the non-validation path does.
   - **N6:** rename the inner loop variable in `ReleaseWorkload`.
   - **N7:** compare with `VK_NULL_HANDLE`.
   - **N8:** change `static` functions in `RHIShaderUtil.h` to `inline`.
   - **O6:** `ReleaseFence` swap-and-pop, and delete the unused local.

Tests: every suite builds and passes; the `static_assert`s compile; `format_active.py --check` is clean; `NoEngineExceptions` passes.

Exit: hygiene changes merged with no behavior change other than the documented RV17 and N items.

### Phase 7 — Measured optimizations (RV16/O1, RV14/O2, O3, O4, O5)

Measure each item against the Phase 0 baseline with the same preset, device, scene, mode and thread setting. Keep it only if the gating metric improves or is neutral, with no new validation messages. Record every result, including the reverted ones, in the verification document.

1. **O2, heap work only where used.** Phase 4 step 4 is the change: pipelines that do not use the heap stop binding set 0, recording a bindless epoch and flushing the heap. No heap slot is reachable from them. This step only measures it. Metric: RHI execution CPU time per frame and the bindless capture counter (`--rhi-counters=1`), modes 2 and 3, `--rhi-thread=1`. `RHIExecutionCounters` has no descriptor-bind counter; add one (relaxed atomic, like the others) only if the time difference needs it to be explained. The `RHICommandList` epoch capture on the recording thread is already lock-free in steady state and is not changed.
2. **O5, completed dependencies.** In `VulkanRHI::PrepareSubmissionDependencies`, skip adding the timeline wait when `producer->GetLastCompletedSerial() >= serial`. This uses the cached atomic and does not poll. Metric: wait-semaphore count per frame with `--async-compute=1`, and GPU frame time. This is a correctness-neutral change; a completed serial needs no GPU wait.
3. **O3, copy-region conversion.** Use `SmallVector<VkImageCopy, 4>`, `SmallVector<VkImageBlit, 4>` and `SmallVector<VkBufferImageCopy, 4>` instead of the per-call `HeapVector`. Metric: CPU time of scene load (texture uploads and mip generation) and steady-state RHI execution time.
4. **O4, RHI thread queue copies.** `Push(Task& task, ...)` moves the task into `m_tasks` only on acceptance. On rejection the caller keeps it, so `Enqueue` can still run `task.cancelled`. Keep the single final return in both functions. Metric: `Invoke` and `Dispatch` throughput in a microbenchmark (non-gating test), plus `--rhi-thread=1` CPU frame time.
5. **O1, pipeline compilation (RV16).** Gate: the Phase 0 measurement shows pipeline-cache misses causing CPU frame-time spikes after the first frame, for example on GI-mode or settings switches. If not, record the result and stop here. If so:
   1. First, warm known pipeline keys during scene/renderer initialization through the existing RenderCore pipeline cache, which moves compilation to load time without new threading. Re-measure.
   2. Only if spikes remain, design asynchronous compilation as a separate follow-up plan. A worker creates `VkPipeline` objects (the device pipeline cache is internally synchronized); the RHI thread publishes the `VulkanPipeline` wrapper; RenderCore's pipeline cache gains an explicit pending state, and RenderCore chooses whether to skip or substitute a pass while a pipeline is pending. That plan must state how it amends the "native objects are created on the RHI thread" invariant, and return results as `RHIResult<RHIPipeline*>` with no exceptions.

Exit: each kept optimization has a recorded before/after; each rejected one is reverted with its numbers recorded.

### Phase 8 — Documentation sweep (RV18)

Apply what was not already updated in earlier phases, then reread the whole [RHI README](../ZenCore/Include/Graphics/RHI/README.md) against the code:

1. **"Resources and recorded commands":**
   - Direct `IRHICommandContext` calls borrow resources and layouts for the duration of the call.
   - `RHICommandList` retains every resource it records, including a view's base texture, until reset, rollback or destruction.
   - Recorded `BeginRendering` copies the layout, so the caller's layout need not outlive recording.
   - GPU-safe reuse still follows the retirement contract.
2. **"Vulkan device and presentation requirements":**
   - The presentation-state contract from Phase 1.
   - Remove `descriptorBindingVariableDescriptorCount` from the required features: no binding uses it after Phase 4. Drop it from `GetUnsupportedReason`, `EvaluatePhysicalDeviceCandidate` and `VulkanDescriptorIndexingExtension` in the same change, and re-run device selection on the test machines.
3. **"Global bindless heap":** the set-0 rule, the runtime-array rule, and that pipelines not using the heap do not bind set 0.
4. **"Recoverable creation and recording failures":** texture, texel-view and sampler description validation; immediate submission rejects without blocking; `BlockSubmissions` always carries the cause.
5. **"Allocation, features and scalar contracts":** `eNone` is rejected; the optional features reported in `RHIGPUInfo`; integer clear values.
6. **"RHI worker shutdown":** `Stop` without a finalizer keeps admission open and runs late cleanup inline.

Exit: README matches the code; the verification document lists every RV/N/X/O item with its commit and test evidence.

## 5. Acceptance

The plan is complete when all of the following hold:

1. Every RV, N and X item is fixed, or explicitly closed with a recorded reason. O items are kept or reverted according to their measurements.
2. `VulkanRHITest`, `VulkanRHIIntegrationTest` (with `VK_LAYER_VALIDATE_SYNC=1`), `RenderCoreTest`, `CommonTest` and `NoEngineExceptions` pass in Debug and Release. New tests are included, and skipped cases are justified.
3. Validation-enabled smoke runs of every demo mode, with `--rhi-thread=0|1`, `--async-compute=0|1`, and with and without `--no-ui`, report no new validation or synchronization messages compared with Phase 0. The same holds for the editor rendering tests.
4. `python tools/format_active.py --check` is clean. A grep over changed files finds no `throw`, `try`, `catch` or `auto` outside iterator declarations, and no `std::vector`.
5. The README and `Doc/RHIReviewFixVerification.md` are updated.

## 6. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| Phase 1 adds a transition to frames that already end in color attachment, costing GPU time. | The compiler emits it only when the last access differs. The fake-backend test asserts no extra transition for normal frames. |
| Phase 1's extraction approach places the transition on a non-graphics queue. | The step 2 decision rule rejects it in that case, and the queue-placement test catches it. |
| Phase 3 validation rejects a texture the renderer currently creates successfully (driver tolerance of invalid usage). | The demo and editor smoke runs list every rejected creation. Fix the caller's description rather than loosening a rule that reflects a Vulkan requirement. |
| Phase 4's set-0/runtime-array rules reject a shipped shader. | The corpus gate (step 2) runs before any deletion. |
| `BitField` deleted conversions expose many call sites. | Treat each compile error as a finding. Fix it or cast it explicitly with a reason; never silence it by restoring the conversion. |
| Removing the variable-count requirement changes which GPUs are selected. | Only additional devices become eligible. Re-run device selection and the capability tests on the AMD baseline machine, and note the change in the verification document. |
| O1 grows into a threading redesign. | It is gated, and its asynchronous part is explicitly a separate follow-up plan. |

## 7. Out of scope

- NVIDIA/Intel hardware acceptance and other open items in [RHIProductionTODO.md](RHIProductionTODO.md).
- Suspend-and-retry viewport recreation after an allocation failure (Phase 2, step 2 records the decision).
- Real runtime-sized descriptor arrays outside the global heap. If they are ever needed, design them separately: a reflection-declared upper bound, a set without dynamic UBOs, per-type update-after-bind features, and the variable count on the last binding only.
- Synchronization2 and a presentation redesign, still deferred as in the [improvement plan](RHIImprovementPlan.md).
