# RHI deferred work verification

Executed on 2026-10-02 from `e79660ec`, following section 5 of [RHIImprovementPlan.md](RHIImprovementPlan.md). Each item was measured against its gate before any change was kept. Two decisions came from the user before work started: VSync becomes a configurable present mode with FIFO for VSync on (5.8), and 5.3, 5.9 and 5.10 stay unstarted. Nothing was staged or committed.

| Item | Outcome | Evidence |
| --- | --- | --- |
| 5.1 Presentation copy | Option A implemented; option B gate closed | One `vkQueueSubmit` per presented frame on timeline devices; copy ≈ 8 µs GPU |
| 5.2 One submit per queue per frame | Gate closed | Steady frames have one group, including with async compute |
| 5.3 Synchronization2 | Not started (decision) | — |
| 5.4 Per-frame command pools | Gate closed | Setup ≈ 4–6 µs per command buffer |
| 5.5 Descriptor binding model | Diagnosed; RenderCore fix proposed | Per-frame miss = G-buffer targets recreated by the RDG pool budget |
| 5.6 Persistent pipeline cache | Implemented, measured, removed | No warm-start gain over the driver's own cache |
| 5.7 Uniform-buffer limit | Implemented | Over-limit shaders fail before native creation; this driver allows 8 |
| 5.8 Present mode | Implemented | `present_mode` / `--present-mode=`; FIFO for VSync on |
| 5.9 Global singletons | Not started (decision) | — |
| 5.10 Error handling | Not started (decision) | — |
| 5.11 Small allocation pools | Deleted | Pools added 32 MiB committed memory in every mode |

## Environment and method

Same machine as [RHIImprovementVerification.md](RHIImprovementVerification.md): Windows x64, VS 2022 Professional (MSVC 19.44), Ninja, Vulkan SDK 1.4.304.0, AMD Radeon RX 7900 XT with timeline semaphores and separate graphics, compute and transfer families.

Every GPU process ran from a byte-identical copy named `7zFM.exe` so the installed RTSS overlay does not hook it, with `VK_LOADER_LAYERS_DISABLE=~implicit~`. Correctness runs also set `VK_LAYER_VALIDATE_SYNC=1`. Profiles use the Phase 0 settings: Sponza, voxel resolution 256, 1280 × 720, 60 warm-up and 600 measured frames, `--fixed-step --vsync=0 --disable-validation`; only `phase=measured` rows count. These conditions differ from the earlier verification, which ran hooked, so its absolute timings are not comparable with the tables here; every comparison below is within this session. `Data/engine.cfg` was never modified (SHA-256 unchanged across all runs).

Raw logs, CSVs and JSON summaries are under `build/rhi-deferred/`: `baseline/`, `probe/` (temporary instrumentation), `probe-budget512/`, `gate-5.2-motion/`, `gate-5.6/`, `gate-5.11/` and `final/`.

### Baseline at `e79660ec`

Each cell is the median of two 600-frame runs, in ms (CPU frame / RHI execution / GPU frame). Every run had 2 native submissions per frame, 600 descriptor misses and no steady-state dispatches, including with async compute enabled.

| Mode / RHI thread | Async compute off | Async compute on |
| --- | --- | --- |
| PBR (2) / inline | 1.227 / 0.105 / 1.138 | 1.251 / 0.599 / 1.145 |
| PBR (2) / threaded | 1.233 / 0.537 / 1.146 | 1.260 / 0.483 / 1.143 |
| GI (3) / inline | 1.577 / 0.112 / 1.454 | 1.582 / 0.885 / 1.458 |
| GI (3) / threaded | 1.573 / 0.879 / 1.454 | 1.583 / 0.763 / 1.455 |

Between the two baseline repeats, CPU medians moved by up to 4% and GPU medians by up to 1.4%. Inline frames with async compute off use RenderDevice's synchronous frame path, whose RHI execution interval (≈ 0.1 ms) is the actual translation and submission work. The other combinations go through the queued frame path, whose interval also includes waiting.

## Gate measurements

Temporary instrumentation (`probe/probe.patch`, reverted before any kept change) recorded the last 600 or 1,200 samples of each measured run:

