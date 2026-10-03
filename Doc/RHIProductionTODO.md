# RHI production TODO

Status: in progress, 2026-10-03. This is the single list of work before the RHI can ship in a product. It merges the open items from section 9.2 of [RHIImprovementPlan.md](RHIImprovementPlan.md) with the gaps found by a static review of `Graphics/RHI`, `Graphics/VulkanRHI` and their `RenderCore/V2` callers at `ee20bf99`. The review items (R17–R24 and the extensions to R10 and R13) originated from reading code; completed items link their implementation and verification records below. Problem descriptions and file references describe `ee20bf99` unless stated otherwise.

Scope: Windows desktop Vulkan, the only platform the [RHI README](../ZenCore/Include/Graphics/RHI/README.md) supports. Item IDs continue section 8 of the improvement plan; carried items keep their original IDs so existing verification documents still match.

## 1. Rules

Section 5.0 of the improvement plan applies to every item:

1. Measure first when an item has a gate.
2. Keep a change only if it helps or is neutral and adds no validation or synchronization-validation errors.
3. Preserve the improvement plan's section 3 invariants and the README contracts. Update the README for every contract change.
4. Validate as in the improvement plan's section 6: both RHI thread modes, and both the timeline and fence submission paths when submission changes.
5. Record results in a verification document per item (`RHIProductionR<n>Verification.md`).

Items that change failure behavior follow section 2 of [RHIErrorHandlingPlan.md](RHIErrorHandlingPlan.md). Modified code follows the repository C++ rules and `.clang-format`.

The engine does not use exceptions. A broken invariant or a missing required engine asset writes an error message and aborts (`VERIFY_EXPR_MSG_F`). Invalid or unsupported input data, such as a glTF file the user picks, returns a status so the caller can keep running. Engine-owned C++, including tests, has no `try`, `catch`, `throw`, or `LOG_ERROR_AND_THROW`; `tools/check_no_exceptions.py` enforces the source policy.

## 2. Summary

| ID | Priority | Item | Size | Source | Gate or status |
| --- | --- | --- | --- | --- | --- |
| R17 | Blocker | Recoverable failures stop rendering permanently | M–L | Review | Implemented: option B; [verification](RHIProductionR17Verification.md) |
| R18 | Blocker | Shader reflection leaks and crashes on bad SPIR-V | S | Review | Implemented; [verification](RHIProductionR18Verification.md) |
| R19 | Blocker | Shader files load from a compile-time source path | S | Review | None |
| R20 | Blocker | Validation is enabled by default in Release | S | Review | None |
| R21 | Blocker | Startup failure crashes without a message | S | Review | Implemented; [verification](RHIProductionR21Verification.md) |
| R7 | Blocker | Device-loss diagnostics: manual acceptance | S | Improvement plan 9.2 | Needs a dedicated test machine |
| R8 | Blocker | Hardware and CI acceptance | M | Improvement plan 9.2 | Needs NVIDIA and Intel hardware |
| 5.10 | Important | Remaining error-handling work | L | Improvement plan 9.2 | Implemented with R17; [verification](RHIErrorHandlingVerification.md) |
| R22 | Important | Fixed bindless heap capacity | M | Review | Done: `8b59a3d0`, 2026-10-03 ([verification](RHIProductionR22Verification.md)) |
| R23 | Important | Releases after teardown are not detected | S | Review | Done 2026-10-03 ([verification](RHIProductionR23Verification.md)) |
| R24 | Important | RHI stall watchdog | S–M | Review | Design in [RHIStallWatchdogPlan.md](RHIStallWatchdogPlan.md) |
| R10 | Gated | Synchronous pipeline compilation | M–L | Improvement plan 9.2, extended | Cold-cache and resize spikes |
| R11 | Gated | Single-threaded recording and translation | L | Improvement plan 9.2 | A heavier scene |
| R13 | Gated | Missing API features | Per feature | Improvement plan 9.2, extended | A consumer |
| R14 | Gated | Presentation requires the graphics queue | M | Improvement plan 9.2 | A platform that needs it |
| 5.3 | Gated | Synchronization2 | M | Improvement plan 9.2 | Render-graph Phase 4 |
| 5.9 | Deferred | Global singletons | L | Improvement plan 9.2 | A concrete need |
| 5.1 B, 5.2, 5.4 | Deferred | Presentation copy removal, one submit per frame, per-frame pools | — | Improvement plan 9.2 | Gates closed |
| 5.5 | RenderCore | Per-frame descriptor miss | S–M | Improvement plan 9.2 | Done 2026-10-03 ([verification](RenderCoreImprovementVerification.md)) |
| H1 | Housekeeping | Ignored `FinalizeCommandLists` results in tests | S | Improvement plan 9.2 | Checked during error-handling migration |

