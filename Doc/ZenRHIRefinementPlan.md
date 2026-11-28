# ZenRHI refinement plan

Proposed on 2026-11-28 for ZenRHI `a83b6c0` in ZenEngine `7886a8bc`. Line references point at those revisions.

**Goal: make ZenRHI smaller, more cohesive and less coupled without changing rendering behaviour.** It should serve ZenEngine and outside developers through the same API. This plan covers code structure only. Production hardening stays in [ZenRHIEnhancementPlan.md](ZenRHIEnhancementPlan.md), and nothing here changes its 1.0 exit criteria.

## 1. Where the code stands

| | ZenRHI | NVRHI (Vulkan, common, headers) |
| --- | --- | --- |
| Library lines, excluding VMA | 31,655 (include 6,990, Core 3,890, Vulkan 20,726) | 17,494 |
| Test lines | 25,348 in 466 tests, plus `FakeRHI.h` at 1,849 | — |

ZenRHI is about 1.7 times NVRHI's size and has fewer features. NVRHI leaves instance, device and swapchain creation to the application; Donut's device manager adds about 1.5k lines for that. ZenRHI's extra code sits in four places:

| Block | ZenRHI | NVRHI counterpart |
| --- | --- | --- |
| Deferred recording: `RHICommandList`, executor, `RHIThread` | 4.8k | none |
| Descriptors and binding | 3.6k | 1.1k |
| Queue, synchronization, lifetime | 1.95k | 0.4k |
| Instance, adapter, device, extensions | 3.6k | Donut, 1.25k |

Five problems account for most of it.