| Probe | Median | p95 | Per frame |
| --- | ---: | ---: | --- |
| Presentation copy, GPU timestamps | 8.0 µs | 13 µs | 1 copy, ≈ 0.6% of `gpu_frame_ms` |
| `vkQueueSubmit` call | 16–18.5 µs | 20–49 µs | 2 calls |
| Command buffer setup (acquire, reset, begin) | 4.2–6.4 µs | 6–7 µs | ≈ 2 buffers |
| Command buffer end | 1.0–1.3 µs | 1.4 µs | ≈ 2 buffers |

- **5.1 option B:** the copy is about 0.6% of the GPU frame at 720p, inside the GPU variation above. Gate closed.
- **5.2:** steady Sponza frames contain one graphics group in every combination, so after 5.1 A each frame makes one submission. The motion fixture that would update voxels every frame requires a dedicated test scene and was not profiled. The gate stays closed for the shipped workload.
- **5.4:** command-buffer setup and end total about 10–15 µs per frame. Per-frame pools would replace only the individual resets, a small part of that, which is below the CPU variation. Gate closed.

### 5.5: the per-frame descriptor miss

Logging the content key of each miss showed one miss per frame, always set 1 of `DeferredLightingSP` (or `DeferredVoxelGISP` in GI mode). Bindings 0, 1, 4 and 5 alternate between two pooled texture IDs, one per frame slot, while bindings 2 and 3 (`albedoMap`, `metallicRoughnessMap`) receive new IDs every frame. A second probe in `RDGResourceManager::CreatePhysicalResource` confirmed that `offscreen_albedo` and `offscreen_roughness`, two 2048 × 2048 RGBA8 render targets, are recreated every frame.

The render graph's transient pool budget defaults to 256 MiB of idle payload. The default 2048² G-buffer needs about 144 MiB per frame, and two frame slots are in flight, so each trim evicts the last-added RGBA8 pair. The 256 MiB default predates the G-buffer becoming an RDG transient resource ([RDGRenderCoreAnalysisAndPlan.md](RDGRenderCoreAnalysisAndPlan.md), Phase 5).

With the budget temporarily raised to 512 MiB (`probe-budget512/`), descriptor misses fell from 600 to 0 per interval and no texture was recreated, while CPU and GPU medians stayed within variation (for example PBR inline 1.2288 vs 1.2277 ms). The churn therefore costs allocations, not measurable frame time. The fix belongs to RenderCore and changes a verified Phase-5 default; it is planned, with a screen-sized G-buffer, in [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md).

## Kept changes

### 5.1 A: presentation copy in the last graphics submission

`RHICommandListExecutor::ExecuteGroups` prepares presentation immediately before submitting a frame's last group when that group runs on the graphics queue, and passes `[group list, present list]` to one `ExecuteBatch`. The copy keeps its own context, command buffer and `VkSubmitInfo`; only that submit info waits on the acquire semaphore, so the frame's own transfer work is never gated by image acquisition. On timeline devices both submit infos go into one `vkQueueSubmit`. Fence-path devices still submit each workload with its own fence.

Each submit info has its own timeline serial. A group's accepted point now comes from `IRHICommandContext::RHIGetLastSubmittedSerial()`, which `VulkanRHI::FlushAllGPUCommands` already sets per context, falling back to the queue's serial when the context accepted nothing new. Producer points therefore remain exact; `requiredSerials` still includes the copy.

`RenderDevice::ExecuteRenderGraph`'s synchronous inline path now hands its viewport to `SubmitGroups`, so both frame paths present through the executor. A rejected submission accepts neither the frame nor its copy and keeps the acquired image for the next attempt; the device is not blocked. A frame whose last group runs on async compute still presents in a separate submission. `RenderDevice::AcquireGraphicsCmdLists` and `SubmitCommandLists` became unused and were removed.

Tests:

- New `RHIWindowSurfaceIntegrationTest.PresentationCopySharesTheLastGraphicsSubmission` (inline and threaded, real swapchain): one `vkQueueSubmit` per presented frame, presented, group serial exact and the copy one serial later.
- `RenderCoreTest.FrameAndPresentationShareOneSubmissionAndRejectTogether` replaces the test that required a separately rejectable copy: a rejection is one flush with nothing submitted and no present; the next frame is one flush and presents.
- Updated expectations for the shared flush: `AsyncUploadTest.FrameSubmissionKeepsUploadWaitAndOrdersLaterTransfer` (the fake backend gives the group and copy one serial), `RDGScheduledSubmissionTest.PartialFailureConsumesTicketAndKeepsAcceptedFrameRetirement` (presentation is prepared before the failed graphics submission) and `ThreadedRenderCoreTest.WindowsFrameTicketWaitServicesSentMessages` (one sent message per flush).
- Two RenderCore tests whose stack `TestViewport` now outlives a frame batch drain retirement before leaving scope, as the queued-path tests already did.