R17, R18, R21, R22, R23 and 5.10 are done; 5.5 was fixed in RenderCore afterwards ([verification](RenderCoreImprovementVerification.md)). On the final tree of the R-items, clean Debug and Release builds pass all four acceptance suites (CommonTest 112, RenderCoreTest 557, VulkanRHITest 48, VulkanRHIIntegrationTest 336 in Debug and 337 in Release, with 6 capability skips), every other test executable and all 24 smoke runs, with no validation errors. Captures for all three rendering modes are byte-identical to the R22 baseline. Hardware coverage remains limited to the RX 7900 XT; see the per-item verification records for results and capability skips.

The RHI is ready for a shipped Windows desktop product when the remaining Blocker items (R19, R20, R7 and R8) are done and R24 is done or accepted with a recorded reason. The Gated and Deferred items decide whether the RHI can serve as a general-purpose engine RHI. They do not block a product whose content fits the current limits.

## 3. New items from the review

### R17. Recoverable failures stop rendering permanently

**Status.** Implemented 2026-10-03: option B. Required-frame rejection is terminal in every execution mode, with one primary cause diagnostic and ordered cleanup; optional standalone rejection remains local. Automatic recovery/device recreation is deferred. [Verification](RHIProductionR17Verification.md).

The problem description below records the original review.

**Problem.**

- `RenderDevice::ExecuteRenderGraph` ([RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp)) submits frames through `RHICommandListExecutor::SubmitFrame` whenever the RHI thread or async compute is enabled. Frame batches set `endFrame`, and `ExecuteFrame` ([RHICommandListExecutor.cpp](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp)) blocks the executor after any frame that does not succeed, including `eRejected`. Only inline execution with async compute off keeps a rejected frame retryable.
- Errors that R2, R4 and R12 made recoverable therefore end rendering. Each of these latches a recording error, `FinalizeCommandLists` fails, and the frame is rejected:
  - uniform-ring growth or mapping failure (`VulkanDescriptorSetState::FlushPackedValueBuffers`);
  - descriptor-pool creation failure;
  - bindless registration failure, including a full heap (R22).
- Two other paths also block the device:
  - a failed swapchain rebuild in `VulkanViewport::FinishResize`;
  - a rejected submission through `VulkanCommandContextBase::SubmitRecordedWorkloads`, which `VulkanViewport::CreateSwapchain` uses to initialize backbuffers.
- Nothing restores rendering. There is no executor reset or device recreation, and the demo leaves its main loop once `AreSubmissionsBlocked()` is true.
- The README's GPU-budget section says allocation failure remains recoverable. That holds only on the inline path without async compute.
- `RHIExecutorTest.RejectionStopsAlreadyQueuedDependentBatches` asserts the current behavior. It matches the error-handling plan's first-delivery policy (sections 5 and 9), which no plan has yet scheduled to replace.

**Decision.** Option B is selected and implemented. The alternatives considered were:

