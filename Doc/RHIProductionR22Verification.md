# RHI production readiness: R22 verification

Status: Implemented, 2026-10-03, in commit `8b59a3d0`. Part of [RHIProductionTODO.md](RHIProductionTODO.md#r22-fixed-bindless-heap-capacity). Based on `ee20bf99`.

## Result

The global bindless heap sizes are no longer compile-time constants.

| Done-when condition | Implementation |
| --- | --- |
| The capacities are queryable. | `RHIGPUInfo::bindlessHeapCapacities` reports the slots the backend created per heap, or zero for an unavailable heap. The executor caches it with the rest of `RHIGPUInfo`. |
| The capacities can be configured before initialization, and initialization checks them against the device limits. | `RHIOptions::SetBindlessHeapCapacities` takes an `RHIBindlessHeapCapacities` and rejects a heap with zero slots. `VulkanBindlessDescriptorPoolManager::Init` copies the request, so later option changes do not resize a live heap. `VulkanDevice::GetUnsupportedReason` and the manager share one check, `BindlessHeapsFitLimits`, which sums counts in 64 bits. Device selection reports the requested counts in its rejection reason. |
| RenderCore checks a scene's texture and sampler count at load time. | `GetSceneBindlessCapacityError` sits next to `BindSceneTextureArray` in `RendererUtils.h` and checks that slot layout: textures from 2D slot zero, scene samplers after the fallback sampler in sampler slot zero. The `RenderScene` constructor runs it first, before changing the scene or allocating anything. A scene that does not fit stops the process through `VERIFY_EXPR_MSG_F`, which writes the error message and aborts; the engine does not use exceptions for this. |
| A test loads a scene above the default capacity. | Two tests build a scene with 2049 textures, one more than the default 2D heap. With default heaps, construction aborts with the message (death test). With a 2049-slot 2D heap, the scene constructs and loads all 2049 textures, and a compute pass binds the whole scene texture array through the real backend. |

Defaults are unchanged: 2048 2D textures, 64 cube textures and 128 samplers. Shaders already declare the heaps as runtime-sized arrays, so no shader changed. The demo accepts `--bindless-textures=N` and `--bindless-samplers=N` (N > 0). The [RHI README](../ZenCore/Include/Graphics/RHI/README.md) describes the contract.

Changed files: `RHICommon.h` (`RHIBindlessHeapCapacities` and the `RHIGPUInfo` field; `ToUnderlying` and `RHIBindlessHeapType` moved above `RHIGPUInfo`), `RHIOptions.h`, `VulkanDescriptorPool.h/.cpp` (`kBindlessHeapCapacity` and `GetBindlessHeapCapacity` removed), `VulkanDevice.cpp`, `VulkanContext.cpp`, `RendererUtils.h`, `RenderScene.cpp`, `SceneRendererDemo.cpp`, the RHI README, and tests.

## Tests

New tests:

| Test | Executable | Covers |
| --- | --- | --- |
| `RenderCoreBindlessCapacityTest.SceneCheckFollowsTheTextureArraySlotLayout` | RenderCoreTest | Texture and sampler limits of the scene slot layout, including the fallback sampler slot and absent heaps |
| `RenderCoreBindlessCapacityTest.OptionsRejectEmptyHeapsAndKeepTheEarlierRequest` | RenderCoreTest | Zero-slot rejection; an accepted request replaces the previous one |
| `VulkanBindingIntegrationTest.DeviceSelectionRejectsBindlessHeapsBeyondDescriptorLimits` | VulkanRHIIntegrationTest | A `UINT32_MAX` 2D request is rejected with the bindless reason (no 64-bit wrap); the default request is accepted; a live backend keeps its heaps |
| `VulkanBindlessCapacityIntegrationTest.ConfiguredHeapsSampleSlotsBeyondTheDefaultCapacity` | VulkanRHIIntegrationTest | With 2304-slot 2D and sampler heaps, a compute dispatch samples texture and sampler slot 2303 and reads back the expected texel; slot 2304 is rejected |
| `BindlessSceneCapacityDeathTest.SceneLoadAbortsWhenTexturesExceedTheHeap` | VulkanRHIIntegrationTest | With default heaps, constructing a 2049-texture scene aborts and prints the capacity message. Only the death-test child creates a device |
| `BindlessSceneCapacityIntegrationTest.ConfiguredHeapLoadsAndBindsTexturesBeyondTheDefault` | VulkanRHIIntegrationTest | With a 2049-slot 2D heap, the scene loads all 2049 textures and a compute pass binds them through the real backend |

Six existing tests that used `GetBindlessHeapCapacity` now read `QueryGPUInfo().bindlessHeapCapacities`.

## Results

Environment: Windows 10 Pro 10.0.19045, AMD Radeon RX 7900 XT (raw driver version 8388981), MSVC 2022 Ninja presets `x64-windows-msvc-debug` and `x64-windows-msvc-release`. GPU executables ran as `7zFM.exe` copies to exclude the RTSS overlay. `tools/verify_rhi_production.py` sets `VK_LAYER_VALIDATE_SYNC=1` and disables implicit layers.

| Check | Debug | Release |
| --- | --- | --- |
| Build of `VulkanRHIIntegrationTest`, `VulkanRHITest`, `RenderCoreTest`, `CommonTest`, `scene_renderer_demo` | Pass. No new warnings; only the existing H1 `[[nodiscard]]` warnings and the third-party gli macro warning | Same |
| `VulkanRHITest` | 45 passed | 45 passed |
| `RenderCoreTest` | 542 passed (540 before, plus 2 new) | 542 passed |
| `CommonTest` | 111 passed, 1 skipped | 111 passed, 1 skipped |
| `VulkanRHIIntegrationTest` | 333 passed (329 before, plus 4 new), 6 capability skips | 334 passed (adds the Release-only F6 test), 6 capability skips |
| Smoke matrix: modes 1–3 × `--rhi-thread=0/1` × `--async-compute=0/1`, 44 frames | 12 of 12 pass | 12 of 12 pass |
| Validation and synchronization-validation messages | 0 | 0 |

Command: `python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-<config> --output <dir> --smoke --executable-alias 7zFM.exe`.

Additional Release checks under the same environment:

- `--mode=3 --async-compute=1 --smoke-test --frames=44 --fixed-step --vsync=0 --bindless-textures=4096 --bindless-samplers=256`, with `--rhi-thread=1` and `--rhi-thread=0`: both exit 0 and log `Bindless heaps: 4096 2D textures, 64 cube textures, 256 samplers`, with no validation messages.
- `--bindless-textures=0` exits 1 and prints the usage line, which lists both new flags.
- Default-heap captures (`--mode=1/2/3 --frames=8 --fixed-step --vsync=0 --capture=...`) are byte-identical to captures from a Release build of `ee20bf99` taken the same way, both before and after switching the scene rejection from an exception to an abort. SHA-256 for modes 1, 2 and 3: `2f9890cd…`, `08b5aa62…`, `8d81caff…`. They differ from the older R5 baseline hashes because `ee20bf99` changed automatic camera framing, not because of this change.

## Follow-up verification, 2026-10-03

The implementation and all four done-when conditions were rechecked after commit `8b59a3d0`. No further R22 code changes were needed.

| Focused check | Result |
| --- | --- |
| Debug `VulkanRHIIntegrationTest`: the four R22 tests listed above | 4 passed; no validation or synchronization-validation messages |
| Release `VulkanRHIIntegrationTest`: the same four tests | 4 passed; no validation or synchronization-validation messages |
| Release `RenderCoreTest`: `RenderCoreBindlessCapacityTest.*` | 2 passed |
| Debug `RenderCoreTest` rebuild | Initially blocked by existing uncommitted changes: `DynamicRHI`'s constructor and destructor moved to `RHIFactory.cpp`, which this target does not link. Resolved by the build fix below. |

The Debug integration target was up to date with the working tree. Release checks used the existing binaries from the original R22 verification, without rebuilding the unrelated working-tree changes. GPU runs used the same overlay exclusion and validation environment as the original verification. Commands, logs and results are in `build/rhi-production/r22-recheck-20261003/`. This focused rerun does not replace the original full-suite and smoke results above.

### Debug build fix, 2026-10-03

Moved the unchanged `DynamicRHI` constructor and destructor into `DynamicRHI.cpp`, compiled by both `ZenCore` and `RenderCoreTest`. The test target keeps its fake backend factory and does not link the Vulkan factory.

- The full `x64-windows-msvc-debug` build passes, including `RenderCoreTest` and `scene_renderer_demo`.
- `RenderCoreBindlessCapacityTest.*`, `RHIThreadTest.*` and `*RHILateRelease*`: 13 passed, including both R22 unit tests.
- The four R22 GPU integration tests pass again with validation and synchronization validation enabled, with no validation errors.
- The initial full Debug `RenderCoreTest` run exited with code 3 during `RenderCoreEnvironmentTest.EnvironmentReplacementRetainsAndRetiresAllOutputs`: `Resource released after RHI cleanup admission closed`. R23 subsequently fixed the fixture cleanup order while preserving the ownership check. Its final Debug and Release runs each pass all 554 RenderCore tests; see [R23 verification](RHIProductionR23Verification.md).

Logs and results: `build/rhi-production/dynamic-rhi-link-fix-20261003/`.

## Limits

- Only the AMD Radeon RX 7900 XT ran these tests; R8 still applies.
- The scene test runs headless, where environment loading is skipped (logged), and binds the scene textures in a compute pass rather than a full deferred frame.
- An oversized scene now ends the process with an error message. Loading one from the demo's model picker therefore exits the demo instead of keeping the previous scene, as other load errors still do.
- No performance measurement: the default heaps and the per-registration work are unchanged, and the only new per-call cost is reading the capacity from a member instead of a constant.
- Storage-image, buffer and 3D bindless heaps are still not provided; add them with an R13 consumer.
