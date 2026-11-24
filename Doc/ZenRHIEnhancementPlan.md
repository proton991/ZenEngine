# ZenRHI production enhancement plan

The [implementation and verification record](ZenRHIEnhancementVerification.md) contains the hardware results, SDK checks, 4K experiments and outstanding real-device-loss qualification.

Proposed on 2026-11-23 for ZenRHI `7b8ccd145188` in ZenEngine `5309ec20`. Revised the same day after checking each claim against the code. The scope widened from the next four steps to a 1.0 production release.

**First make the evidence trustworthy and failures survivable. Then let measurements choose the performance work.** A validation run must prove that the layer observed the workload. Device creation must report failure to an embedding application instead of aborting it. Hangs and device loss must name the operation involved. Pipeline compilation, resource creation, command translation and submission all run on the single RHI worker today. Performance work therefore starts by measuring how that worker spends its time.

| Order | Deliverable | Class | Evidence that closes it |
| --- | --- | --- | --- |
| 1 | [Validation that proves it ran](#2-make-validation-evidence-trustworthy) | Blocker | A missing layer fails a required run. The hazard probe reports its write-after-write hazard and accepts the corrected case. Each change runs native integration tests. |
| 2 | [Checked device creation and an adapter report](#3-let-an-embedding-application-survive-startup-failure) | Blocker for SDK consumers | An external executable reports a startup error, keeps running, and later creates a device in the same process. |
| 3 | [Diagnosable hangs and device loss](#4-make-hangs-and-device-loss-diagnosable) (R24, R7) | Blocker | A blocked task is reported and later completes. A real device loss reports the faulting queue and pass. |
| 4 | [RHI worker time breakdown](#5-measure-the-rhi-worker-before-changing-it) | Measurement | ZenApp traces divide worker time by category for startup, first GI entry, steady state and scene load. |
| 5 | [Work chosen by that breakdown](#51-persistent-pipeline-cache): pipeline cache, off-worker compilation, off-worker creation, parallel translation | Gated by step 4 | Each change improves its target workload beyond run-to-run variation and leaves output unchanged. |
| 6 | [Features requested by consumers](#6-add-features-when-a-consumer-needs-them) | Gated per consumer | Each feature has a consumer, a capability flag, a test and a README entry. |
| 7 | [Release engineering for 1.0](#7-release-engineering-for-10) | Before the 1.0 tag | The API policy, guide, examples, hardening jobs and reconciled backlog are in place. |

The [backend comparison](RHIBackendComparison.md) records the NVRHI, UE5 and Leonida revisions cited here. The NVRHI and UE5 links point at those revisions. The [production backlog](RHIProductionTODO.md) keeps the original item identifiers; [section 7](#7-release-engineering-for-10) reconciles it with this plan.

## 1. What production means for ZenRHI

### Consumers

| Consumer | Needs beyond today |
| --- | --- |
| ZenApp and ZenEngine on Windows desktop | Hardware acceptance across GPU vendors, no first-use pipeline hitch on GI entry, and diagnosable hangs and device loss |
| Editor, asset tools and plug-ins built on the installed SDK | A startup failure that leaves the host running, adapter information before device creation, and a documented thread model |
| External applications | A versioned API, a programming guide, examples beyond Triangle, and documented thread safety for every entry point |

### Support matrix

Target Windows 10 and 11 on x64, built with MSVC. Require Vulkan 1.2 with the device profile listed in the [README](../ZenRHI/include/ZenRHI/README.md#vulkan-device-and-presentation-requirements). For each vendor, record the driver versions that were tested rather than promising minimum versions. [API version](../ZenRHI/src/Vulkan/VulkanRHI.h#L20)

Decide before 1.0 whether to raise the baseline to Vulkan 1.3. Synchronization2 and dynamic rendering are core in 1.3. The barrier work in [section 6](#6-add-features-when-a-consumer-needs-them) could then replace the legacy emitter instead of keeping two. Current drivers for the GPU generations their vendors still support expose 1.3. The cost is dropping devices whose drivers stopped at 1.2.

### 1.0 exit criteria

1. Every validation-enabled acceptance run proves the layer was active and passes the hazard probe.
2. Checked device creation and the adapter report ship in the installed SDK. The relocated-consumer and Triangle checks pass against it.
3. The release candidate passes native acceptance on NVIDIA, AMD and Intel. Every change runs the native integration suite on a software driver, or the plan records why the driver cannot run it.
4. R24 stall reports and R7 device-loss acceptance are complete.
5. First-use pipeline cost has a measured decision.
6. The public API has a versioning and deprecation policy, a programming guide and the examples listed in section 7.
7. The production backlog is reconciled with this plan and has no open blocker.

### Not in 1.0

Each item stays out until its trigger occurs.

| Non-goal | Reopen when | Note |
| --- | --- | --- |
| A second backend, such as D3D12 or Metal | A target platform or a required feature is unavailable through Vulkan | NVRHI and UE5 support several backends. New public types should stay backend-neutral so a second backend remains possible. Existing format and stage enums carry Vulkan numeric values, which another backend would translate. |
| Device recreation after loss | An editor or tool must keep running after a GPU timeout | Device loss stays terminal and reported. UE5’s Vulkan RHI also terminates on a GPU crash. Same-process retry in section 3 is the prerequisite. [UE GPU-crash handling](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanUtil.cpp#L459) |
| Several devices in one process | Multi-GPU work, parallel in-process tests, or a host that loads its own Vulkan dispatch | `RHIOptions`, `GDynamicRHI`, `GVulkanRHI` and volk’s function table are process-wide (backlog item 5.9). [volk device load](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L577) |
| Linux, macOS and mobile | A product targets them | The README already declares them unsupported. |

## 2. Make validation evidence trustworthy

In an earlier RTX 5080 probe, `VulkanPipelineIntegrationTest.SpecializedVoxelWorkgroupsClearAnEntireNonAlignedVolume` passed normally. It still passed with the Khronos layer hidden. ZenRHI logged that the requested layer was unavailable. The acceptance runner reported exit code zero, one passing test and no validation errors. The code explains that result:

- The runner requests synchronization validation through an environment variable and scans the output for `VUID-` and `SYNC-HAZARD`.
- The test entry point enables validation but does not require it.
- The debug callback logs messages without counting them.

Absence of errors is therefore the only evidence of a clean run. [Layer selection](../ZenRHI/src/Vulkan/VulkanContext.cpp#L455) · [Debug callback](../ZenRHI/src/Vulkan/VulkanContext.cpp#L344) · [Test entry point](../ZenRHI/tests/VulkanTestMain.cpp) · [Acceptance runner](../tools/verify_rhi_production.py#L36)

Make validation produce positive evidence:

- **Required mode.** Add a validation mode with off, requested and required values. In required mode, a missing layer is a device-creation failure, which the checked path in section 3 returns. Requested mode keeps today’s warning for ordinary startup.
- **Programmatic settings.** Configure synchronization validation and message limits with `VK_EXT_layer_settings` at instance creation. Environment variable names have changed between SDK releases. The environment remains an override for manual runs. [Layer settings extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_layer_settings.html)
- **Counted messages.** Count callback messages by severity and message ID on the device. Expose the counts through `ZenRHITestSupport`. A test fails on any unexpected error-severity message.
- **Active-layer record.** Log one line with the layer’s name, specification and implementation versions, and enabled settings. The runner requires that line from every validation-enabled suite and smoke run.
- **Hazard probe.** Use ZenRHI’s own instance setup and callback. Record two overlapping buffer clears without a transition and require `SYNC-HAZARD-WRITE-AFTER-WRITE`. Add the transfer-to-transfer transition and require zero messages. Only the expected diagnostic in the deliberate case counts as success. Leonida’s equivalent probe reported one hazard and zero errors in the corrected case on the same GPU. [Probe evidence](RHIBackendComparison.md#priorities-for-zenrhi)

**Native coverage on each change.** ZenRHI’s CI runs only `unit|policy` tests on a runner without a GPU. ZenEngine’s software-driver workflow calls the acceptance runner with `--unit-only`. No native Vulkan test runs for each change. Run `ZenRHIVulkanIntegrationTests` in the software-driver job with capability-based skips, and make the hazard probe a required test. Record each skipped test and its reason. If the software driver cannot satisfy the device profile, record that and keep the job at unit level. Do not lower the profile to fit CI. [ZenRHI CI](../ZenRHI/.github/workflows/windows.yml) · [Software-driver CI](../.github/workflows/rhi-software.yml#L80)

**Hardware matrix for each release candidate.** Run Debug and Release on NVIDIA, AMD and Intel. Record the library revision, device, driver, layer version, enabled validation settings, passes and capability-based skips. The R8 record covers AMD and NVIDIA and leaves Intel open. Report which hardware actually ran. [Hardware acceptance record](RHIProductionTODO.md#r8-hardware-and-ci-acceptance)

## 3. Let an embedding application survive startup failure

`CreateRHI` calls the Vulkan factory, which initializes the backend before returning it. Loader initialization, instance creation and GPU selection all use fatal checks that stay active in Release. ZenApp gets a clear startup diagnostic from that policy. An editor plug-in or asset tool using the installed library instead needs to disable its GPU feature and keep running. During library-split verification, Vulkan enumerated no hardware GPU on the first launch after a driver reset. [Vulkan factory](../ZenRHI/src/Vulkan/VulkanRHIFactory.cpp#L7) · [GPU selection](../ZenRHI/src/Vulkan/VulkanContext.cpp#L619) · [Recorded launch](LibrarySplitVerification.md#L71)

**Checked creation.** Add an entry point that returns the existing `RHIResult<DynamicRHI*>` type. The combined factory and the Vulkan factory should share one checked initialization and cleanup path. Existing entry points keep their fatal startup contract. [Result type](../ZenRHI/include/ZenRHI/RHIError.h#L130) · [Existing startup contract](RHIProductionR21Verification.md)

- **Error contents.** Convert environmental failures into errors that carry the failing operation and, where available, the native result. Per-adapter rejection details go to the diagnostic callback.
- **Coverage.** Cover loader and allocator setup in the constructor, plus instance, device, queue and allocator initialization.
- **Cleanup.** Undo completed steps in reverse order.
- **Process-wide state.** The factory sets the message callback and the `RHIOptions` singleton before initialization, and it sets `GDynamicRHI` afterward. A failure must restore the previous callback and options and leave `GDynamicRHI` null. A retry reloads volk’s instance and device tables.
- **Executor.** Construct the executor only after device creation succeeds. [Executor construction](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L218)

**Adapter report.** Add an enumeration that runs without creating a logical device. For each physical device, it reports the name, vendor, driver and API versions, and the first requirement the device fails. Base the report on the same capability checks that selection uses, so the two cannot disagree. Launchers and settings screens can use it to explain an unsupported GPU and to offer the `SetPreferredDevice` choice. [Preferred-device option](../ZenRHI/include/ZenRHI/RHIOptions.h#L86)

**Acceptance.**

- **Unsupported profile.** Use the Khronos profiles layer to hide a required feature, such as dynamic rendering, on real hardware. That makes rejection deterministic on every machine. [Vulkan profiles](https://github.com/KhronosGroup/Vulkan-Profiles)
- **Retry.** Inject failures after instance creation and after device creation. The checked caller receives the cause, stays alive, and can then create a device in the same process.
- **Cleanup.** Partial native objects are destroyed exactly once. Borrowed device handles expose no failed device.
- **Existing callers.** Keep the existing fatal-path death test.

[Capability tests](../ZenRHI/tests/VulkanCapabilityIntegrationTests.cpp) · [Borrowed-handle API](../ZenRHI/include/ZenRHI/Vulkan/VulkanInterop.h)

**Installed SDK.** Test the change through the installed SDK as well as the source build:

1. Configure against an installed ZenFoundation so `ZenRHIRelocatedConsumer` is registered.
2. Run the existing `package` CTest label, and build the consumer against the relocated installation. That test currently stops after linking.
3. On a native worker, run the standalone Triangle example against the installed SDK. It covers device creation, recording, presentation and shutdown.

Package failures and driver failures then leave separate evidence. [Package test registration](../ZenRHI/tests/CMakeLists.txt#L137) · [Relocation test](../ZenRHI/tests/cmake/RelocatedConsumer.cmake) · [Triangle](../ZenRHI/examples/Triangle/Triangle.cpp)

## 4. Make hangs and device loss diagnosable

**Stall reports (R24).** The earlier draft listed this item as a candidate with a trigger, but its trigger already holds. The Windows RHI wait uses an infinite timeout, and queue and swapchain waits use `UINT64_MAX`. A task that never returns hangs the application without a diagnostic.

Adapt phases 1 and 2 of the watchdog design to the standalone library. Report the waiting operation, elapsed time and worker state through the device message callback. Test it with a task that is deliberately blocked and then released. Verify the report, eventual completion and continued Windows message servicing. A diagnostic timeout must never advance a GPU serial. [Current wait](../ZenRHI/src/Core/RHIThread.cpp#L24) · [Watchdog design](RHIStallWatchdogPlan.md)

**Device-loss acceptance (R7).** ZenRHI already reads `VK_EXT_device_fault` information and records optional breadcrumbs. Acceptance still needs a deliberate device loss on a dedicated test machine. A GPU timeout on a development machine can require a reboot, as the library-split record shows.

Combine everything known at the loss into one report delivered through the message callback:

- Device-fault data
- Breadcrumbs
- The driver version
- The last submitted and completed serial for each queue
- The open debug labels

NVIDIA Aftermath crash dumps remain an optional, capability-gated addition, behind a build option as in UE5. Add them only if the R7 reproduction shows that the built-in report cannot identify the faulting pass. [Device-fault query](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L70) · [R7 verification](RHIProductionR7Verification.md) · [UE Aftermath breadcrumbs](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanQueue.cpp#L45)

## 5. Measure the RHI worker before changing it

The executor sends shader, pipeline, texture, view, buffer, sampler and acceleration-structure creation to the worker through synchronous `InvokeChecked` calls. That worker also translates and submits command lists. A 162 ms compilation of `HybridTemporalSP` therefore delays any frame queued behind it, and the calling thread waits as well. UE5’s Vulkan RHI declares support for multithreaded resource creation. NVRHI creates native objects on the calling thread. [Executor pipeline creation](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1384) · [Executor texture creation](../ZenRHI/src/Core/RHICommandListExecutor.cpp#L1451) · [Compilation measurement](RHIPipelineCompilationFollowup.md) · [UE multithreaded resources](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanRHI.cpp#L456) · [NVRHI texture creation](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-texture.cpp#L351)

`RHIExecutionCounters` count events such as draws, submissions and descriptor hits. They do not measure time. [Counters](../ZenRHI/include/ZenRHI/RHIExecutionCounters.h)

**Measure worker time.** Divide worker time per frame into these categories:

- Command translation
- Pipeline creation
- Other resource creation
- Native submission
- Fence, timeline and present waits
- Idle time

Also record how long callers block in `InvokeChecked`. Publish the values with the existing counters, and make them cost nothing when disabled.

**Workloads.** Measure four ZenApp workloads, each with an explicit `--setup`:

- Cold start to the first frame
- First entry into GI
- A draw-heavy steady state
- Scene load

| Dominant cost | Next step |
| --- | --- |
| Pipeline creation on first use | [Persistent pipeline cache](#51-persistent-pipeline-cache), then [off-worker compilation](#52-off-worker-pipeline-compilation) |
| Resource creation during loading | [Off-worker resource creation](#53-off-worker-resource-creation) |
| Translation in steady state | [Parallel translation](#54-parallel-command-translation-r11) |
| Waits | Inspect dependencies and frame pacing first. Add no threads for a wait. |

### 5.1 Persistent pipeline cache

ZenRHI creates an empty `VkPipelineCache` for each device. The existing microbenchmark measures reuse within one process. RenderCore also caches live pipeline objects and warms selected keys. [Device cache](../ZenRHI/src/Vulkan/VulkanDevice.cpp#L581) · [Existing benchmark](../ZenRHI/tests/VulkanRecordingIntegrationTests.cpp#L1869)

**Design.**

- **Backend.** Accept optional cache input at device creation. Provide an explicit snapshot operation for use while the device is healthy.
- **Host.** Own the cache path, the file size policy and atomic file replacement.
- **Validation.** Store the payload length and checksum. Check the Vulkan header version, vendor, device and cache UUID before import. Pass only previously exported driver bytes to Vulkan.
- **Failure handling.** A truncated or mismatched file selects the existing empty-cache path. A failed snapshot or write keeps the previous file.

[Cache input](https://docs.vulkan.org/refpages/latest/refpages/source/VkPipelineCacheCreateInfo.html) · [Cache snapshot](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetPipelineCacheData.html)

**Decision experiment.** NVIDIA and AMD drivers keep their own on-disk shader caches. Those caches already remove most repeated cost on a machine that has run the application before. Run a 2×2 matrix with the driver cache cold and warm, and the application seed empty and present. Record how the driver cache was cleared. The application cache matters most when the driver cache is cold, for example after a driver update. A completely new installation gains nothing from either cache.

For each fresh process, record:

- Pipeline creation time
- Startup time
- Worst and p95 CPU frame time, including the first entry into GI
- Cache size

Check that the rendered output is equivalent. Exercise truncated and mismatched seeds. Keep persistence only if repeated pairs improve first-use time beyond baseline variation at an acceptable startup and storage cost.

`VK_KHR_pipeline_binary` replaces the opaque cache with explicit per-pipeline binaries. Evaluate it only if the cache proves useful but its size or invalidation becomes a problem. [Pipeline binaries](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_binary.html)

### 5.2 Off-worker pipeline compilation

**Trigger.** One of these holds:

- After 5.1, the first entry into GI still spends more than its frame budget in pipeline creation.
- Step 4 shows pipeline creation delaying translation.

**Design.** Implement the existing proposal. RenderCore owns request timing, deduplication and fallback selection. ZenRHI owns native compilation and device lifetime. [Async compilation design and acceptance](RHIPipelineCompilationFollowup.md#proposed-ownership-and-execution)

The measured hitches are compute pipelines. Graphics pipeline libraries, which UE5 supports, would not shorten them. Consider libraries only for a workload with many graphics variants. [UE pipeline-library support](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.h#L573)

### 5.3 Off-worker resource creation

**Trigger.** Step 4 shows loading threads blocked in `InvokeChecked` for creation, or creation delaying translation.

**Smallest experiment.**

- Create buffers, textures, views and samplers on the calling thread.
- Before relying on concurrency, confirm the synchronization requirement of each Vulkan and VMA call. Vulkan object creation does not externally synchronize the device. VMA locks internally unless its allocator is created externally synchronized.
- Publish state that the worker reads, such as bindless registration and lifetime tracking, through the existing ordered paths.
- Keep destruction deferred through the lifetime tracker.

**Acceptance.** Rendered output is unchanged. Threaded and inline runs produce no validation errors. Blocked time in the scene-load workload falls.

### 5.4 Parallel command translation (R11)

**Trigger.** A draw-heavy ZenApp trace shows translation on the CPU critical path. Separate main-thread recording, queue waits, pipeline creation, native translation and submission.

**Smallest experiment.** Translate independent lists on Vulkan contexts and command pools owned by worker threads. Keep ordered submission and the existing receipts. UE5 uses this model. NVRHI instead records native commands on each recording thread. [Threading baseline](RHIProductionR11Verification.md) · [UE translation scheduling](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/RHI/Private/RHICommandList.cpp#L1568)

**Acceptance.**

- CPU frame p95 and GPU output are compared with the serial path.
- When a native submission is rejected, dependent work receives no accepted serial for it. [Queue failure tests](../ZenRHI/tests/VulkanQueueIntegrationTests.cpp#L428)

## 6. Add features when a consumer needs them

Each feature needs a consumer, a capability flag in `RHIGPUInfo`, a test and a README entry. This table checks the backlog’s R13 list against the current code and adds three architecture items.

| Feature | Current state | Trigger | Reference |
| --- | --- | --- | --- |
| Synchronization2 barriers (backlog 5.3) | One source and one destination stage mask per `vkCmdPipelineBarrier` call. Synchronization2 is used only for micromaps. | A graph consumer needs per-resource stages, a capture shows a broad barrier delaying work, or the Vulkan 1.3 baseline is adopted. Verify readback and queue dependencies with the feature on and off, including aliased queues and a distinct-family device. | [Barrier emitter](../ZenRHI/src/Vulkan/VulkanSynchronization.cpp#L422) · [NVRHI barriers](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/src/vulkan/vulkan-state-tracking.cpp#L214) |
| Transient memory aliasing | Resources allocate from VMA default pools. The public API has no heaps or placed resources. | ZenApp frames at 3840×2160 with hardware ray-traced GI approach the memory budget reported by `GetGPUMemoryStats`. | [UE transient allocator](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanTransientResourceAllocator.h#L44) · [NVRHI heaps](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/include/nvrhi/nvrhi.h#L3845) |
| Descriptor buffers | Descriptor sets with update-after-bind global heaps | Descriptor preparation and binding take a significant share of the step 4 breakdown. | [UE bindless default](https://github.com/EpicGames/UnrealEngine/blob/821fa3e7e3e5c66ddcd2d66d0603704f52e09fcc/Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp#L31) |
| Non-indexed indirect and indirect-count draws | Only indexed indirect draws and indirect dispatch and trace commands exist. | GPU-driven culling | |
| Occlusion and pipeline-statistics queries | Only internal timestamp queries exist. | Visibility culling or a profiling interface | |
| Stencil reference | No command exists, and it is not dynamic state. | A pass that uses stencil | |
| Mesh shaders, variable-rate shading, tessellation | Not supported. Tessellation stages are rejected. | A renderer feature that needs them | |
| HDR presentation | Only 8-bit sRGB-nonlinear surfaces are accepted. | HDR output in a product | |
| Several viewports in one submission | `SubmitFrame` presents one viewport. | Editor windows that must present together | |
| External device and memory interop | ZenRHI exports borrowed handles but cannot adopt a caller’s device or memory. | An OpenXR runtime that chooses the device, or CUDA interop | [NVRHI caller-provided device](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/include/nvrhi/vulkan.h#L50) |
| Separate presentation queue (R14) | The graphics queue must present. | A supported platform that requires one | |

## 7. Release engineering for 1.0

**Versioning.** Every Zen repository already shares one `vX.Y.Z` version. Add these rules for 1.0:

- A breaking public API change requires a major version.
- A removal follows at least one minor release in which the API is deprecated.
- The policy job diffs public declarations against the previous tag, so every breaking change is explicit. `check_public_api.py` already checks declarations against the Foundation SDK. [Public API checker](../ZenRHI/tools/check_public_api.py)

**Documentation.** The README is a contract reference. Add a task-oriented programming guide, like NVRHI’s, that covers device creation, resources, recording, submission, presentation, retirement and shutdown. Include a table that states, for each public entry point, whether the render thread, any thread, the window owner or only the RHI worker may call it. [NVRHI programming guide](https://github.com/NVIDIA-RTX/NVRHI/blob/bf0bbd9b666a7297492beb1999a4ddc0dfded143/doc/ProgrammingGuide.md)

**Examples.** Add examples alongside Triangle:

- Compute dispatch with readback
- Bindless textures
- An async-compute dependency
- Ray query

Build each one against the installed SDK under the `package` label, so the examples also serve as SDK tests.

**Hardening.** In a scheduled job, run the unit and integration suites under MSVC AddressSanitizer. Fuzz SPIR-V reflection, which rejects malformed input since R18, using MSVC’s libFuzzer support. Shader input is the main untrusted data that crosses the RHI boundary. [R18 verification](RHIProductionR18Verification.md)

**Backlog reconciliation.** The production backlog still describes paths from before the library split, under `ZenCore`. Its R13 table predates adapter selection, acceleration structures and ray-tracing pipelines. Bring R7, R8, R10, R11, R13, R14, R24, 5.3 and 5.9 into this plan or update their paths. Then mark the backlog as superseded for the standalone library. [Production backlog](RHIProductionTODO.md)

## 8. Rules for every change

**Contracts to preserve.**

- A published queue serial means Vulkan accepted that submission.
- Resource and bindless-slot reuse requires the relevant recordings and GPU uses to retire.

Extend the existing timeline and fence tests whenever a change touches either contract. Validate both RHI execution modes, and test both the timeline and fence paths when submission changes. Keep a change only if it helps or is neutral and adds no validation or synchronization-validation errors. [Lifetime contract](../ZenRHI/include/ZenRHI/README.md#resources-and-recorded-commands) · [Device-loss serial regression](../ZenRHI/tests/VulkanQueueIntegrationTests.cpp#L512) · [Backlog rules](RHIProductionTODO.md#1-rules)

**Application performance decisions.**

- Use the release `build/x64-windows-msvc-release/bin/ZenApp.exe` with an explicit `--setup=<file.zenrender>` and recorded startup overrides.
- Keep the executable, setup and input hashes, plus the exact arguments.
- Use `tools/profile_hybrid_gi.py` for Nsight GPU traces at 3840×2160.
- Verify the actual framebuffer and render extents, fullscreen state, backend and async-compute state.
- Keep validation-enabled correctness runs separate from timing runs.

[Project profiling workflow](../AGENTS.md#profiling-workflow) · [Capture tool](../tools/profile_hybrid_gi.py)