### 5.7: uniform-buffer limit check

`CountUniformBufferDescriptors` counts every non-bindless uniform-buffer element per pipeline layout and per stage. `VulkanShader::Init` compares the counts with `maxDescriptorSetUniformBuffersDynamic`, `maxDescriptorSetUniformBuffers` and `maxPerStageDescriptorUniformBuffers` before creating any native object; on a violation it logs the counts and `VulkanShader::CreateObject` returns null, as it already did for unloadable SPIR-V. Both RenderCore callers already handle a null shader. A source scan finds at most four uniform blocks per shipped program; this driver allows 8 per layout.

Tests: `VulkanDescriptorLayoutTest.UniformBufferLimitsCountEveryDynamicElementPerLayoutAndStage` (device-free counting and each limit), and `VulkanDescriptorIntegrationTest.ShaderOverUniformBufferLimitIsRejectedBeforeNativeCreation`, which uses a new 64-element fixture (`descriptor_uniform_limit.comp`) and counts `vkCreatePipelineLayout` calls: zero for the rejected shader, one for a four-buffer shader.

### 5.8: present mode

`RHIPresentMode` (`eDefault`, `eFifo`, `eFifoRelaxed`, `eMailbox`, `eImmediate`) is an `RHIOptions` setting, read whenever a swapchain is created or recreated. `eDefault` follows the viewport's VSync flag: FIFO when on (previously mailbox first), unchanged immediate → mailbox → FIFO when off. An explicit mode falls back to FIFO with a warning when unsupported. The demo reads `present_mode` from the configuration (documented in `Data/engine.example.cfg`) and `--present-mode=` overrides it. Tests: `VSyncChoicesUseOnlyAdvertisedModes` (updated), `ExplicitPresentModeOverridesVSyncAndFallsBackToFifo` and `RHIPresentModeTest.ParsesConfigurationNames`.

### 5.11: small allocation pools deleted

Peak VMA commitments over 120 frames, three runs per cell, identical across runs:

| Mode | With pools: committed / device-local MiB | Without pools |
| --- | --- | --- |
| 1 | 896 / 800 | 864 / 768 |
| 2 | 1408 / 1312 | 1376 / 1280 |
| 3 | 1979 / 1883 | 1947 / 1851 |

The pools never reduced memory; each run committed one extra 32 MiB block. `VulkanMemoryAllocator` now allocates every image and buffer from VMA's default pools, and the per-image `vkCreateImage` probe that selected small images is gone. The initial measurement omitted separate glTF memory measurements; the review follow-up below closes that gap with all 18 smoke assets.

## Measured and removed: 5.6 persistent pipeline cache

The implementation loaded a validated file at device creation and saved it atomically at destruction. Validation covered an engine header with magic, version, size and FNV-1a checksum, plus the Vulkan header's vendor, device and pipeline-cache UUID. Its unit tests passed. Startup in Release, five runs each, measured as the first frame's CPU time (which includes lazy pipeline creation) and process wall time:

| Mode | Cache | First-frame median ms | Wall-time median s |
| --- | --- | ---: | ---: |
| PBR (2) | off | 27.9 | 1.086 |
| PBR (2) | warm (116 KB file) | 28.2 | 1.043 |
| GI (3) | off | 189.4 | 1.240 |
| GI (3) | warm | 192.8 | 1.220 |

The first "off" run of each mode was the only cold one: 181 ms and 561 ms first frames. Every later run, with or without the engine cache, was already warmed by the AMD driver's own on-disk cache. The engine cache added nothing measurable, so the change was removed; `gate-5.6/pipeline-cache-5.6.patch` keeps the tested implementation for drivers without their own cache.

## Initial validation (before review follow-up)