| Option | Behavior | Cost |
| --- | --- | --- |
| A. Skip the frame | Discard the batch, invalidate the frame's speculative `RenderSubmissionHistory`, cancel already-queued batches that depend on it, and let RenderCore record the next frame. | M. RenderCore must rebuild submission history and resource state for cancelled frames. |
| B. Terminal with a clean exit | Keep the executor blocked, report the cause through R21's path, and exit. | S |

Device loss stays terminal under both options. Device recreation, which rebuilds the backend and reloads every resource, is a separate L-sized project. Record whether the product needs it.

**Done when** the decision is recorded in the README, and:

- **With option A:** injected uniform-growth, descriptor-pool and bindless-registration failures, in threaded mode with async compute on and off, drop only the affected frames. Rendering then resumes, and no resource is retired early. A failed swapchain rebuild is retried on the next resize instead of blocking the device.
- **With option B:** each of these failures produces one diagnostic and a clean exit.
- **With either option:** the README's GPU-budget section and R12 describe the actual behavior.

### R18. Shader reflection leaks and crashes on bad SPIR-V

**Status.** Implemented 2026-10-03. Reflection cleanup, malformed-input rejection and specialization input reuse are in place. [Verification and memory-measurement scope](RHIProductionR18Verification.md).

The problem description below records the original review.

**Problem.**

- `RHIShaderUtil::ReflectShaderGroupInfo` ([RHIShaderUtil.h](../ZenCore/Include/Graphics/RHI/RHIShaderUtil.h)) calls `spvReflectCreateShaderModule` for every stage but never `spvReflectDestroyShaderModule`; only `ShaderReflectionTests.cpp` frees its module. Every shader creation leaks its reflection data.
- `RenderDevice::GetOrCreateGfxPipeline` creates a new specialized shader on every pipeline-cache miss that has specialization constants. It reloads the SPIR-V files from disk and reflects them again. `InvalidateRDGPassCompilerForResize` clears the whole pipeline cache on each resize, so the leak grows with every resize.
- When `spvReflectCreateShaderModule` fails, the failure is logged and the uninitialized module is used; the next `VERIFY_EXPR` aborts.
- `ParseSpvPushConstants` and `ParseSpvSpecializationConstant` throw on more than one push-constant block or an unsupported specialization-constant type. `VulkanShader::CreateObject` ([VulkanPipeline.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanPipeline.cpp)) doesn't catch, so the half-built shader is never released and the exception reaches RenderCore, which doesn't catch either. The README promises that shader creation returns null.
- An unsupported descriptor type (dynamic uniform or storage buffer, acceleration structure) is logged and left as `RHIShaderResourceType::eMax`. `VulkanShader::Init` then uses that value as an index into `m_SRDCount`, one past its end.

**Design.**

- Free every reflection module on all paths, and make reflection return a result.
- Reject unsupported input with a logged error and a null shader.
- Build specialized shaders from the base shader's SPIR-V and reflection, so a cache miss reads no files.

**Done when:**

- Creating and destroying a shader repeatedly leaves process memory flat.
- Each of these makes `CreateShader` return null without leaking the RHI object: corrupt SPIR-V, two push-constant blocks, a 64-bit specialization constant, and an unsupported descriptor type.
- Creating a specialized pipeline reads no files.

### R19. Shader files load from a compile-time source path

**Problem.** `SPV_SHADER_PATH` is the absolute path of the source tree's `Data/SpvShaders/`, defined in [ZenCore/CMakeLists.txt](../ZenCore/CMakeLists.txt) and used by `platform::FileSystem::LoadSpvFile`. A build copied or installed to another directory or machine cannot find its shaders, so every shader and pipeline fails to create.

**Done when:**

- The data root is resolved at run time: next to the executable, or from a setting or command-line option.
- The compiled path remains only as a development fallback.
- An install or package step copies `Data/SpvShaders`.
- A build run from a different directory renders.

Check the other `Data/` paths in the same change.

### R20. Validation is enabled by default in Release