1. **Globals do the wiring.**
   - The backend reaches the device through `GVulkanRHI` 263 times, and the memory allocator through `GVkMemAllocator`.
   - The library reads the `RHIOptions` singleton 34 times, and the engine writes it in about 17 places.
   - Three places assign `GDynamicRHI`: the factory, `RenderDevice`, and the executor constructor.
   - `GetRHIThread()` and `GRHIFrameState` are process-wide.
   - `~RHIResource` calls `GDynamicRHI->NotifyResourceDestroyed` ([RHIResource.cpp:13](../ZenRHI/src/Core/RHIResource.cpp#L13)).
2. **The executor pretends to be a backend.**
   - `RHICommandListExecutor` derives from `DynamicRHI` and overrides 54 of its 58 virtuals. Four overrides are dead: `Init`, `FinalizeCommandLists`, `SubmitPlatformCommandLists` and `PrepareSubmissionDependencies`.
   - Trivial calls wait for a worker round trip. Every `Destroy*` is just `ReleaseReference()`, and `ReleaseWhenComplete` blocks too ([RHICommandListExecutor.cpp:394](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L394)).
   - The facade is incomplete, so the engine still calls `GetRHIThread().Invoke` itself five times.
3. **Layers that only forward.**
   - **Creation.** It goes `DynamicRHI::CreateBuffer` → `VulkanRHI::CreateBuffer` → `GVulkanRHI->GetResourceFactory()->CreateBuffer` → `VulkanResourceFactory::CreateBuffer` (which adds only a blocked check) → `VulkanBuffer::CreateObject`.
   - **Startup.** `CreateRHI` → `CreateVulkanRHI` → `CreateVulkanRHIChecked` → `InitializeBackend` → `VulkanRHI::InitChecked` → `VulkanDevice::InitChecked` → `InitializeSupportedDevice` → `SetupDevice`. The first three only forward.
   - **Recording.** One `vkAllocateDescriptorSets` passes through six levels, and one draw walks `GetWorkload` three times.
4. **One job, several mechanisms.**
   - **Lifetime.** Seven mechanisms overlap, and the engine keeps its own retirement on top:
     - command-list references
     - workload references
     - the executor's deferred list
     - the lifetime tracker
     - bindless transactions
     - queue serials plus fences
     - a timed command-buffer free
   - **Submission.** There is a timeline path and a fence path, plus a second viewport path (`SubmitRecordedWorkloads`).
   - **Capabilities.** They are queried and interpreted five times, with two definitions of device-local memory.
   - **Validation.** Backend-neutral rules live in Vulkan and run again on every descriptor-cache hit.
   - **API.** Seven operations come in checked and unchecked pairs. There are four health queries and five progress readers.
5. **The public surface leaks in both directions.**
   - **Backend internals in public headers:**
     - about 25 `RHICommand*` structs and the `ALLOC_CMD` macro
     - `RHIWorkerTimer` and `RHIExecutionCounterStorage`
     - `kInlineDescriptorSetCount`
     - a test friend
   - **Engine types in the RHI:** `TextureFormat`, `RHIGeometryBuffer` and `RHIShaderCreateInfo::spirvFileName`.
   - **Engine knowledge in the library:**
     - Comments and the README name RenderCore, RenderDevice, RenderScene and `HybridTemporalSP`.
     - Vulkan's application and engine names are hard-coded as "ZenEngineRHI" and "ZenEngine".
   - **The engine reaches into ZenRHI internals:**
     - RDG builds draws with `RHICommandDrawIndexed::Param` ([RDGPassCompiler.cpp:764](../ZenCore/Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp#L764)).
     - Three engine tests include private Vulkan headers, swap volk function pointers and reset globals.
     - `VulkanViewport.h` declares an engine test as a friend.

**What stays.** Keep these four:
- the binding model: reflected parameter names plus the global bindless heap
- explicit transitions computed by the RDG
- the typed errors (`RHIStatus`, `RHIResult<T>`)
- the ordered RHI worker

For an engine with a render graph, each one matches or beats NVRHI and UE5. The [backend comparison](RHIBackendComparison.md) records the reference revisions.

## 2. Rules for this work

- **Behaviour first.**
  - Every phase preserves rendering output and the [README contracts](../ZenRHI/include/ZenRHI/README.md): a published queue serial means Vulkan accepted the submission, and reuse waits for retirement.
  - Each phase lands green on its own.
- **Delete before restructuring.** Phase 1 removes dead code so later phases move less of it.
- **Break the API now, once.**
  - The policy job rejects removals only from 1.0 onward ([check_public_api.py:216](../ZenRHI/tools/check_public_api.py#L216)).
  - Batch the breaking changes into 0.3.0 and update callers in the same ZenEngine commit. All Zen repositories bump to v0.3.0 together.
- **Fix callers, don't add shims.** The one exception is a temporary engine-side accessor while the globals are removed in Phase 4.
- **Tests follow behaviour, not internals.**
  - When a phase deletes a mechanism, delete or merge its tests.
  - Rewrite tests that pinned an internal so they use the public API.
  - The large consolidation waits until Phase 6, so tests are not rewritten twice.
- **No unrequested performance runs while GI work is open.** Phases 5c to 5e state their expected CPU effect, and the owner decides whether to measure.
- **Code style.** The [C++ preferences](../AGENTS.md#c-preferences) apply to every new line. Use clang-format and LF line endings.

## 3. Decisions for the owner

**Status: on 2026-11-28 the owner accepted every recommendation below and started Phase 1.**

| # | Decision | Recommendation | Reason |
| --- | --- | --- | --- |
| D1 | Require timeline semaphores and delete the fence submission path | Yes | See below. |
| D2 | Lift the [5.9 deferral](RHIProductionTODO.md) and remove the globals | Yes | Low coupling and outside users are the concrete need the backlog asked for. Isolated tests and several devices per process follow from it. |
| D3 | Hide the deferred command structs and `IRHICommandContext`, but keep the deferred layer | Yes | See below. |
| D4 | Delete the macOS platform code and create surfaces only through `VulkanSurfaceProvider` | Yes | See below. |
| D5 | Public features the engine does not use: explicit bindless registration, adapter enumeration, checked creation, GPU frame timing | Keep the first three. Keep frame timing only if the engine adopts it for frame GPU time; otherwise delete it (about 1.1k test lines). | Outside users need the first three. |
| D6 | `SetDebugPrintfEnabled` and `SetRobustBufferAccessEnabled` have no caller in ZenEngine | Keep both (revised during Phase 1) | Both work end to end for any host that calls the public setters. The original premise, that neither feature could turn on, was wrong. |
| D7 | Recorded commands nothing calls: `CopyTextureToBuffer`, `ResolveTexture`, `SetDepthBias`, `SetLineWidth`, `SetBlendConstants` | Keep them, and test them through `RHICommandList` | A general-purpose RHI needs them. |
| D8 | One lifetime mechanism (Phase 5e) | Yes, after 5c and 5d | See below. |
| D9 | Order relative to the [upscaler work](UpscalerPlan.md) | Phases 1–2 now, then upscaler U1, then Phase 3 onward | See below. |

The longer reasons:

- **D1, fence path.**
  - Device selection already requires Vulkan 1.2 ([VulkanDevice.cpp:217](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L217)), and Vulkan 1.2 makes `timelineSemaphore` mandatory. The fence path is reached only when tests mask the feature.
  - Deleting it removes:
    - about 330 queue lines
    - the CPU-wait fallback in `DynamicRHI::PrepareSubmissionDependencies`
    - the engine's synchronous-upload branch ([RenderDevice.cpp:895](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L895))
  - Eight parameterised test suites lose their second run.
  - This replaces the enhancement plan's §8 rule to test both the timeline and fence paths.
- **D3, deferred layer.**
  - The worker stays and is measured under [enhancement plan §5](ZenRHIEnhancementPlan.md#5-measure-the-rhi-worker-before-changing-it); only its plumbing leaves the public API.
  - Direct NVRHI-style recording would delete about 4k more lines, but it needs an inline-versus-threaded ZenApp measurement. Revisit after the GI work.
- **D4, macOS.**
  - The code is about 120 lines, compiles only on Darwin and is untested. Linux does not compile at all.
  - The README already declares both unsupported. Provider-only surfaces are OS-neutral, so a later port adds no backend code.
- **D8, lifetime.**
  - This is the NVRHI model, and ZenRHI already pays its per-bind retain cost.
  - The risk is medium to high, because it changes when the last reference drops.
- **D9, upscaler order.**
  - U1 adds a host extension callback and native-handle accessors to ZenRHI, which Phases 3 and 5a reshape.
  - Phase 3 wraps U1's accessors in the interop header, and Phase 5a's extension table carries U1's hook.

If the owner raises the baseline to Vulkan 1.3 ([enhancement plan §1](ZenRHIEnhancementPlan.md#support-matrix)), do it before Phase 5a so the feature table is written once.

## 4. Phases

The order is 0 → 1 → 2 → 3 → 4 → 5 → 6.

- **Phases 1 and 2** are local, low-risk and need no decisions.
- **Phase 3** is the 0.3.0 API break.
- **Phase 4** comes before 5, because the restructured classes take their device context through constructors. Doing 5 first would rewrite them twice.
- **Within 5:** 5a, 5b and 5f are independent. 5c comes before 5d, and 5e needs both.
- **Phase 4 must not interleave with other executor changes** (backlog 5.9 rule).

### Phase 0 — Baseline

1. Record the baseline test results at `a83b6c0`:
   - ZenRHI CTest `unit|policy`;
   - the native Vulkan suite with validation required;
   - the engine tests that link ZenRHI (RenderCoreTest, CommonTest RHI and window tests, Assets integration);
   - a ZenApp smoke run with validation in inline and threaded modes.
2. Build through `tools/check_common.build`.
3. Close RTSS before native runs.
4. Record library and test line counts, so each phase can report its delta.

### Phase 1 — Delete dead code (about −600 lines, no behaviour change)

**Status: done on 2026-11-28, in ZenRHI 82dfd97.** Library lines fell by 433, from 31,655 to 31,222. All five ZenRHI CTest suites (658 cases) and the six engine suites that link ZenRHI pass with unchanged counts. D6's options and most of the context serial plumbing stayed, as noted below.

Every item has no caller in ZenRHI, its tests, examples, test-support or the engine. [Appendix A](#appendix-a-dead-code-for-phase-1) lists them with locations. In outline:

- **Core.**
  - `RHIBatchedShaderParameters::CopyFrom` and its only helper `CopyParameters`.
  - Four `RHIRenderTargetClearValue` factories that only repeat its constructors; `ColorClear` had one test caller. `DefaultColorClear`'s alpha 1.0 also disagreed with `DEFAULT_COLOR_CLEAR_VALUE`'s 0.0. Also make both `DEFAULT_*` constants `const`.
  - `WaitDeviceIdleChecked`, `DestroyAccelerationStructure`, `RHIFrameState::NumFramesSince` and `RHIResourceType::eDescriptorSet`.
  - The copy-source structs `RHIBufferCopySource` and `RHIBufferTextureCopySource`.
- **Recording and submission.**
  - The `AddWaitSemaphores` and `AddSignalSemaphores` span overloads.
  - The `ZEN_VK_RHI_DEBUG` blocks, which are never compiled because the macro defaults to 0.
  - The single-value `VulkanCommandBufferType`, and two uncalled `VkToString` specialisations.
  - The `IRHICommandContext::RHIGetLastSubmittedSerial` virtual and its override. The rest of the context serial plumbing feeds `RHIWaitUntilCompleted` and moves to Phase 5c.
  - Stale forward declarations (`class VulkanCommandBuffer` does not exist) and unused includes.
- **Binding and resources.**
  - `GetDescriptorSetLayoutData`, the unused `std::hash<VulkanDescriptorPoolKey>`, `VersatileResourceTemplate::Alloc<T>`, and the argument overload of `GetVkSubresourceRange`.
  - The write-only `lastUsedFrame` and `m_currentFrame`, the cache's `BeginFrame`, and the VERIFY-only `m_pDevice`.
  - Unreachable null-manager fallbacks, `AllocBuffer`'s unread `size`, and `PoolChain::numPools`.
- **Device and startup.**
  - `VulkanAPIVersion`, and `EnableMode` (whose `eManual` value is never used).
  - The unused `VulkanSurface` struct.
  - The write-only flags `hasSPIRV_14` and `hasDynamicRendering`, and the duplicate `m_physicalDeviceProperties`.
  - Two macro leftovers.
  - The `VK_KHR_get_physical_device_properties2` branch, which is core in 1.1 and always taken.
  - Platform defines that take no effect, and unused includes in `VulkanRHI.h`, which has 26 includers.

`GetMemoryAllocation` is **not** dead: the engine's [SceneUploadBenchmark.cpp:506](../tests/integration/Assets/CommonTest/SceneUploadBenchmark.cpp#L506) uses it through private headers. Phase 6 handles it.

### Phase 2 — Collapse forwarding layers and duplicates (about −1.1k, internal)

**Status: done on 2026-11-28, in ZenRHI 82dfd97.**
- **Size.** Library lines fell by a further 474, to 30,748; that is 907 below the 31,655 baseline.
- **Suites.** The same five ZenRHI suites (658 cases) and six engine suites pass with unchanged counts.
- **ZenApp smoke.** The `inspection.gltf` fixture passed in all four configurations (inline and threaded RHI, async compute off and on) with validation required and zero validation errors. Results are in `build/checks/zenrhi-phase2-smoke-20261128-140811`.

Done:
- **Creation and destruction:** everything listed below except `CreateResource<T>` and the `CreateTextureView` choice.
- **Startup:** the factory merge, the void `Init()` removal, the 1.2 constant and single-point device-loss reporting. `ZenRHI` is now an INTERFACE target.
- **Recording:** `RHICommandBase`, `pDestroy`, the shared destroy loop, the single context field, and `VulkanBindPointState` held by value.
- **Binding:** `ReleaseSlot`/`ReleaseAllSlots`, `CanRegisterLocked` reuse, `GetBindlessDescriptorType`, `TryVulkanTextureView`, `kMaxEntriesPerSlot`, aspect flags, push-constant forwarding, `FindBlock`, and `TryVulkanAccelerationStructure` in place of `dynamic_cast`.

These items moved after reading the code:

| Item | Moved to | Reason |
| --- | --- | --- |
| `CreateTextureView` vs `RHITexture::CreateView` | kept both | The executor version marshals view creation onto the RHI thread; calling `CreateView` from the render thread would change threading. Only the missing blocked check was added. |
| `CreateResource<T>` | 5b | The nine create functions share allocate, construct, `Init` and release, but each validates differently and checks a different native handle. Once 5b moves validation into Core, only the shared part remains. |
| `FlagInstanceExtensionSupported`, `EnumerateWSI`, queue-family checks, feature tables | 5a | `FindExtensionIndex` logs every miss, so the helpers are not interchangeable. The adapter snapshot and extension table replace all four. |
| `RHICommandListBase` into `RHICommandList` | 3 | The inline command structs need the list type while its signatures use the commands' `::Param`. Moving the commands private breaks the cycle. Callers name `RHICommandList::CommandCheckpoint` then. |
| `BeginCommand()`, workload release, tracker `RHICompletionSet`, barrier helper | 5c | The context flattening and per-workload ownership record rewrite the same lines. |
| Descriptor hashers and `Find` | 5b | The descriptor allocator merge replaces the cache keys. |
| Concurrent queue-family list | 4 | The device context supplies the families. |
| Enum `static_assert`s | 3 | Done together with the enum renames. |
| Validation counting in test-support | 3–4 | The message callback carries no message ID, and the factory swaps the global callback. It needs a structured diagnostic callback. Shipping builds already skip it because the messenger exists only with validation on. |

**Creation and destruction**
- Delete `RHIResourceFactory`, `VulkanResourceFactory`, `GetResourceFactory` and `m_pResourceFactory`.
  - Put the blocked-submission check inside each `VulkanRHI::Create*`. `CreateTextureView` skips that check today, so this also fixes it.
  - Replace the allocate, construct, `Init`, check-handle, release sequence, repeated nine times, with one `CreateResource<T>(info)` helper.
- Make `Destroy*` non-virtual inline calls to `ReleaseReference()` on `DynamicRHI`.
  - This deletes 15 overrides and the executor's blocking wrappers. Final destruction is already ordered on the RHI thread by the release path.
  - The 355 engine call sites stay as they are.
- Remove the pure virtual `RHIResource::Init()` from the base class. Nothing calls it polymorphically, and it forces eight empty FakeRHI overrides plus engine test doubles.
- In the executor:
  - Replace the 13 copies of `RHIResult r = InvokeChecked(...); return r ? r.GetValue() : fallback;` with one template.
  - `CollectGarbage`, `FlushRHIThread` and `CollectRetiredBindlessResources` have identical bodies. Keep one.
- `GetTransferCommandContext` is the same as `GetCommandContext(eTransfer)`. Remove it and update [RenderDevice.cpp:156](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L156).
- Keep one of `DynamicRHI::CreateTextureView` and `RHITexture::CreateView`. Recommended: `CreateView`, which means updating [RenderDevice.cpp:1271](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L1271).

**Startup**
- Delete these, and start the test fixtures through the factory as production does:
  - `DynamicRHI::Create(RHIAPIType)`, which reads the singleton and is called only by tests;
  - the public `VulkanRHIFactory.h` trio;
  - the void `Init()` paths on `VulkanRHI`, `VulkanDevice` and the bindless manager.
- Merge the `ZenRHI` library target, which holds one 49-line file, into `ZenRHIVulkan`. Keep the `ZenRHI::ZenRHI` alias.
- Merge the small duplicates:
  - `FlagInstanceExtensionSupported` into the `FlagExtensionSupported` template;
  - `EnumerateWSI` and `EnumerateVulkanProperties` into one bounded helper;
  - the three hand-written queue-family checks, derived from `SelectVulkanQueues` instead;
  - the two hand-maintained lists of about 20 features, replaced with one member-pointer table;
  - the 1.2 minimum, written in three places, into one constant.
- Report device loss in one place. Today it is reported up to three times per event. Extract `WaitIdleOrMarkLost()`.

**Recording and submission**
- Merge the single-child hierarchies: `RHICommandBase` into `RHICommand`, and `RHICommandListBase` into `RHICommandList`.
  - Drop `pDestroy`, which duplicates the virtual destructor.
  - Share the destroy loop that `Reset` and `RollbackCommands` both contain.
  - Replace the three context fields with `m_contextOwner`.
- Use one binding-state type per bind point, held by value. `VulkanGfxState` and `VulkanDispatchState` are identical, and `PreDispatch` is the first half of `PreDraw`.
- Replace the `EnsureRecording()` plus `GetCommandBuffer()` pair at 21 sites with one `BeginCommand()`. The getter loses its side effect of starting a workload.
- Merge `ReleaseWorkload`, `DestroyWorkload` and `DiscardWorkload` into `VulkanWorkload::ReleaseOwnership()`.
- Store an `RHICompletionSet` in lifetime-tracker entries, instead of a hand-rolled per-queue serial list.
- Build the barriers in `End`, `WriteBreadcrumb` and `MakeHostReadAvailable` through `VulkanPipelineBarrier`.

**Binding and resources**
- Bindless:
  - Extract one `ReleaseSlot()` from its four copies.
  - Have `RegisterBindlessResource` call `CanRegisterLocked`.
  - Delete `GetBindlessDescriptorType`.
- Add `TryVulkanTextureView` to [VulkanResourceCast.h](../ZenRHI/src/Vulkan/VulkanResourceCast.h), replacing two copies.
- Use one packed-key hasher, and remove the always-true key compare and the duplicate key storage in `VulkanDescriptorSetCache::Find`.
- Rename one of the two `kMaxSetsPerPool` constants. They have the same name and different meanings.
- Compute the texture aspect mask with `ToVkAspectFlags(GetTextureFormatAspects(...))`, and decide the default depth/stencil aspect in one layer.
- Forward `m_pushConstantsStageFlags` instead of copying it at three sites.
- Compute the concurrent queue-family list once on `VulkanDevice`.
- Extract one `FindBlock` for uniform block decoding.
- Add `static_assert`s for the 17 casts that assume RHI enum values equal Vulkan's, replacing `RayQueryEnumsMatchNativeValues`.
- Use the resource-type tag instead of the five `dynamic_cast`s in the acceleration-structure code.

**Test hooks out of production code**
- Move validation counting (`zen::test::GetValidationCount`, [VulkanContext.cpp:338](../ZenRHI/src/Vulkan/VulkanContext.cpp#L338)) into test-support through `messageCallback`.
  - Shipping builds lose a mutex and a string allocation per message.
  - The four engine tests that build their own debug messenger switch to it.

### Phase 3 — Public API cleanup, released as 0.3.0

**Status: done on 2026-11-28, in ZenRHI 82dfd97, except the interop item, which waits for U1.**
- **Size.** Public headers fell from 6,990 to 6,215 lines. The library is at 30,666 lines, 989 below baseline.
- **Suites and examples.** Every suite passes: ZenRHI has 659 cases, and RenderCoreTest has 659 tests after one moved to ZenRHI. All five examples run and verify their results, and the full engine release build passes.
- **ZenApp smoke.** All four configurations passed with zero validation errors (`build/checks/zenrhi-phase3-smoke-20261128-150830`).

It runs before upscaler U1, so U1 is written against the cleaned API. The interop item waits for U1. The shared v0.3.0 bump happens at commit time, because it touches every Zen repository.

- **Group 1, command internals: done.**
  - `RHICommandListBase` is merged into a `final` `RHICommandList`.
  - The concrete commands and `ALLOC_CMD` moved to private `src/Core/RHICommands.h`. The public header shrank from 1,204 to 617 lines.
  - `RHIDrawIndexedArgs` and `RHIDrawIndexedIndirectArgs` replace `::Param`, and `BindPipeline(RHIPipeline*)` lost its unused type.
  - The engine's parameter-snapshot test moved to ZenRHI's Core suite.
- **Group 2, headers: done.**
  - Deleted the dead `zen::TextureFormat`; the engine uses its own `rc::TextureFormat`.
  - `RHIGeometryBuffer` became the engine's `RDGGeometryBuffer`.
  - `RHIError.h` no longer includes `RHIOptions.h`, because `VerifyTeardownOwnership` moved out of line.
  - `Color` is now in `namespace zen`.
  - `kInlineDescriptorSetCount` moved to `VulkanPipeline.h`.
- **Group 3, one way: done.**
  - `RHIBatchResult` lost its unread error string, the duplicate `presented` flag and two timing fields.
  - The two queue-capability forwards are gone; callers read `GetQueueCapabilities()`.
  - `WaitForCompletion` now returns `RHIWaitResult`, which converts to `true` only on completion; `WaitForCompletionChecked` is gone.
  - The executor returns at once on published completion, so the engine dropped its "query, then wait" pre-checks.
  - `QueryProgressChecked` and `RHIProgressResult` are gone.
- **Group 4, names: done.**
  - Dropped the `F` from `VulkanCommandBuffer`, `VulkanCommandBufferPool` and `VulkanCommandListContext`.
  - Renamed `RHITextureSubResourceRange` to `RHITextureSubresourceRange`, and `eViewPort` to `eViewport`.
  - Fixed the blend enum typos `eSrcAlphaSaturate`, `eSubtract` and `eReverseSubtract`, plus three member and constant typos.
- **Group 5, engine names: done.**
  - Comments describe the host, not RenderCore.
  - `VkApplicationInfo` names the library, with its version from CMake.
  - The test shader macro is now `ZENRHI_TEST_SHADER_DIR`.
  - The README's engine paragraphs moved to [RHIRenderCoreContracts.md](RHIRenderCoreContracts.md).

These items moved after reading the code:

| Item | Moved to | Reason |
| --- | --- | --- |
| Private `RHIWorkerTimer`, `TimedRHICall`, `RHIExecutionCounterStorage` | 5d | The public `RHIThread.h` uses the timer, and the engine needs `RHIThread.h` until 5d covers its direct `Invoke` calls. |
| Private `GRHIFrameState`, frames-in-flight on the executor | 4 | FakeRHI and about 20 test sites use the global. It leaves together with the other globals. |
| Destruction listener in place of `NotifyResourceDestroyed` | 4 | `~RHIResource` reaches the executor only through `GDynamicRHI`. |
| `RHIWindowHandle::provider`, `surfaceExtensions` | 5f | Provider-only surfaces reshape both. |
| `IRHICommandContext` private | 5d | The installed FakeRHI and engine mocks implement it until the backend interface splits from the facade. |
| `RHIStatus` vs `RHIResult<void>` | kept | `RHIResult<void>` exists for the generic `InvokeChecked` and has two uses. Merging would rewrite 36 `RHIStatus` sites for no behaviour gain. |
| `SetValidationEnabled` vs `SetValidationMode` | kept | The bool setter is a convenience over the same mode, not a second mechanism. |
| `messageCallback` vs `SetRHIMessageCallback` | 4 | Options and callback ownership move to the device. |
| One `GetHealth()` | kept | The four queries carry different facts. `HasDeviceLoss` can rise after the first terminal cause, and `GetLastSubmissionError` reports a non-terminal rejection. `AreSubmissionsBlocked` is the cheap atomic check that 158 sites use. |
| `spirvFileName` into ZenCore | 6 | About 43 engine test sites use it as a lookup key on created shaders. Phase 6 rewrites those lookups. |
| Renaming the `rdg_*` and `review_*` test shaders | 6 | Done with the test regrouping. |
| Parameter `p` prefixes, span constness, getter prefixes | 5 | Applied as each subsystem is rewritten, so the lines are touched once. |

**Hide internals**
- Move the `RHICommand*` structs, `ALLOC_CMD` and the definition of `IRHICommandContext` to `src/Core`.
  - The public headers keep a forward declaration. The engine never calls `RHI*` methods directly, and ZenRHI tests already include `src`.
  - Publish `RHIDrawIndexedArgs` and `RHIDrawIndexedIndirectArgs` in place of `::Param`, and update [RDGPassCompiler.cpp:764,799](../ZenCore/Source/Graphics/RenderCore/V2/RDGPassCompiler.cpp#L764).
  - `BindPipeline` loses its ignored pipeline-type argument. That touches RDG, RenderGraph, two examples and four tests.
- Move these to `src`: `RHIWorkerTimer`, `RHIExecutionCounterStorage`, `TimedRHICall`, `kInlineDescriptorSetCount`, and the `RHISubmissionState` test friend.
- Make `GRHIFrameState` private, and pass frames-in-flight to the executor. This replaces [RenderDevice.cpp:125](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L125).
- Replace `NotifyResourceDestroyed` and `DrainDestroyedResourceIds` with an opt-in destruction listener on the executor.
  - Hosts that never drain no longer accumulate an unbounded vector.
  - The engine side is [RenderDevice.cpp:521](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L521).

**Split the catch-all headers**
- Split `RHICommon.h` (1,593 lines) by topic: capabilities, shader, pipeline state, barriers, render targets.
- `RHIError.h` stops including `RHIOptions.h`, which it needs only for `VerifyTeardownOwnership`.
- Move the engine types to ZenCore: `TextureFormat` (61 engine references, 0 in RHI), `RHIGeometryBuffer` (labelled "Transitional raw adapter") and `RHIShaderCreateInfo::spirvFileName`.
- Take the Vulkan concepts out of the neutral headers: `RHIWindowHandle::provider` and `RHIDeviceDesc::surfaceExtensions`.
- Move `Color`, `GDynamicRHI` and `GRHIFrameState` out of the global namespace.

**One way to do each thing**
- **Checked and unchecked pairs.** `WaitForCompletion`, `WaitDeviceIdle`, `PrepareForPresent`, `Present`, `AcquireNextImage` and `CreateRHI` each keep only the result-returning form, under the plain name. Engine callers check the result.
  - `Invoke` and `InvokeChecked` stay distinct: ordered cleanup must not be able to fail.
- **Health.** One `GetHealth()` replaces `AreSubmissionsBlocked`, `GetTerminalError`, `HasDeviceLoss` and `GetLastSubmissionError`.
- **Progress.** Keep the cached-versus-poll distinction from the [synchronization plan](SynchronizationSimplificationPlan.md), with one name each:
  - a cached snapshot;
  - `PollGPUProgress()`;
  - a `WaitForCompletion(const RHICompletionSet&)` that returns at once on cached completion.

  That removes the engine's "query, then wait" pattern ([RenderDevice.cpp:1900](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L1900), [StagingUploadQueue.cpp:207](../ZenCore/Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp#L207)).
- **Other duplicate pairs, keep one of each:**
  - `SetValidationEnabled` and `SetValidationMode`;
  - `RHIDeviceDesc::messageCallback` and `SetRHIMessageCallback`;
  - `RHIStatus` and `RHIResult<void>`;
  - `RHIBatchResult::error` and `cause`;
  - `presented` and `presentation.presented`;
  - `RHIShaderCreateInfo::stageFlags`, which is ignored whenever `spirv` is set.
- **Small simplifications.**
  - Make `RHIBatchResult::executionCPUUs` and `queueWaitUs` locals in the executor.
  - Callers read `GetQueueCapabilities()` instead of the two one-line forwards.
  - Flatten `GetModules().GetModules()`.

**Names**, one mechanical commit per family:
- `FVulkanCommandBuffer`, `FVulkanCommandBufferPool` and `FVulkanCommandListContext` lose the `F`.
- Enum typos:
  - `eSrcAlphaSatuate`, `eSubstract` and `eReverseSubstract` are unreferenced, so renaming them is free.
  - `eViewPort` → `eViewport` touches about 20 engine sites.
- `RHITextureSubResourceRange` casing, matching `RHITextureSubresourceLayers`.
- One parameter style. The `p` prefix is the majority.
- Span constness, and `RHIDeviceDesc` getters gain `Get` or `Is`.
- Typos: `m_pSemaphoreManger`, `defaulFormat`, `kContanierIdleGCThreshold`.

**Engine names out of the library**
- Take the VkApplicationInfo names and version from `RHIDeviceDesc` ([VulkanContext.cpp:535](../ZenRHI/src/Vulkan/VulkanContext.cpp#L535)).
- Rewrite comments that name RenderCore, RDG, RenderDevice or Directional-GI as caller-neutral contracts.
- Move the README paragraphs on RenderDevice, RenderSubmissionHistory, RenderScene, ZenApp and the warmed shader keys to [RHIRenderCoreContracts.md](RHIRenderCoreContracts.md).
- Rename `RDG_REFLECTION_TEST_PATH` to `ZENRHI_TEST_SHADER_DIR`, and rename the `rdg_*` and `review_*` shaders by subject.

**Interop.** After upscaler U1, `ZenRHI/Vulkan/VulkanInterop.h` is the single place for native access:
- device handles, image, view, buffer, command buffer and queue;
- format conversion;
- U1's host extension hook.

### Phase 4 — Pass the device instead of globals (needs D2)

**Status: done on 2026-11-28, in ZenRHI 82dfd97.** ZenRHI defines no process-wide device, thread, frame state or option object.
- **Size.** The library is at 30,800 lines, 856 below baseline; the device plumbing added 134 lines back after Phase 3. Public headers are at 6,269 lines.
- **Suites and examples.** Every suite passes. ZenRHI has 658 cases after the obsolete global-pointer test was deleted, and the six engine suites are unchanged. All five examples run and verify their results, and the full engine release build passes.
- **ZenApp smoke.** All four configurations passed with zero validation errors (`build/checks/zenrhi-phase4-smoke-20261128-163610`).

What changed, step by step:
- **Back-pointers.** The swapchain, viewport, queue, descriptor managers, binder and bindless manager hold their `VulkanDevice`. Buffers, textures, views, samplers, shaders, pipelines and acceleration structures hold their `VulkanRHI`. Free helpers take the backend as a parameter.
- **Allocator.** The VMA allocator is a `VulkanRHI` member, next to the other device-scoped managers; 5a's device split moves them together. Tests install observer allocators through `VulkanRHITestAccess`.
- **Description.** `RHIOptions.h` became `RHIDeviceDesc.h`, and the singleton is gone.
  - Each backend keeps its own copy, read through `GetDeviceDesc()`.
  - Two settings are process-wide because they apply when no device exists: `SetRHIMessageCallback`, which replaces `RHIDeviceDesc::messageCallback`, and the new `SetRHIStrictTeardownChecks`.
  - Device creation applies the stall-report and worker-timing settings.
  - The engine keeps its description in `RenderConfig::rhiDevice`, and a window registers its surface extensions there.
  - `ZEN_VULKAN_DEVICE` is read by the hosts: `RenderConfig` and the ZenRHI test harness.
- **`GVulkanRHI`.**
  - Device loss and creation failures report through `VulkanRHI::CheckDeviceLoss` and `ReportCreationFailure`, so `MakeVulkanError` has no side effect.
  - `GetVulkanDeviceHandles` takes the device and reads `DynamicRHI::GetNativeObject`.
  - `RHIDebug` is deleted: its one caller set a texture name the backend already applies at creation.
- **Runtime.**
  - Each device owns an `RHIDeviceRuntime`: its ordered `RHIThread`, its `RHIFrameState` and an opt-in `RHIDestructionListener`.
  - Resources and command contexts derive from `RHIDeviceChild` and hold a reference to their device's runtime. A release after teardown meets closed admission, not freed memory.
  - The executor drives its backend's runtime and takes frames in flight at construction.
  - `GetRHIThread()`, `GRHIFrameState`, `GDynamicRHI`, `OpenAdmission`, `NotifyResourceDestroyed` and `DrainDestroyedResourceIds` are gone. `RenderDevice` is now the destruction listener.
- **Engine.** ZenCore defines its own `GDynamicRHI` beside `RenderDevice`, as UE5 does, so the engine call sites are unchanged. Routing them through `RenderDevice` is engine work, outside this plan.
- **Queue sharing.** `VulkanDevice` computes its concurrent queue-family lists once, when it creates the queues.

Still deferred:

| Item | Moved to | Reason |
| --- | --- | --- |
| Validation counting in test-support | 5d | The message callback carries no message ID. The counts move out once diagnostics get a structured callback. |

Work from the leaf classes up. The size stays about the same.

1. **The device owns its description.**
   - The device keeps an immutable copy of `RHIDeviceDesc` and exposes it through `GetDeviceDesc()`.
   - The engine builds that description from its own configuration, instead of mutating the singleton:
     - [RuntimeApplication.cpp](../ZenApp/RuntimeApplication.cpp) and [EditorApplication.cpp](../ZenEditor/App/EditorApplication.cpp);
     - [WindowSurface.cpp:61](../ZenCore/Source/Platform/WindowSurface.cpp#L61);
     - the reads in RenderGraph and ShaderProgram.
   - Delete the `RHIOptions` singleton, and the copy-and-restore in `InitializeBackend`.
   - The `ZEN_VULKAN_DEVICE` environment read moves to ZenApp.
2. **Pass a device context.**
   - A `VulkanDeviceContext` carries the device, allocator, lifetime tracker, counters, bindless heap, uniform ring and description. Constructors receive it.
   - Replace `GVulkanRHI` in this order: swapchain, viewport, buffer, texture, sampler, shader, pipeline, descriptors, command list, queue.
   - `GVkMemAllocator` moves into `VulkanDevice`.
   - The debug messenger gets its context through user data.
3. **Remove `GDynamicRHI` from ZenRHI.** `RenderDevice` holds the executor, and the 36 engine sites call through it.
4. **Replace `GetRHIThread()` and `GRHIFrameState`** with executor members. Each resource gets a release sink when it is created, so `ReleaseReference` no longer reaches a process-wide thread.
5. **Rewrite the tests that swap globals:**
   - [AsyncComputeLifetimeTests.inl:191](../tests/integration/RenderCoreTest/AsyncComputeLifetimeTests.inl#L191);
   - SceneUploadBenchmark, RHIWindowThreadingTests and VulkanPresentationReviewTests;
   - the 109 `RHIOptions::GetInstance()` uses in tests.

### Phase 5 — Restructure the large subsystems

**Status: done on 2026-11-28 within the owner's scope, in ZenRHI 82dfd97.** Slim 5a replaced full 5a. The `DynamicRHI` split and 5e moved after reading the code; see the deferral table.
- **So far.** The library is at 29,353 lines, 2,302 below baseline, and public headers are at 6,267 lines. Every suite passes, at ZenRHI 152/82/349 and engine 15/1/6/20/71/659. All five examples run without validation errors or leaks, the full engine release build passes, and the ZenApp smoke matrix passed with zero validation errors in all four configurations (`build/checks/zenrhi-phase5b-smoke-20261128-185040`).
- **5c, submission: done.**
  - Timeline semaphores are a device requirement (D1). The fence submission path, `VulkanFence` and `VulkanFenceManager` are deleted, and so is Vulkan's CPU wait for cross-queue dependencies.
  - The timeline-or-fence test parameterisation is gone. The ZenRHI integration suite fell from 423 to 350 cases, and the engine's window tests from 97 to 71, without losing a timeline case.
  - One backend `SubmitCommandLists(lists)` replaces `FinalizeCommandLists`, `SubmitPlatformCommandLists` and `FlushAllGPUCommands`. `FlushAllGPUCommands()` stays as a non-virtual flush of pending work.
  - `RHIPlatformCommandList`, `VulkanPlatformCommandList` and its pool are deleted. Vulkan keeps the translate and submit halves as a backend-private `VulkanTranslatedBatch`, which tests drive directly.
  - Contexts no longer track submission serials. `RHIWaitUntilCompleted`, `WaitForLastSubmittedWork`, `SetLastSubmittedSerial` and `MarkWorkloadPendingFlush` are gone; callers read the list's receipt or wait on the queue.
  - The neutral `asyncSubmissionDependencies` capability and the engine's handling of it stay, because fake backends and future ones can report false.
  - Each workload has one ownership record: lifetime ids, resource references and the bindless transaction, with `Accept(queue, serial)` and `Release`. Release and destruction share one path, and acceptance recurses over merged children in one place.
  - The command buffer owns a `VulkanBreadcrumbRecorder` and a `VulkanGPUTimingRecorder` (in `VulkanCommandRecorders.*`), which hold the breadcrumb buffer, the timestamp pool, the scopes and the frame interval. The buffer keeps the state machine and the bound-state cache. The unused pool constant moved to the timing recorder.
- **5a, device startup: slim scope done.**
  - One device extension table (`VulkanDeviceExtensionName` and its name list) and one `VulkanDeviceFeatures` chain replace the 14 device extension classes and their three-hook protocol. The chain holds the core features, the Vulkan 1.2 struct and one struct per optional extension.
  - `SelectDeviceExtensions` reads the advertised names and the reported features once, and decides the names, the enabled features and the capability flags, including the ray-tracing dependency chain. The device's gating loop and the per-class flag writes are gone. `VulkanExtension.cpp` fell from 946 to 431 lines; the header gained 107.
  - The promoted features come from `VkPhysicalDeviceVulkan12Features`: descriptor indexing, timeline semaphores, host query reset, buffer device addresses, 64-bit buffer atomics and sampler mirror-clamp. Validation required the mirror-clamp bit in the 1.2 struct once that struct is chained, so the old extension name is no longer enabled.
  - `VulkanDevice` keeps the supported and the enabled core features in separate members.
  - The capability test driver fakes the 1.2 struct; device selection still queries the individual structs.
  - Surfaces come only from a `VulkanSurfaceProvider` (D4). `RHIWindowHandle` is a window plus its provider. The Windows and macOS platform classes and the remaining macOS code are deleted. Test windows and the Triangle example supply providers.
  - `RHIViewport` has one present API: `PrepareForPresent` returns a status and `Present` returns an `RHIPresentResult`. The unchecked pair and the viewport's inline recreation are gone; the host recreates between frames.
  - The semaphore manager creates and destroys. Its free list, `ReleaseSemaphore`, and the recycling tests are gone.
  - `VulkanSwapchainConfig` holds the chosen image count, format, present mode, transform, usage and alpha. Choosing it is pure over the queried surface lists, which shortened the swapchain constructor from 229 to 179 lines.
  - A move-only `VulkanSurface` owns the window surface. It moves from viewport creation through the recreation info to the swapchain and back, replacing four explicit destroy sites and the scoped surface guard.
  - The swapchain alone owns its images and acquire state. The viewport reads the acquired image and its semaphore through two getters, and no longer copies the image list.
  - `VulkanSwapchain::Destroy` (140 lines) is a wait step, a sync-object step and the hand-off. The repeated device-idle-with-loss handling is one function. `VulkanViewport::CreateSwapchain` (133 lines) moved its back-buffer creation into `CreateBackBuffers`.
- **5b, descriptors: done.**
  - The bindless heap and its capacity policy left `VulkanDescriptorPool.*` as `VulkanBindlessHeap` and `VulkanBindlessCapacity`. The uniform ring left `VulkanBuffer.*` as `VulkanUniformRing`.
  - Core `RHIShaderUtil` holds `ValidateShaderLayout` and the global-heap binding rule, `IsGlobalHeapBinding`, which bindless registration also uses. The engine's ShaderReflectionTests no longer include `VulkanPipeline.h`.
  - One `VulkanDescriptorTraits` table holds the Vulkan type, limit class, stride, required usage and image layout. It replaced `ShaderResourceTypeToVkDescriptorType`, `GetResourceStride`, the limit-class switch and the per-type choices in descriptor writes. Reflection maps SPIR-V descriptor types through one table too.
  - `VulkanSetLayout`, created once per distinct set by `VulkanSetLayoutCache`, replaced the shader's four per-set arrays, their getters and the pool manager's layout-id registry. Shaders now share native set layouts, and the global heap exposes its layout the same way.
  - `VulkanDescriptorAllocator` replaced `VulkanDescriptorPool`, the pool-set container, `VulkanDescriptorSetCache` and `VulkanDescriptorPoolManager2`; `VulkanDescriptorPool.*` is deleted. One map holding the newest set per key replaced the entry list and its hash chain. Contexts record the set's lifetime id instead of a container.
  - The allocator has `Find` and `Insert` rather than the planned single `Acquire(key, layout, writes)`. Descriptor writes are built only after a miss, so a hit stays as cheap as before.
  - Binding: each bind point already had one binder (`VulkanBindPointState` owns its `VulkanDescriptorSetState`). Set-time validation stays layout-only. A content-cache hit now checks only the dynamic offsets: the key names the resources and ranges, so the hit's set was validated when it was written. A miss validates every element while it builds the writes.
  - Core `RHIValidation` holds the neutral rules for buffers, textures, texture views, samplers and the shader stage contract. Vulkan keeps the format queries, BC support and the sampler LOD-bias limit. Acceleration-structure description rules stay in Vulkan, because they check native device addresses.
  - Creation errors are values. `Init()` returns `RHIError`, every `CreateObject` returns `RHIResult<T*>`, and `VulkanRHI` has no last-creation-error slot. The uniform ring and the descriptor allocator return their errors with their results. The viewport takes back-buffer errors from the texture results.
  - `CreateResource<T>` became two helpers: `NewVulkanResource<T>` allocates, constructs and binds, and `KeepIfInitialized` keeps or releases on the `Init` result. Pipelines need different init calls, so one helper could not cover all nine.
  - Long functions:
    - `ResolveDescriptorBinding` is one element at a time, written straight into its payload slot.
    - `ResolveSet` moved its ordinary-set path into `ResolveOrdinarySet`.
    - `RegisterBindlessResource` uses `FindFreeSlotLocked` and `ClaimSlotLocked`.
    - `InitGraphics` uses rasterization and depth-stencil translators; the depth-stencil one handles both stencil faces with one translator.
    - `VulkanShader::Init` and `InitNativeObjects` are split into specialization, module, vertex-input and pipeline-layout steps.
    - `VulkanPipeline.cpp` is split by job; shaders and their layout limits are in `VulkanShader.cpp`.
- **5d, executor: done except the facade split.**
  - `ExecuteFrame` (186 lines) is the frame skeleton plus `PresentFrame` and `FinishFrame`. `PreparePresentCopy` is the one presentation-copy preparation that `ExecuteGroups` and the frame's own present share.
  - `ReleaseWhenComplete` dispatches its hand-off in order instead of waiting for the RHI thread. The engine never called it; its staging retirement already uses the render graph's own retirement tracking.
  - `RHIViewport::NeedsRecreation` and `CanRecreate` are callable from any thread and run on the RHI thread themselves. The engine's three direct `thread.Invoke` calls in `RenderDevice` are gone, so production code no longer reaches the RHI thread.
  - Diagnostics are structured. The message callback receives `RHIMessage{severity, id, text}`, and validation messages carry their VUID or hazard name. `VulkanRHI` no longer counts validation messages; test-support's header-only `ValidationCounter`, installed by the test main, does.

Deferred within Phase 5:

| Item | Moved to | Reason |
| --- | --- | --- |
| Merge `VulkanCommandContextBase` into the context | not done | After 5c the base is a cohesive recording core (workloads, ownership, semaphores, transactions), and the lifetime tests drive it on their own queues without an RHI context. Merging would grow the 2,000-line context and need a test-only constructor. |
| Split `DynamicRHI` into a backend interface and a public facade (5d) | Phase 6 | The executor forwards about 45 calls, and four classes implement `DynamicRHI`: the executor, `VulkanRHI`, test-support's `TestRHI` and an engine benchmark mock. The split rebases all four and most of the 1,849-line `FakeRHI.h` for about no line savings. It belongs with Phase 6's `FakeRHI` split. Owner decision, 2026-11-28. |
| One lifetime mechanism (5e) | own change, after the DLSS/FSR work | It changes when final references drop, across the RHI and the engine's `RenderDevice` and staging queue, and is the plan's riskiest item. It needs its own ZenApp scene-reload validation run. Owner decision, 2026-11-28. |
| Make `RHIThread.h` private | Phase 6 | Production code no longer calls the RHI thread directly, but about 200 engine test sites reach the runtime and its thread. They move with Phase 6's test rewrite. |

**5a. Device startup** (about −0.8k)

**Owner decision, 2026-11-28: slim 5a.** Only the constexpr extension table, the Vulkan 1.1 and 1.2 feature chain, and separate supported and enabled feature members are in scope, about −0.5k to −0.6k. The instance, adapter and device split, the adapter snapshot, the method moves and the other long-function splits are dropped. `GetEnabledExtensions` is rewritten by the table anyway. Phase 5 ends the refinement for now; Phase 6 is not scheduled.

- **Split `VulkanRHI`**, which today owns about 12 unrelated responsibilities:
  - `VulkanInstance`: loader, instance, layers, messenger;
  - `VulkanAdapter`: one snapshot per GPU (properties, the Vulkan 1.1 and 1.2 feature chain, queue families, extensions, heaps, unsupported reason, score), which also serves enumeration without a device;
  - `VulkanDevice`: device, queues, sync objects, allocator, pipeline-cache unit, capabilities that are const after creation;
  - a thin `VulkanRHI` facade.
- **One adapter snapshot feeds** selection, the required-profile check, device creation and `RHIGPUInfo`. Choose one meaning for device-local memory and document it.
- **Replace the 14 per-extension classes with a constexpr table.** Each entry holds the name, struct type and size, the offsets of the required and enable flags, the flag bit, the core version, dependencies, required-ness and U1's host extensions.
  - Chaining `VkPhysicalDeviceVulkan11Features` and `VkPhysicalDeviceVulkan12Features` removes five of the classes.
  - `VulkanExtension.cpp` should drop from 979 to about 400 lines.
- **Split the long functions:**
  - `SetupDevice` (214 lines);
  - `EvaluatePhysicalDeviceCandidate` (165), which becomes scoring over the snapshot;
  - `GetEnabledExtensions` (165);
  - `PopulateGPUInfo` (154);
  - `CreateInstance` (129).
- **Move `VulkanRHI` methods to their owners.** They are spread over nine .cpp files today; pipeline-cache code, for example, moves to its own unit.
- **Keep supported and enabled features in separate members.** Today one member holds the first and then the second.

**5b. Descriptors and binding** (about −1.0k)
- **`VulkanSetLayout`, created by a shared layout cache.** It holds the native layout, layout id, pool sizes, dynamic offset slots and descriptor list.
  - It replaces the four parallel arrays in `VulkanShader`, their four single-use getters, and the layout-id registry in the pool manager.
  - Native layouts are then shared across shaders.
- **`VulkanDescriptorAllocator`** merges `VulkanDescriptorPool`, the pool-set container, `VulkanDescriptorSetCache` and `VulkanDescriptorPoolManager2` behind one `Acquire(key, layout, writes)`. The friend access and the six-level allocation chain go away.
- **Three renamed or moved units:**
  - `VulkanBindlessHeap`, with its capacity policy in its own file and capacities passed in;
  - `VulkanUniformRing`, out of `VulkanBuffer.*`.
- **One binder per bind point.** It validates parameters once, when they are set; the resolve step checks only offsets and ranges. Keep one redundant-bind check per level where a test shows it matters.
- **One descriptor-type traits table** holds the Vulkan type, pool type, limit class, stride, required usage and image layout.
  - It replaces about six per-type switches, and makes `ParseSpvReflectDescriptorBinding` table-driven.
- **Move to Core `RHIShaderUtil`:** the "set 0 is the global heap" rule (four copies today) and `ValidateShaderLayout`. The engine's ShaderReflectionTests then stops including `VulkanPipeline.h`.
- **Core `RHIValidation`** holds the backend-neutral creation rules, checked against `RHIGPUInfo`: texture shape, view range and type, sampler, buffer, acceleration-structure description, stage contract. Vulkan keeps the format and limit queries.
- **Split the long functions:**
  - `InitNativeObjects` (281 lines);
  - `InitGraphics` (274);
  - `ResolveDescriptorBinding` (141);
  - `RegisterBindlessResource` (112 lines, 5 nesting levels);
  - `ResolveSet` (7 nesting levels).

  Also split `VulkanPipeline.cpp` and `VulkanDescriptorPool.cpp` by job.
- **Return creation errors as values**, replacing the `SetCreationError` side channel.

**5c. Submission** (about −0.6k, needs D1)
- **Delete the fence path:**
  - `SubmitWorkloadsWithFences`;
  - `VulkanFence` and `VulkanFenceManager` (the swapchain already uses raw fences);
  - the fence parameterisation in tests;
  - the engine's synchronous-upload branch.
- **One backend `Submit(groups)`.**
  - It replaces `FinalizeCommandLists` → `SubmitPlatformCommandLists` → `FlushAllGPUCommands`, which `ExecuteBatch` always calls back to back, and the viewport's `SubmitRecordedWorkloads`.
  - `RHIPlatformCommandList`, `VulkanPlatformCommandList` with its pool, and the pending list go away.
  - Exact per-group receipts stay. They are why the synchronization plan deferred a shared owner.
- **One ownership record per workload.** It holds lifetime ids, resource references, the bindless transaction and merged children, with `Accept(queue, serial)`, `Abandon` and `Release`.
- **Split `FVulkanCommandBuffer`.**
  - The state machine and the bound-state cache stay.
  - Extract a breadcrumb recorder, and a timestamp recorder. GPU timing is spread over three classes today.
  - Merge `VulkanCommandContextBase` into the context.
- **Delete the context serials.** `RHIWaitUntilCompleted`, `WaitForLastSubmittedWork`, `SetLastSubmittedSerial`, `MarkWorkloadPendingFlush` and the context's `GetLastSubmittedSerial` give way to list receipts plus `WaitForCompletion`. Tests that wait through them move to receipts.

**5d. Executor as a facade, not a backend** (about −0.5k)
- **Split `DynamicRHI` in two:**
  - an internal backend interface: submit, per-queue progress, native factories;
  - a public device facade: resources, submission groups, progress, health, viewports, debug names, interop.

  The executor implements the facade over the backend. Inline mode calls the backend directly, as UE5's bypass does.
- **Cover the engine's five direct `GetRHIThread().Invoke` calls** ([RenderDevice.cpp:1694](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L1694), [TextureManager.cpp:788](../ZenCore/Source/Graphics/RenderCore/V2/TextureManager.cpp#L788)) with:
  - a viewport-recreation query;
  - a `SetDebugName` that replaces `RHIDebug` and `VulkanDebug`.

  `RHIThread.h` then becomes private.
- **`ReleaseWhenComplete` stops blocking.** A retirement token, `CaptureRetirement()` / `IsRetired()`, serves the engine's staging retirement until 5e.
- **Split `ExecuteFrame`** (186 lines, repeating `ExecuteGroups`' presentation setup).
- **Resource creation stays marshalled** until the [§5.3 measurement](ZenRHIEnhancementPlan.md#53-off-worker-resource-creation) says otherwise.
- **Expected effect:** inline mode gets slightly cheaper; threaded mode is unchanged.

**5e. One lifetime mechanism** (needs D8)
- **The new rule.**
  - Submitted workloads keep the command list's references until the queue serial completes.
  - Descriptor pools and uniform blocks use the same retention.
  - Bindless slots keep explicit retirement.
- **This removes:**
  - the executor's deferred list;
  - most of `VulkanLifetimeTracker`;
  - the double retention in workloads;
  - the engine's frame-slot pending-release lists and staging retirement lists ([RenderDevice.cpp:1767](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp#L1767), [StagingUploadQueue.cpp:139](../ZenCore/Source/Graphics/RenderCore/V2/StagingUploadQueue.cpp#L139)).

  That is about −0.5k to −1k across RHI and engine, and about half of the README's lifetime section.
- **Verification:** the lifetime and retention tests, plus a ZenApp scene reload with validation.

**5f. Presentation** (about −0.3k)
- **Create surfaces only through `VulkanSurfaceProvider`.**
  - The native Win32 path becomes a provider in test-support and the Triangle example.
  - The Windows and macOS platform classes, whose bodies are identical, go away (D4).
- **One RAII `VulkanSurface`.** Surface ownership no longer passes through four holders.
- **`VulkanSwapchain` alone owns images and acquire state.** `VulkanViewport` keeps the backbuffers and resize, and stops mirroring swapchain state.
- **Move a pure `ChooseSwapchainConfig(caps, formats, modes, desc)` out of the swapchain constructor** (229 lines). Also split `Destroy` (140) and `CreateSwapchain` (132).
- **Drop the test-only synchronous present path.** Once only the checked APIs remain (Phase 3), it has no user.
- **Fix semaphore recycling.** Production never recycles semaphores today. Either the swapchain uses `ReleaseSemaphore`, or delete it.

### Review follow-up

**Status: done on 2026-11-28, uncommitted.** [ZenRHIRefinementReview.md](ZenRHIRefinementReview.md) reviewed 82dfd97 and a7e1efb0. Every finding was checked against the code. These were introduced by the refactor and are fixed:

- **One live backend per process (review 2.1, filed there as pre-existing).** The old factory refused a second backend while `GVulkanRHI` was set; removing the global removed the guard. `EnumerateRHIAdapters` during a live device then ran `volkFinalize()` and wiped the live device's entry points. `VulkanRHI` now claims the process-wide volk loader in its constructor; a second one fails `InitChecked` with `eInvalidArgument` and leaves the loader alone. Worker timing is applied only when a device is created.
- **Destroy order (1.2).** The eight resource `Destroy()` methods read `m_pRHI` after running their own destructor. `DeleteVulkanResource` reads the allocator first.
- **Blocked submission is fatal (1.3).** `SubmitCommandLists` and `SubmitRecordedWorkloads` return `eFatal` whenever submissions are blocked, not only for device loss.
- **Standalone teardown (1.4).** `VulkanRHI::Destroy` closes cleanup admission (`RHIThread::CloseCleanupAdmission`), so a release after a backend torn down without an executor leaks with a report instead of reaching the deleted backend.
- **SceneUploadBenchmark (1.1)** builds again: it passes its `RHIDeviceDesc`, waits on the queue's serial and drops the removed global.
- **Stale documentation (1.9)** in the README, the programming guide and code comments, and the leftover dead helpers, duplicate includes and repeated device queries from the review's low list.

The engine commit's line "The viewport recreation queries no longer go through the RHI thread" is inaccurate: the engine no longer calls `Invoke` itself, but `VulkanViewport::NeedsRecreation` and `CanRecreate` still run on the RHI thread through `Invoke` inside the viewport. The per-frame wait predates the refactor.

The review's other findings predate the refactor and stay open; 2.2 (micromap alignment), 2.5 (a bindless slot claimed by a rejected parameter set) and 2.13 (creation validation gaps) matter most. 1.6 (shader creation errors collapse to `eBackendFailure`) was introduced but only loses the error code, and is left for the error-path work.

### Phase 6 — Tests and documentation

**Tests** (about −2.5k lines and 40–45 tests, without losing a failure that only they catch)
- **Regroup files by subsystem:**
  - Resources, DeviceStartup, ShaderPipeline, Binding, Recording, Submission, Presentation, Timing and Core;
  - a separate benchmark target for the `Measure*` cases, SceneUploadBenchmark and EncoderAllocationBenchmark.

  Retire the Review, ProductionFailure, Verification and ValidationProbe file names.
- **Collapse the overlapping clusters:**
  - The retention matrix written once per allocator becomes tracker unit tests, one wiring test, and one rejected-retry case per allocator.
  - The accepted-prefix-on-rejection case, written six times, becomes one.
  - Also collapse the terminal-error and device-loss cases, the viewport and surface cases, the shader-layout rules (five places) and the queue-aliasing cases (six places).
- **Assertions:**
  - Replace assertions that restate the implementation with `static_assert`s, or delete them.
  - Keep native call-count assertions only where avoiding redundant calls is the contract.
- **Test public behaviour through `RHICommandList`.** Today there are 342 direct context calls. Raw-context tests remain only for backend-internal contracts.
- **Move engine tests that reach into internals into ZenRHI, on the public API.**
  - This covers the window and presentation cases (about 700 lines, mostly duplicates) and the presentation readback.
  - Rewrite SceneUploadBenchmark (723 lines) and the engine's `MockRHI` on the public API and FakeRHI.
- **Split `FakeRHI.h`** (1,849 lines: mutable anonymous-namespace globals, gtest fixtures, Win32 helpers) into a fake backend and separate fixtures.

**Documentation**
- **Cut the [contract README](../ZenRHI/include/ZenRHI/README.md) (66 KB) to caller-facing contracts.**
  - Backend implementation detail moves to code comments.
  - Engine-specific text moves to [RHIRenderCoreContracts.md](RHIRenderCoreContracts.md).
- **Update** the programming guide and the examples to the 0.3.0 API.
- **Mark superseded rules:**
  - the enhancement plan's §8 fence rule (if D1);
  - backlog 5.9 (if D2).

## 5. Expected size

These are audit estimates. Each phase replaces them with measured deltas.

| Phase | Library | Tests | Engine |
| --- | --- | --- | --- |
| 1 | −0.6k | small | none |
| 2 | −1.1k | −0.2k | about 10 files |
| 3 | −0.5k; about 1.5k moves out of public headers | renames | RDG, RenderDevice, ShaderProgram, RenderGraph, ZenApp |
| 4 | about 0 | global-swap rewrites | about 45 sites |
| 5a | −0.8k | small | none |
| 5b | −1.0k | small | one test include |
| 5c | −0.6k | fence runs halved | upload branch |
| 5d | −0.5k | small | RenderDevice, TextureManager |
| 5e | −0.5k | retention tests merged | −0.3k to −0.5k |
| 5f | −0.3k | small | none |
| 6 | — | −2.5k | about 0.7k moves into ZenRHI |

Target: library lines from 31.7k to about 25k, and test lines from 25.3k to about 21k.

## 6. Verification for each phase

- **Build.** Build Debug and Release through `tools/check_common.build`.
- **ZenRHI tests.** Run CTest `unit|policy` and the native Vulkan suite with validation required. `VulkanTestMain` must report no new validation errors.
- **Engine tests.** When a phase touches callers, run the engine suites that link ZenRHI.
- **ZenApp smoke runs.** Phases 2–5 need a ZenApp smoke run with validation in inline and threaded modes. Mock tests do not catch incomplete descriptor bindings or presentation failures.
- **API checker.** `check_public_api.py` lists the 0.3.0 removals, and the release commit names them.
- **Performance.** No FPS, `--profile` or Nsight runs unless requested.
- **RTSS.** Close it before native runs.

## 7. Delivery

- Work in the ZenRHI checkout on `main`, the branch `.gitmodules` tracks. Keep unfinished phases in a worktree selected with `ZEN_RHI_SOURCE_DIR`, because other sessions build from the shared checkout.
- Make one commit per item group. Push ZenRHI first, then ZenTest if it changed, then the ZenEngine commit that moves the pointer and updates callers.
- Phases 1 and 2 can land as several small pointer updates. Phase 3 lands with the shared v0.3.0 bump and tags across every Zen repository.

## 8. Not planned

| Idea | Reason |
| --- | --- |
| Automatic resource-state tracking (NVRHI) | The RDG owns transitions, and only it knows about async compute and aliasing. An optional tracker utility for hosts without a render graph can follow later. |
| NVRHI binding layouts and sets | Reflection is friendlier to outside users. |
| A validation wrapper device | The Khronos layer covers Vulkan usage. Cheap invariants stay inline. |
| Direct recording without the RHI worker | Needs a measurement (D3). |
| A second backend, device recreation | Non-goals in the [enhancement plan](ZenRHIEnhancementPlan.md#not-in-10). |

## Appendix A: dead code for Phase 1

Every item was grepped across ZenRHI `src`, `include`, `tests`, `test-support` and `examples`, and across ZenCore, ZenEditor, ZenApp, ZenUI, ZenSample, ZenTest and `tests`.

| Item | Location |
| --- | --- |
| `CopyFrom`, `CopyParameters` | [RHIShaderParameters.h:166](../ZenRHI/include/ZenRHI/RHIShaderParameters.h#L166) |
| `ColorClear` (one test caller), `DefaultColorClear`, `DepthStencilClear`, `DefaultDepthStencilClear` | [RHICommon.h:1166](../ZenRHI/include/ZenRHI/RHICommon.h#L1166) |
| `RHIBufferCopySource`, `RHIBufferTextureCopySource` | [RHICommon.h:871](../ZenRHI/include/ZenRHI/RHICommon.h#L871), [:1104](../ZenRHI/include/ZenRHI/RHICommon.h#L1104) |
| `WaitDeviceIdleChecked` | [DynamicRHI.h:257](../ZenRHI/include/ZenRHI/DynamicRHI.h#L257) |
| `DestroyAccelerationStructure` and its executor override | [DynamicRHI.h:204](../ZenRHI/include/ZenRHI/DynamicRHI.h#L204), [RHICommandListExecutor.cpp:1518](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1518) |
| `RHIResourceType::eDescriptorSet` | [RHIResource.h:31](../ZenRHI/include/ZenRHI/RHIResource.h#L31) |
| `NumFramesSince` | [RHIFrameState.h:59](../ZenRHI/include/ZenRHI/RHIFrameState.h#L59) |
| `RHIGetLastSubmittedSerial` and its Vulkan override | [RHICommandList.h:214](../ZenRHI/include/ZenRHI/RHICommandList.h#L214), `VulkanCommandList.h` |
| `AddWaitSemaphores`, `AddSignalSemaphores` | [VulkanCommandList.h:410](../ZenRHI/src/Vulkan/VulkanCommandList.h#L410), [:432](../ZenRHI/src/Vulkan/VulkanCommandList.h#L432) |
| `ZEN_VK_RHI_DEBUG` blocks | [VulkanCommandList.h:13](../ZenRHI/src/Vulkan/VulkanCommandList.h#L13) and `VulkanCommandList.cpp` |
| `VulkanCommandBufferType` (one value) | [VulkanCommandList.h:19](../ZenRHI/src/Vulkan/VulkanCommandList.h#L19) |
| `VkToString<VkImageUsageFlagBits>`, `VkToString<VkShaderStageFlagBits>` | [VulkanCommon.h:105](../ZenRHI/src/Vulkan/VulkanCommon.h#L105), [:314](../ZenRHI/src/Vulkan/VulkanCommon.h#L314) |
| `class VulkanCommandBuffer` forward declarations | `VulkanRHI.h`, `VulkanSynchronization.h`, `VulkanViewport.h` |
| `GetDescriptorSetLayoutData` | [VulkanPipeline.h:90](../ZenRHI/src/Vulkan/VulkanPipeline.h#L90) |
| `std::hash<VulkanDescriptorPoolKey>` | [VulkanPipeline.h:27](../ZenRHI/src/Vulkan/VulkanPipeline.h#L27) |
| `VersatileResourceTemplate::Alloc<T>` | [VulkanResourceAllocator.h:20](../ZenRHI/src/Vulkan/VulkanResourceAllocator.h#L20) |
| `GetVkSubresourceRange(mip, count, layer, count)` member overload | [VulkanTexture.h:45](../ZenRHI/src/Vulkan/VulkanTexture.h#L45) |
| `lastUsedFrame`, `m_currentFrame`, cache `BeginFrame`, `m_pDevice` | [VulkanDescriptorPool.h:184](../ZenRHI/src/Vulkan/VulkanDescriptorPool.h#L184), [:203](../ZenRHI/src/Vulkan/VulkanDescriptorPool.h#L203), [:216](../ZenRHI/src/Vulkan/VulkanDescriptorPool.h#L216) |
| `VulkanAPIVersion` | [VulkanExtension.h:34](../ZenRHI/src/Vulkan/VulkanExtension.h#L34) |
| `EnableMode`, `SetEnable`, `m_enabled` | [VulkanExtension.h:43](../ZenRHI/src/Vulkan/VulkanExtension.h#L43) |
| `struct VulkanSurface` | [VulkanPlatformCommon.h:8](../ZenRHI/src/Vulkan/Platform/VulkanPlatformCommon.h#L8) |
| `hasSPIRV_14`, `hasDynamicRendering` device flags (write-only) | [VulkanDevice.h:33](../ZenRHI/src/Vulkan/VulkanDevice.h#L33) |
| `m_physicalDeviceProperties` (duplicate of `m_gpuProps`) | [VulkanDevice.h:200](../ZenRHI/src/Vulkan/VulkanDevice.h#L200) |
| `SET_SIMPLE_EXTENSION_FLAG`, stray `#undef SET_EXTENSION_FLAG` | [VulkanExtension.cpp:817](../ZenRHI/src/Vulkan/VulkanExtension.cpp#L817), [:972](../ZenRHI/src/Vulkan/VulkanExtension.cpp#L972) |
| `VK_KHR_get_physical_device_properties2` flag, branch and `#ifdef` | [VulkanExtension.cpp:742](../ZenRHI/src/Vulkan/VulkanExtension.cpp#L742), [VulkanDevice.cpp:428](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L428) |