**Builds.** Debug and Release build every default target; the only compiler warning is the existing GLI `GLM_ENABLE_EXPERIMENTAL` redefinition. All 21 changed C++ files pass whole-file `clang-format --dry-run --Werror` (clang-format 19.1.5), and `git diff --check` passes. The Performance preset was not rebuilt.

**Suites** (identical counts in Debug and Release, zero validation or synchronization-validation messages; [results](../build/rhi-deferred/final/)):

| Suite | Passed | Skips | Change from `e79660ec` |
| --- | ---: | ---: | --- |
| CommonTest | 98 | 1 | — |
| RenderCoreTest | 528 | 0 | One test replaced, three expectations updated, two tests drain batches |
| VulkanRHITest | 42 | 0 | +1 (uniform-buffer limits) |
| VulkanRHIIntegrationTest | 297 | 6 | +5 (limit rejection, explicit present mode, present-mode names, shared submission ×2) |
| SmartPtrTest, FlatHashMapTest, LRUCacheTest, InputControllerTest, ConfigLoaderTest, UIDrawPacketTest, RuntimeUIIntegrationTest, SceneModelSwitchTest, ConeVoxelGIIntegrationTest | 34, 6, 12, 7, 21, 8, 10, 8, 4 | 0 | — |
| ThreadPoolTest (sample program) | exit 0, no leaks | — | — |

The six skips are the same capability skips as before (swapchain-maintenance presentation fences, the EXT surface-maintenance dependency, D24S8).

**Smoke matrix.** All 24 runs (modes 1/2/3 × RHI thread 0/1 × async compute 0/1, Debug and Release, 44 frames with resize, minimize/restore, light changes and shutdown) exit 0 with zero engine errors and zero validation or synchronization-validation messages.

**Captures.** The three 8-frame captures are byte-identical to the baseline hashes in [RHIImprovementVerification.md](RHIImprovementVerification.md).

**Image tools.** `smoke_gltf_rendering.py`: 18 of 18 assets passed, including the BasisU/KTX, Draco/Meshopt, skinning and morph-target assets that exercise small textures and buffers. `validate_voxel_gi.py`: all 162 GPU cases and image assertions passed. Both ran unhooked; `Data/engine.cfg` was byte-identical before and after (SHA-256 `c09a51bc…7871`).

### Performance

Final profiles at the final source, same 16 runs as the baseline (median of two runs per cell, ms):

| Mode / RHI thread / async | Baseline CPU / GPU | Final CPU / GPU | CPU | GPU | Submissions per frame |
| --- | --- | --- | ---: | ---: | --- |
| PBR / inline / off | 1.2267 / 1.1381 | 1.2487 / 1.1397 | +1.8% | +0.1% | 2 → 1 |
| PBR / inline / on | 1.2508 / 1.1455 | 1.2552 / 1.1434 | +0.4% | −0.2% | 2 → 1 |
| PBR / threaded / off | 1.2333 / 1.1463 | 1.2660 / 1.1390 | +2.7% | −0.6% | 2 → 1 |
| PBR / threaded / on | 1.2596 / 1.1432 | 1.2824 / 1.1413 | +1.8% | −0.2% | 2 → 1 |
| GI / inline / off | 1.5765 / 1.4539 | 1.6077 / 1.4690 | +2.0% | +1.0% | 2 → 1 |
| GI / inline / on | 1.5819 / 1.4581 | 1.5973 / 1.4710 | +1.0% | +0.9% | 2 → 1 |
| GI / threaded / off | 1.5730 / 1.4539 | 1.6184 / 1.4644 | +2.9% | +0.7% | 2 → 1 |
| GI / threaded / on | 1.5834 / 1.4545 | 1.6169 / 1.4679 | +2.1% | +0.9% | 2 → 1 |

Submissions per frame halve everywhere, but CPU frame medians are 0.4–2.9% higher in every cell. Because the baseline and final batches ran 2.5 hours apart, a same-build A/B followed: a temporary switch restored the separate presentation submission, and runs were interleaved A-B-B-A-A-B in a single session (`ab-5.1/`).