**Problem.** `RHIOptions::m_validationEnabled` ([RHIOptions.h](../ZenCore/Include/Graphics/RHI/RHIOptions.h)) defaults to true in every build configuration. Only the demo's `--disable-validation` turns it off. A Release build on a machine with the Vulkan SDK installed therefore runs with the Khronos validation layer and its CPU cost.

**Done when:**

- Validation defaults to off in `NDEBUG` builds and on in Debug.
- An option or command-line flag enables it explicitly.
- Tests and smoke runs that rely on validation enable it explicitly.
- A Release run enables no validation layer unless one is requested.

### R21. Startup failure crashes without a message

**Status.** Implemented 2026-10-03. Startup failures use release-active diagnostics/abort, including unsupported API/device profiles. The interactive Windows demo installs a native message reporter. [Verification](RHIProductionR21Verification.md).

The problem description below records the original review.

**Problem.**

- Backend initialization reports failure by throwing. That covers volk initialization (no Vulkan loader), `vkCreateInstance`, `VulkanRHI::SelectGPU` (no device meets the requirements), `VulkanDevice::Init` and `vkCreateDevice`.
- Neither `RenderDevice::Init` nor the demo's `main` catches the exception, so a user with an unsupported GPU or driver sees a crash.
- `DynamicRHI::Create` ([RHIFactory.cpp](../ZenCore/Source/Graphics/RHI/RHIFactory.cpp)) also dereferences null for an unsupported API type.

This is step 5 of the error-handling plan's terminal response ("let the application report failure and close cleanly"), which no item tracked.

**Done when:**

- Backend initialization failures, including an unsupported API type in `DynamicRHI::Create`, write the rejection reason and abort instead of throwing.
- On Windows the reason also appears in a message box, because a windowed application has no visible console.
- A death test covers a forced device-selection failure.

### R22. Fixed bindless heap capacity

**Status.** Implemented, 2026-10-03, in commit `8b59a3d0` ([verification](RHIProductionR22Verification.md)). The problem and done-when text below describe the state at `ee20bf99`.

**Problem.**

- The global heap holds 2048 2D textures, 64 cube textures and 128 samplers (`kBindlessHeapCapacity` in [VulkanDescriptorPool.h](../ZenCore/Include/Graphics/VulkanRHI/VulkanDescriptorPool.h)).
- These sizes are compile-time constants that nothing exposes, and RenderCore never checks them.
- A scene that needs more slots fails during command translation, which R17 currently turns into a permanent stop.
- Only 2D and cube sampled images and samplers have heaps.

**Done when:**

- The capacities are queryable, for example in `RHIGPUInfo`.
- The capacities can be configured before initialization. Initialization checks them against the device limits that `VulkanDevice::GetUnsupportedReason` checks today.
- RenderCore checks a scene's texture and sampler count at load time. It either rejects the load with a message or uses a defined fallback.
- A test loads a scene above the default capacity.

Add storage-image, buffer or 3D heaps only when an R13 consumer needs them.

### R23. Releases after teardown are not detected

**Status.** Implemented and validated, 2026-10-03 ([verification](RHIProductionR23Verification.md)). The problem and done-when text below describe the state at `ee20bf99`.

**Problem.**

- After `RHIThread::Stop`, the thread runs every task inline because `m_threaded` is false. `Enqueue` therefore accepts cleanup after admission has closed.
- As a result, the "released after RHI cleanup admission closed" checks in `RHIResource::ReleaseReference` and `IRHICommandContext::OnFinalRelease` never fire. Inline mode never closes admission at all.
- A late release runs `Destroy` against the deleted backend. `GVulkanRHI` is never reset after the backend is deleted.
- With non-strict teardown checks, the Release default, a resource still alive at backend teardown is only logged. Releasing it later is a use-after-free at exit.

The README calls a late release an ownership violation, but nothing detects one.

**Done when:**