| Mode / RHI thread | Combined CPU / GPU | Separate CPU / GPU | Per-run CPU ranges |
| --- | --- | --- | --- |
| PBR / inline | 1.4210 / 1.1388 | 1.3895 / 1.1419 | 1.353–1.422 vs 1.277–1.446 |
| GI / inline | 1.5948 / 1.4667 | 1.5891 / 1.4741 | 1.588–1.600 vs 1.587–1.591 |
| PBR / threaded | 1.4722 / 1.1354 | 1.4568 / 1.1387 | 1.466–1.480 vs 1.456–1.496 |
| GI / threaded | 1.6138 / 1.4640 | 1.6065 / 1.4648 | 1.603–1.620 vs 1.605–1.610 |

PBR medians drifted from about 1.23 to 1.39–1.47 ms within the session, so most of the batch difference above is environmental. The combined path shows a small, consistently signed tendency: CPU frame interval +0.4% to +2.3% with overlapping run ranges, and GPU time 0.1–0.5% lower. These frames are GPU-paced, so the RHI-thread savings measured by the probes (one fewer `vkQueueSubmit`, about 16–18 µs, plus one executor batch, about 15 µs) do not shorten them. Acquiring the image before the last group is submitted may expose a few microseconds of acquire latency. 5.1 A is kept as neutral within this measurement resolution; a frame-time improvement is not established.

**VSync pacing.** With `--vsync=1` (FIFO) on the 165 Hz display, a GI configuration was run at 2560 × 1440 with a 4096² G-buffer and the RDG pool budget temporarily raised so the G-buffer was not recreated. Its GPU frame was 5.4–6.0 ms, about 97% of the 6.06 ms refresh. Combined and separate presentation both held 165 Hz (frame medians 5.98–6.07 ms). Frames later than 1.5 refreshes: inline 8 vs 5 and threaded 0 vs 1, out of 1,800 each (`vsync-pacing/`). No pacing regression from the earlier acquisition was observed.

**Larger G-buffers.** At a 4096² G-buffer with the default pool budget, the CPU frame was 10.2 ms against 5.5 ms of GPU time: the whole G-buffer, about 576 MiB per frame, exceeds the 256 MiB budget and is recreated every frame. This is the 5.5 finding at full scale.

## Limits

- One machine and driver. The review follow-up below forces and validates the real fence submission path on the timeline-capable GPU; a physically timeline-incapable device, integrated GPUs and macOS/Linux were not exercised. On the fence path, 5.1 A saves the executor round but not the second `vkQueueSubmit`.
- 5.2's gate was judged on the shipped workload; a scene with steady async-compute groups was not profiled.
- The 5.6 decision reflects a driver with an on-disk pipeline cache.
- The RDG pool budget (5.5) is unchanged; see [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md).

## Review follow-up: forced fence coverage and full revalidation (2026-10-02)

Artifacts and executable hashes are under [review-fixes/](../build/rhi-deferred/review-fixes/). The exact runner is [validate.py](../build/rhi-deferred/review-fixes/validate.py); every subprocess command, exit code, skip count and validation message is recorded in its `*-runs.json`. GPU processes run one at a time from copies named `7zFM.exe`, with implicit layers disabled. Correctness runs enable synchronization validation. The original `Data/engine.cfg` bytes are backed up and restored with an external-edit check; SHA-256 after correctness validation is `c09a51bce3cc624cd1b66d3d90fb26072089bf22aa8c6c4ad0ac5aa483bf7871`.

### Regression coverage added

`RHIWindowSurfaceIntegrationTest` now runs every case in four configurations: inline/native, threaded/native, inline/forced-fence and threaded/forced-fence. After backend initialization and an idle wait, the test clears `hasTimelineSemaphore` before the executor snapshots capabilities and before viewport creation. The native device still supports timelines, but the real queue submission, fence and completion implementations use the fence path. No production option or alternate submission implementation was added.

- The successful combined frame verifies one native call on timelines, two on fences, an exact rendering-group serial, and a retirement serial including the copy.
- `RejectedCombinedGroupKeepsAcquisitionForRetry` rejects the first native call, verifies no accepted work or presentation, then rebuilds successfully without acquiring another swapchain image.
- `FailedCombinedFrameRetainsItsAcceptedPrefix` rejects the timeline batch atomically or the second fence submission after rendering was accepted. It verifies the rejected/fatal distinction, exact accepted and retirement serials, failed producer state, no native presentation, blocked later work, and resource retention even after an idle wait. Teardown reports no leaked memory.