- Admission stays closed from the backend finalizer until a new backend or executor starts, in both execution modes. Standalone raw-backend tests keep working.
- A late release reports the violation through `VerifyTeardownOwnership` without touching the backend: fatal under strict checks, logged and leaked otherwise.
- `GVulkanRHI` and `GDynamicRHI` are cleared on destruction.
- A test releases a resource after executor destruction, in both modes.

### R24. RHI stall watchdog

**Problem.** Every wait on the RHI thread or its results is unbounded, including the swapchain's acquire and presentation fence waits and every `UINT64_MAX` queue wait. A task that never returns hangs the application without a diagnostic.

[RHIStallWatchdogPlan.md](RHIStallWatchdogPlan.md) (proposed 2026-10-01, not committed) designs report-only stall detection with an optional fatal threshold, plus a stack capture of the RHI thread.

**Done when** phases 1 and 2 of that plan pass its validation section, and the plan document is committed.

## 4. Items carried from the improvement plan

### R7. Device-loss diagnostics: manual acceptance

**Status.** Implemented, acceptance open ([verification](RHIProductionR7Verification.md)). Diagnostics are opt-in through `RHIOptions::SetDeviceLossDiagnostics` or `--device-loss-diagnostics`.

**Remaining work.** Run a deliberate device-loss reproduction on a dedicated test machine with diagnostics enabled. Keep the stderr output and the driver report. Simulated failures and successful marker tests do not close this item.

**Done when** the reproduction reports the faulting queue and pass, and frame time with breadcrumbs enabled is neutral under the section 5.0 measurement rules.

### R8. Hardware and CI acceptance

**Status.** Tooling implemented, acceptance open ([verification](RHIProductionR8Verification.md)). Only a Radeon RX 7900 XT under Windows 10 has run the production changes. No NVIDIA or Intel machine is currently available. The README declares macOS, Linux and mobile unsupported.

**Remaining work.**

- Run `tools/verify_rhi_production.py` in Debug and Release on an NVIDIA GPU and an Intel GPU, with synchronization validation. It covers both RHI suites, `RenderCoreTest` and the section 6 smoke matrix.
- Cover the paths the RX 7900 XT skips: swapchain-maintenance presentation fences (4 tests), the EXT surface-maintenance instance dependency (1 test) and the D24S8 depth fallback (1 test).
- Run the SwiftShader workflow ([rhi-software.yml](../.github/workflows/rhi-software.yml)) for the first time. Neither the workflow nor its cache (F11) has ever run.
- Repeat the hardware runs after R17, R18 and the 5.10 remainder land.

**Done when** the results are recorded in the R8 verification document. macOS stays unsupported unless it becomes a target.

### 5.10. Remaining error-handling work

**Status.** Implemented 2026-10-03, building on R1–R6 ([initial verification](RHIProductionVerification.md)). Structured causes, typed WSI/admission/wait/progress results, explicit worker cancellation and the no-exception source policy are implemented with R17 option B. Pointer/bool compatibility adapters remain explicit failure contracts. See [current verification](RHIErrorHandlingVerification.md) for measured coverage and hardware limits.

**Original remaining-work inventory (now implemented).**

- Carry structured errors through the executor and `RHIBatchResult`, and add typed acquire, present and job-admission results (sections 5 and 6 of the error-handling plan).
- Replace the remaining RHI-owned throws with an error message and an abort (section 1), or with a status where the existing contract already returns one. At `ee20bf99`, `Graphics/RHI` and `Graphics/VulkanRHI` still contain 39 `LOG_ERROR_AND_THROW` sites:

| File | Sites | Paths |
| --- | ---: | --- |
| `VulkanSwapchain.cpp` | 15 | Surface queries, acquire, present, fence waits |
| `VulkanContext.cpp` | 6 | Initialization (R21) |
| `RHIResource.h` | 4 | `RHIRenderingLayout` setters, which throw on the render thread while recording |
| `RHIShaderUtil.h` | 3 | Reflection (R18) |
| `VulkanViewport.cpp` | 2 | `PrepareForPresent`, `Present` |
| `VulkanSynchronization.cpp` | 2 | Semaphore creation and destruction |
| `VulkanDevice.cpp` | 2 | Device initialization (R21) |
| `RHIThread.cpp` | 2 | Event creation, Windows wait failure |
| `VulkanExtension.cpp` | 1 | Missing required instance extension (R21) |
| `VulkanWindowsPlatform.cpp`, `VulkanMacOSPlatform.cpp` | 2 | Surface creation |