Viewport creation refreshes progress before the tests snapshot cached serials; without that refresh, the threaded path's initialization submission is not yet reflected in the cached getter. All 28 window cases pass in both Debug and Release. The declaration-spacing correction in `RHICommandListExecutor.h` is present and verified.

### Correctness results

All default targets build in Debug and Release. Only the existing third-party GLI `GLM_ENABLE_EXPERIMENTAL` redefinition warning remains. Whole-file clang-format 19.1.5 and `git diff --check` pass. No validation errors, validation warnings or synchronization-validation messages were found in the suite or smoke logs; negative tests intentionally emit engine errors for injected failures.

| Suite | Debug passed / skipped | Release passed / skipped |
| --- | ---: | ---: |
| CommonTest | 98 / 1 | 98 / 1 |
| RenderCoreTest | 528 / 0 | 528 / 0 |
| VulkanRHITest | 42 / 0 | 42 / 0 |
| VulkanRHIIntegrationTest | 315 / 6 | 315 / 6 |
| SmartPtrTest | 34 / 0 | 34 / 0 |
| FlatHashMapTest | 6 / 0 | 6 / 0 |
| LRUCacheTest | 12 / 0 | 12 / 0 |
| InputControllerTest | 7 / 0 | 7 / 0 |
| ConfigLoaderTest | 21 / 0 | 21 / 0 |
| UIDrawPacketTest | 8 / 0 | 8 / 0 |
| RuntimeUIIntegrationTest | 10 / 0 | 10 / 0 |
| SceneModelSwitchTest | 8 / 0 | 8 / 0 |
| ConeVoxelGIIntegrationTest | 4 / 0 | 4 / 0 |
| ThreadPoolTest sample | exit 0, no leaks | exit 0, no leaks |

The six Vulkan skips remain capability-dependent (presentation fences, surface-maintenance dependency, D24S8). The 18 additional passing integration cases are expanded forced-fence window coverage and the two new failure tests across four configurations.

All 24 smoke runs pass: Debug/Release × modes 1/2/3 × RHI thread 0/1 × async compute 0/1, 44 frames each with resize, minimize/restore, light changes and shutdown. All three 8-frame captures remain byte-identical to the initial verified captures; full hashes are in [captures.json](../build/rhi-deferred/review-fixes/captures.json).

`smoke_gltf_rendering.py` passes all 18 representative assets. `validate_voxel_gi.py` passes all 162 GPU cases and image assertions using the Release executable. The profile-reader unit suite passes all 15 tests. These image tools restore the configuration exactly. The separate Performance build preset is not required by section 6 and was not rebuilt; the performance measurements below use Release.

### Allocation gate completed: all glTF smoke assets

[build_variants.py](../build/rhi-deferred/review-fixes/build_variants.py) builds two Release measurement binaries from the current source. Both have the same temporary end-of-run `GetGPUMemoryStats()` log after flushing the RHI thread; one restores only `VulkanMemory.h/.cpp` from `e79660ec`, the other uses the current default-pool allocator. This isolates the allocator change. The script saves the source bytes and variant patches, then restores every source byte and rebuilds the final targets. No probe or comparison switch remains in production code.

For each workload, runs are interleaved pools/default/default/pools: 60 warm-up and 120 measured frames at 640 × 480, fixed step, VSync off, no UI, ray tracing and validation disabled. Sponza uses modes 1–3 with the original voxel resolution 256; the 18 representative glTF assets use mode 2 and the smoke tool's configuration (voxel resolution 64, shadow resolution 256, fitted camera). All 84 processes succeed and report zero remaining VMA bytes at teardown.

Peak committed memory, MiB (identical in both repeats of each variant):

| Workload | Custom pools | Default pools |
| --- | ---: | ---: |
| Sponza mode 1 | 896 | 864 |
| Sponza mode 2 | 1408 | 1376 |
| Sponza mode 3 | 1979 | 1947 |
| Box / Draco | 832 | 800 |
| MeshoptCubeTest / Meshopt | 832 | 800 |
| BrainStem / Meshopt EXT | 896 | 864 |
| StainedGlassLamp / KTX BasisU | 512 | 480 |
| SheenWoodLeatherSofa | 384 | 352 |
| MeshPrimitiveModes | 192 | 160 |
| PrimitiveModeNormalsTest | 576 | 544 |
| AlphaBlendModeTest | 384 | 352 |
| SimpleSkin | 832 | 800 |
| AnimatedMorphCube / Quantized | 832 | 800 |
| AnimatedColorsCube | 832 | 800 |
| AnimationPointerUVs | 384 | 352 |
| SimpleInstancing | 832 | 800 |
| NodeVisibilityTest | 832 | 800 |
| LightVisibility | 832 | 800 |
| MosquitoInAmber | 768 | 736 |
| CompareClearcoat | 384 | 352 |
| TransmissionRoughnessTest | 320 | 288 |

Every workload saves exactly **32 MiB in both peak committed and peak device-local memory**. Every glTF end-of-run live sample also saves 32 MiB in committed and device-local memory. Sponza mode 2's live committed sample varies with retirement: 1152/1408 MiB with pools and 1120/1376 MiB without; its peak is stable. The other Sponza live samples are stable. Full live and peak values are in [memory-summary.json](../build/rhi-deferred/review-fixes/memory-summary.json) and [memory-comparisons.json](../build/rhi-deferred/review-fixes/memory-comparisons.json). This closes 5.11's omitted glTF measurement gate and supports keeping the pool deletion.

### Same-session performance validation

64 Release profiles cover modes 2/3 × RHI thread 0/1 × async compute 0/1. Each cell has two interleaved comparisons, each in A-B-B-A order: the saved `e79660ec` baseline versus final, and a current-source separate-presentation variant versus final. The latter changes only whether the copy joins the final graphics group; it preserves the new synchronous executor adapter, so its RHI timing scope is comparable. Each run has 60 warm-up and 600 measured frames, fixed step, 1280 × 720, voxel resolution 256, VSync and validation off. No builds or other validation processes run concurrently with these profiles.

All 64 profiles report successful runs and unchanged inputs. Configuration, scene document, shader fingerprints and device metadata are identical across every run. The device remains the RX 7900 XT with MSVC 19.44. Both before/after samples use the same current shader files. The original baseline executable and every comparison executable are preserved and hashed.

Each cell below is median / p95 in milliseconds, summarized as the median of the two per-run statistics. Entries show baseline → final.

| Mode / RHI thread / async | CPU frame | GPU frame | RHI execution |
| --- | --- | --- | --- |
| PBR / inline / off | 1.1786 / 1.4391 → 1.1852 / 1.4725 | 1.1172 / 1.2030 → 1.1208 / 1.2039 | 0.1045 / 0.1170 → 0.5822 / 0.8650 * |
| PBR / inline / on | 1.1811 / 1.4587 → 1.1802 / 1.4828 | 1.1173 / 1.2066 → 1.1183 / 1.2094 | 0.4852 / 0.7610 → 0.5188 / 0.8105 |
| PBR / threaded / off | 1.1757 / 1.4752 → 1.1777 / 1.4893 | 1.1150 / 1.2023 → 1.1179 / 1.2113 | 0.5145 / 0.8080 → 0.4950 / 0.8165 |
| PBR / threaded / on | 1.2422 / 1.4150 → 1.2332 / 1.4141 | 1.1159 / 1.1949 → 1.1126 / 1.1841 | 0.4500 / 0.6265 → 0.4487 / 0.6340 |
| GI / inline / off | 1.4909 / 1.7999 → 1.4976 / 1.8099 | 1.4297 / 1.5246 → 1.4319 / 1.5319 | 0.1072 / 0.1165 → 0.8655 / 1.1800 * |
| GI / inline / on | 1.4963 / 1.8207 → 1.4931 / 1.8141 | 1.4342 / 1.5338 → 1.4334 / 1.5297 | 0.7855 / 1.1060 → 0.8003 / 1.1155 |
| GI / threaded / off | 1.4976 / 1.8206 → 1.5026 / 1.8445 | 1.4299 / 1.5363 → 1.4323 / 1.5437 | 0.7692 / 1.0845 → 0.7562 / 1.0950 |
| GI / threaded / on | 1.4975 / 1.7996 → 1.4930 / 1.8214 | 1.4357 / 1.5374 → 1.4338 / 1.5452 | 0.7250 / 1.0280 → 0.7145 / 1.0360 |