**Done when** the acceptance list in section 8 of the error-handling plan holds and R17's decision is implemented. Schedule this together with R17, and before or after 5.9, never interleaved with it.

### R10. Synchronous on-demand pipeline compilation

**Status.** Gate not yet measured ([verification](RHIProductionR10Verification.md)).

**Problem.** On a cache miss, `RenderDevice::GetOrCreateGfxPipeline` and `GetOrCreateComputePipeline` create the pipeline synchronously while recording. There is no asynchronous compilation and no warm-up list. The review adds two more sources of spikes:

- `InvalidateRDGPassCompilerForResize` destroys every cached pipeline on each resize, even though RenderCore pipelines use a dynamic viewport and scissor.
- Specialized pipelines reload and reflect their shader files on every miss (R18).

**Gate.** With a cold driver cache, measure per-frame `creationCPUUs` and frame-time spikes in these cases:

- the first frames of Sponza and the glTF smoke assets;
- after runtime GI reconfiguration;
- on window resize.

Proceed only if the spikes are visible.

**Design outline.**

1. Keep pipelines whose keys do not change on resize.
2. Then either compile on worker threads and skip or substitute the draw until the pipeline is ready, or record the pipeline keys a run uses and create them at load time.

### R11. Single-threaded recording and translation

**Status.** Gate not open ([verification](RHIProductionR11Verification.md)).

**Problem.** One render thread records every command list and one RHI thread translates them. Nothing records in parallel, and there are no secondary command buffers. This is adequate for the current scenes but limits CPU throughput for heavier ones.

**Gate.** Profile a heavier scene in which recording or `rhi_execution_ms` dominates the CPU frame before changing recording ownership or concurrency.

### R13. Missing API features

**Status.** Waiting for consumers ([verification](RHIProductionR13Verification.md)). Add each feature only when a consumer needs it, with a test and a README entry.

| Feature | Current state |
| --- | --- |
| Non-indexed indirect draws | Only `DrawIndexedIndirect` and `DispatchIndirect` exist. |
| Indirect-count draws | No `vkCmdDraw*IndirectCount` command. |
| Queries | Only internal timestamp timing. No occlusion or pipeline-statistics queries. |
| Stencil reference | No command, and it is not a dynamic state. |
| Mesh shaders | Not supported. |
| Ray tracing | Acceleration-structure and ray-query extensions are enabled when available, but there is no acceleration-structure resource type or build command. See [HardwareRayQueryEnvironmentLightingPlan.md](HardwareRayQueryEnvironmentLightingPlan.md). |
| HDR output | Swapchain selection accepts only 8-bit formats in `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`. |
| Synchronization2 | See 5.3. |
| Depth/stencil clear outside rendering (review) | `RHIClearTexture` records only `vkCmdClearColorImage`. |
| MSAA resolve (review) | `RHIResolveTexture` resolves color only. Dynamic rendering has no resolve attachments. |
| Integer-format clears (review) | Clear values are float only. |
| Vertex input (review) | One per-vertex binding, with tightly packed offsets derived from shader reflection. No per-instance streams or explicit strides. |
| Storage-buffer ranges (review) | Storage buffers bind their whole size. Only uniform buffers accept an offset. |
| Read-only depth (review) | No read-only depth/stencil layout, so a pass cannot depth-test against a depth image while sampling it. |
| Multiple viewports (review) | A frame submission presents one viewport. No multi-window presentation. |
| Adapter selection (review) | The GPU is chosen by score, with no override option. |
| Further bindless heaps (review) | Only 2D, cube and sampler heaps; R22 made their sizes configurable. |