\* The old synchronous inline adapter presents outside `ExecuteFrame`'s timer; the new adapter presents inside it. Its RHI interval therefore now includes acquisition/presentation and associated waiting. Those two RHI comparisons are **not evidence of increased translation/submission cost**. Frame intervals remain comparable. The isolated separate-presentation comparison uses the same timing scope on both sides; all its median/p95 CPU, GPU and RHI values are in [performance-comparisons.json](../build/rhi-deferred/review-fixes/performance-comparisons.json).

Across the whole-change matrix, CPU medians change by −0.73% to +0.56%, GPU medians by −0.30% to +0.33%. In the isolated presentation comparison, CPU medians change by −1.75% to +0.89%, GPU medians by −0.21% to +0.18%. The largest positive isolated CPU result is GI/inline/async-off: combined run medians are 1.4968 and 1.5176 ms, while separate runs are 1.4931 and 1.4948 ms; variation within the combined repeats exceeds the aggregate difference. A frame-time speedup is not established; results support retaining 5.1 A as neutral within this run resolution.

Every final measured frame has **one native submission**, against two in the baseline and separate variants. All retain one descriptor miss per measured frame (600 per run), consistent with the deferred RenderCore pool issue; this RHI work does not claim to fix it.

GPU pass intervals, median / p95 in microseconds, aggregated across the eight baseline-comparison runs per mode:

| Mode / pass | Baseline | Final |
| --- | --- | --- |
| PBR / OffScreen | 738.09 / 759.42 | 739.26 / 760.36 |
| PBR / SceneLighting | 703.09 / 747.28 | 706.02 / 748.80 |
| PBR / LightMarkers | 681.88 / 711.28 | 683.90 / 713.78 |
| PBR / SkyboxDraw | 17.96 / 19.60 | 17.96 / 19.60 |
| GI / OffScreen | 735.92 / 756.98 | 736.01 / 757.30 |
| GI / SceneLighting | 1021.13 / 1069.62 | 1022.57 / 1070.38 |
| GI / LightMarkers | 998.33 / 1030.28 | 998.33 / 1031.08 |
| GI / SkyboxDraw | 18.44 / 19.70 | 18.48 / 19.78 |

These overlapping timestamp intervals are not additive. No render pass or presentation-copy shader changes in this follow-up. [performance-summary.json](../build/rhi-deferred/review-fixes/performance-summary.json) preserves every run's per-pass CPU and GPU distributions.

### VSync and profile integrity

Eight further interleaved runs compare separate and combined presentation at 2560 × 1440, mode 3, async compute off, FIFO on the 165 Hz display. Each uses 60 warm-up and 600 measured frames, two runs per variant and RHI execution mode. This follow-up uses the normal 2048² G-buffer and default pool budget; it does not repeat the earlier specially configured 4096² saturation test.

| RHI mode | Separate CPU median / p95, ms | Combined CPU median / p95, ms | Frames > 1.5 refreshes, separate / combined |
| --- | --- | --- | --- |
| Inline | 6.1266 / 6.8589 | 6.0321 / 6.8402 | 0 / 0 of 1200 each |
| Threaded | 6.1304 / 6.9618 | 6.0450 / 6.8818 | 1 / 0 of 1200 each |

No pacing regression is observed. GPU frame intervals under VSync include pacing effects and do not establish GPU utilization. Full CPU, RHI and GPU values are in [pacing-summary.json](../build/rhi-deferred/review-fixes/pacing-summary.json).

All **156 profiles** (64 timing, 84 allocation, 8 pacing) pass `validate_engine_profile.validate(..., require_gpu=True, require_frame_gpu=True)`, including frame/pass accounting, summary statistics, available GPU frame timing and saved configuration fingerprints. See [profile-validation.json](../build/rhi-deferred/review-fixes/profile-validation.json). The source snapshots match the restored files exactly; `Data/engine.cfg` retains the original SHA-256 after every phase. The full correctness and performance follow-up is complete on this machine, subject to the hardware/platform limits above.

Commands used after the Debug/Release default builds (the scripts contain exact executable paths and arguments):

```powershell
python build/rhi-deferred/review-fixes/validate.py correctness
python build/rhi-deferred/review-fixes/build_variants.py
python build/rhi-deferred/review-fixes/validate.py performance
python build/rhi-deferred/review-fixes/validate.py memory
python build/rhi-deferred/review-fixes/validate.py pacing
python build/rhi-deferred/review-fixes/summarize.py
python tools/test_engine_profile.py
```