### R14. Presentation requires the graphics queue

**Status.** Waiting for a platform that needs it ([verification](RHIProductionR14Verification.md)).

**Problem.** Presentation fails if the selected graphics queue cannot present to the surface; separate presentation queues are not implemented. Desktop drivers expose graphics queues that can present, so this matters only on unusual platforms.

**Gate.** Implement a separate presentation queue only for a supported platform that needs one.

### 5.3. Synchronization2

**Status.** Not started, by decision.

**Problem.** `VulkanPipelineBarrier` issues `vkCmdPipelineBarrier` with one source and one destination stage mask for every barrier in a call. `RHIPipelineStageFlagBits` is a 32-bit enum of Vulkan 1.0 stages.

**Gate.** Start together with render-graph Phase 4, or when another consumer needs per-barrier stages. The change must be performance-neutral.

**Done when** the conditions in section 5.3 of the improvement plan hold: captures are byte-identical, the full suites and smoke matrix pass with synchronization validation, and GPU pass medians stay within run-to-run variation. It requires `VK_KHR_synchronization2` or Vulkan 1.3.

### 5.9. Global singletons

**Status.** Not started, by decision.

**Gate.** A concrete need: several devices, tests that run in parallel in one process, or a second backend. R23 clears the globals at destruction but does not remove them. Schedule this before or after R17 and the 5.10 remainder, never interleaved with them.

### 5.1 option B, 5.2 and 5.4

**Status.** Measured; gates closed.

- 5.1 B: the presentation copy costs about 8 µs of GPU time.
- 5.2: a `vkQueueSubmit` costs 16–18 µs, and steady frames have one submission group.
- 5.4: command-buffer setup costs about 10–15 µs per frame.

Revisit these items only if the workload changes, for example if steady frames start having several submission groups.

### 5.5. Per-frame descriptor miss

**Status.** Done 2026-10-03 through Phases 0–2 of [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md); [verification](RenderCoreImprovementVerification.md). Steady-state frames record zero descriptor-cache misses and zero render-graph pool misses at 720p, 1440p and the former 4096² stress size.

**Problem (original).** The deferred-lighting descriptor set missed the cache every frame. The render graph's 256 MiB transient-pool budget was smaller than two frame slots of the default 2048² G-buffer, so each trim evicted `offscreen_albedo` and `offscreen_roughness`.

**Fix.** The frame graph keeps every allocation used by its last `numFrames` builds pooled regardless of the budget (`RDGPoolConfig::steadyBuilds`), and the G-buffer now matches the viewport and is read with `texelFetch`. `--gbuffer-size` was removed.

**Done when** steady-state frames record zero descriptor-cache misses.

### H1. Ignored `FinalizeCommandLists` results in tests

**Status.** Fixed during the 2026-10-03 error-handling migration; the original locations below are retained as review context.

[VulkanSwapchainIntegrationTests.cpp](../ZenSamples/CommonTest/VulkanSwapchainIntegrationTests.cpp) ignores the `[[nodiscard]]` `RHIStatus` returned by `FinalizeCommandLists` at lines 715 and 1095. These were lines 499 and 766 before `ee20bf99` reformatted the file. Check the result in both tests. The other housekeeping item in the improvement plan, committing the production changes, was done in `45973b11`.

## 5. Order

1. **R19 and R20.** R18 and R21 are implemented; packaging and default validation remain independent blockers.
2. **R17 and 5.10 are implemented.** Preserve their error/lifetime contracts when scheduling 5.9; do not interleave a singleton redesign with further executor changes.
3. **R24.** R22 and R23 are done.
4. **R7 and R8** whenever hardware is available. Repeat R8 after step 2.
5. **Gated items** when their gates open: R10 (measure resize first), R11, R13, R14 and 5.3. 5.5 is done.
