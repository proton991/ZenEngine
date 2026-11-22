# Local library split verification

The local engine branch is `codex/library-split`, based on fetched `origin/hybrid-gi` at `887fa9da0cc55d5a797345dcd2bf30fe96741a3d`. The two new project trees are [ZenFoundation](../ZenFoundation/) and [ZenRHI](../ZenRHI/). The local split commits are `0e4feb0` on ZenTest `main` and `bd40f48` on the standalone ZenAssetProcessor `main`. Both await publication. The engine-local processor submodule still points at `a42624fec8e8a02cc355563863abd5a88f5ce71f`. Use `-DZEN_ASSET_PROCESSOR_SOURCE_DIR=E:/Dev/ZenAssetProcessor -DZEN_UPDATE_SUBPROJECTS=OFF` to reproduce the local source selection.

## Ownership

ZenTest source cases came from `44f991895f3d4215285c0ef370b28257e8fecf4e`. The processor-to-engine cases came from `tests/integration/zenengine` at the processor revision above. ZenSample was at `4bbb2df933c202a86412b1c46e7200a301635635`. Preserve these revisions alongside any future history extraction.

| Source suites or programs | Current home and retained coverage |
| --- | --- |
| SmartPtrTest, FlatHashMapTest, LRUCacheTest, CommonTest container, allocator, locking and verification cases | ZenFoundationTests. The arena overflow reuse case moved with PoolAllocator. The Foundation-only pointer cases cover ownership and destruction. ResourceOwnershipTest in ZenEngine covers the final release on the RHI thread. The standalone log-only ThreadPoolTest program is superseded by drain and restart assertions. |
| RenderCoreTest recording, worker and timing cases | ZenRHICoreTests owns command recording, worker scheduling, completion, tracking and deferred release. RenderCoreTest in ZenEngine owns frame slots, graph retirement, scheduling and rendering handoffs. |
| VulkanRHITest and VulkanRHIIntegrationTest | ZenRHIVulkanTests and ZenRHIVulkanIntegrationTests own the backend and their SPIR-V fixtures. ZenRHI exercises swapchain negotiation, resize, retirement and failed presents with a native test window. ZenEngine keeps engine shader contracts, the engine window-thread callbacks and RenderCore presentation policy. Generic reflection and engine-shader layout expectations exercise different inputs. The engine consumes the shared fake backend through `ZenRHI::TestSupport`. |
| RuntimeLaunchTest, SceneAssemblyTest, WindowPlatformTest, UIPlatformTest, UIRenderingContractTest, UIRenderingNativeTest, editor input, preview, asset preparation and rendering | ZenEngine registers these from [tests/integration](../tests/integration/). The ImGui shader layout check runs with the rendering contract. |
| InputControllerTest, ConfigLoaderTest, PythonToolTests, UIDrawPacketTest, EditorModelTest, ZenAppStartupOptions | ZenTest keeps the single-component tests. ZenAppStartupOptions invokes only the CLI parser. |
| ZenAssetEngineTests, ZenAssetTextureIntegrationTest, EditorAssetRenderingTest | ZenEngine owns the opt-in CPU adapter, GPU texture upload and editor render comparisons. Shared semantic inputs remain in ZenAssetProcessor/tests/fixtures. Processor-only format, CLI, import, geometry-budget and package tests run from the standalone processor. |
| EncoderAllocationBenchmark, RayQueryIntegrationTest, ConeVoxelGIIntegrationTest, GLTFCorpusImport, SceneLoadBenchmark, ScenePackageSmoke, ScenePackageBenchmark, SceneUploadBenchmark | Separately runnable engine programs. The benchmark targets remain outside the ordinary CTest list. |

## Threading and local builds

ZenApp defaults to `RHIExecutionMode::eThreaded` in [RuntimeLaunchSettings.h](../ZenCore/Include/Runtime/RuntimeLaunchSettings.h). Launch settings and `--rhi=inline` can select inline execution. Standalone `CreateRHI` returns a raw device. The caller constructs one executor and selects its mode explicitly. [Triangle](../ZenRHI/examples/Triangle/Triangle.cpp) exercises threaded recording and presentation.

Engine checks use the MSVC x64 Debug and Release presets with the processor override, `BUILD_TESTING=ON`, `ZEN_BUILD_ASSET_TOOLS=ON` and `ZEN_BUILD_ASSET_INTEGRATION_TESTS=ON`. Library checks use independent Ninja build directories, a Release Foundation install prefix and the Vulkan SDK. Standalone tests use their own shader inputs.

## Windows verification results

| Check | Observed result |
| --- | --- |
| Baseline engine Debug CTest | All 25 suites passed. The 18 CPU suites ran with SwiftShader selected and the seven native or Vulkan suites ran on hardware. |
| Baseline engine Release CTest | 24 of 25 suites passed. EditorRenderingTest reached its 120-second timeout. |
| Split engine Debug and Release | All 26 registered suites passed in each configuration, including the three opt-in asset consumer suites, before the final test-ownership edits. See the [full PR check](../build/checks/pr-20261122-102320/summary.json). |
| Final engine test refactor | The full Debug and Release builds succeeded. All 17 CPU/policy suites and three window/platform suites passed in each configuration with SwiftShader selected. After the reboot all six Vulkan suites passed on hardware in each configuration, completing all 26 registered suites on the final tree. See `build/library-split/final-{cpu,platform}-{debug,release}.log` and the hardware logs for [Release](../build/library-split/recovery-release.log) and [Debug](../build/library-split/recovery-debug.log). Formatting passed on 553 changed C++ and GLSL files, and the no-exceptions policy checked 634 owned C++ files. |
| Post-review source builds | All 26 registered suites passed in both [Debug](../build/library-split/review-debug-verified-tests.log) and [Release](../build/library-split/review-release-verified-tests.log). |
| Standalone Foundation | All 83 allocator, container, ownership, locking and verification cases passed. |
| Standalone RHI | 135 Core and 80 CPU-side Vulkan cases passed. The native suite passed 409 cases and skipped five on capability or topology grounds. All three reported zero validation errors. Per-suite logs are in `build/library-split/rhi-owners-results/`. The public-header check and relocated CMake consumer passed. |
| Software-driver run | Confirmed the SwiftShader Subzero device with vulkaninfo. CommonTest, RenderCoreTest, VulkanRHITest, ZenRHICoreTests and ZenRHIVulkanTests passed with zero validation errors. The local driver came from Chrome. The workflow uses its own pinned SwiftShader revision. |
| ZenApp smoke | Inline and threaded RHI each passed with async compute requested off and on. All four runs used the same setup and reported zero validation errors. |
| Packaging | The independent RHI source override configured and built. An external copy of Triangle configured against a relocated SDK and presented a native 640×480 frame with zero VMA leaks. The engine ZenCore target built against installed Foundation and RHI packages. |
| ZenAssetProcessor | All 46 standalone CTest entries passed. Both installed-processor consumer tests passed after moving the SDK to a different prefix. The format and reader public headers keep their standard-library types. |

Source builds also provide `VulkanRHITest`, `VulkanRHIIntegrationTest` and `SceneUploadBenchmark`, which inspect backend internals. Installed-package builds omit these targets. The remaining engine integration suites use the installed SDK, including the optional `TestSupport` component for the fake backend and scoped Vulkan hooks. All [24 installed-package CTest entries](../build/library-split/review-installed-verified-tests.log) passed with `ZEN_RHI_SOURCE_DIR` set to a nonexistent checkout. MSVC dependency records resolved both support headers under the installed prefix.

The concurrent source loader case disables its package cache so both worker counts import the same source. Dedicated cache cases cover publication and reload.

The packed-alpha compute GI comparison uses the current distance-field renderer. Packed and standalone inputs changed 999 of 196,608 output channels by at most 2/255 on this GPU in both execution modes. Repeating an identical input was bit-exact. The test accepts at most two byte levels and one percent of channels for that comparison. Material-only, hardware ray query and repeat comparisons remain bit-exact. The native hardware triangle-alpha probe still checks the compute and fragment query paths directly.

## 4K GPU tracing

Nsight Graphics 2026.3.1 captured 30 and 120 frames per run on a GeForce RTX 5080, driver 621.35, with clocks locked to base. Each capture launched `build/x64-windows-msvc-release/bin/ZenApp.exe`. The pre-split build linked directly to that same path. The runner verified fullscreen 3840×2160 presentation and rendering, a 1920×1080 GI pass, hardware rays, threaded RHI, GPU indirect culling and disabled async compute. The input setup SHA-256 is `ecdc40e433474a2d7c84b3713ad8a03b402e542c7e4dc58301b615532eeab7af`. The render package SHA-256 is `8a0dee387c4cc911a541e3a8b89719fecd30d2ea6c9f20f816065303cad90a6a`.

Each capture directory under `build/library-split/trace-4k-*` contains `application-command.json`, `command.json`, `inputs.json`, `summary.json`, the native GPU trace and exported per-frame samples. The input manifest identifies the executable, profiler, setup, package and shaders. The application arguments were `--setup=<capture>/setup.zenrender --backend=hardware --fullscreen=1 --width=3840 --height=2160 --rhi=threaded --async-compute=0 --gpu-culling=1 --vsync=0 --validation=0 --fixed-step --no-ui --gpu-markers --frames=90030`. The 120-frame runs used `--frames=90120`.

The control runs used the pristine archive of the engine and the pinned ZenTest sources. All 671 shader binaries in that baseline shader directory match the corresponding split binaries by SHA-256. The control manifests also include `control-shader-inputs.json` for the archive's actual shader directory.

| Capture | Frames | Median (ms) | Mean (ms) | p95 (ms) |
| --- | ---: | ---: | ---: | ---: |
| [Control A](../build/library-split/trace-4k-control-a/summary.json) | 30 | 32.050 | 32.346 | 34.254 |
| [Control B](../build/library-split/trace-4k-control-b/summary.json) | 30 | 32.138 | 32.327 | 34.229 |
| [Preliminary split](../build/library-split/trace-4k-split/summary.json) | 30 | 31.865 | 32.250 | 34.113 |
| [Split A](../build/library-split/trace-4k-final-a/summary.json) | 30 | 32.324 | 32.554 | 34.225 |
| [Split B](../build/library-split/trace-4k-final-b/summary.json) | 30 | 33.506 | 32.730 | 33.583 |
| [Control A](../build/library-split/trace-4k-120-control-a/summary.json) | 120 | 31.834 | 32.228 | 34.541 |
| [Control B](../build/library-split/trace-4k-120-control-b/summary.json) | 120 | 31.748 | 32.098 | 33.568 |
| [Split A](../build/library-split/trace-4k-120-split-a/summary.json) | 120 | 32.260 | 32.525 | 34.004 |
| [Split B after reboot](../build/library-split/trace-4k-120-split-b-recovered/summary.json) | 120 | 31.797 | 31.894 | 33.888 |

The post-reboot split median of 31.797 ms falls between the two control medians. The first split capture was 0.426–0.513 ms slower than those controls. Both split p95 values fell within the control range. The earlier median increase did not reproduce. The two split medians differ by 0.463 ms, exceeding the 0.087 ms control-to-control spread, so these measurements do not establish equivalence at that precision.

The setup and package hashes, actual render state, and all 38 GPU marker names and occurrence counts matched across the four long captures. The `GIDenoiseStabilize` marker exceeded 2 ms on 23 and 38 control frames and on 50 and 8 split frames respectively. Its two timing clusters account for some frame-time variation. The captures do not establish why its frequency changed.

The 120-frame order was split, control, control, a failed split launch, then the post-reboot split repeat. The control executable SHA-256 was `2461f251f1256d5954d01947e70fe5e2508fd44e847751e2b9c9d3d2059b0706`. The first split executable was `b5a30ffc9ae3342cb4705102282fed80c7cec44e450e569ecdd28c0ad7a453d8`. On November 22, 2026 at 11:01:19 KST, Windows recorded NVIDIA driver event 153 with a bus-reset TDR (GPU timeout). The last control summary was written at 11:01:16. The split application launched at 11:01:37, after the reset. Vulkan could enumerate no hardware devices and `nvidia-smi` reported the GPU lost. That launch produced no trace. Windows restarted at 11:11 KST, after which both Vulkan and `nvidia-smi` recognized the RTX 5080. The cause of the reset remains undetermined.

The repeat used the restored split executable, SHA-256 `b34bd30dede4557843dd2cf7c014b645108963560467557ad2e04989dcf54f6b`. Once Nsight saved the trace, ZenApp received a normal window-close request and exited with a clean allocator report. Nsight finished exporting. Vulkan and `nvidia-smi` still enumerated the RTX 5080 afterward, and Windows recorded no further NVIDIA event 153 during verification. [120-frame native trace](../build/library-split/trace-4k-120-split-b-recovered/ZenApp_2026_11_22_11_34_53.ngfx-gputrace)

## Additional native acceptance case

The separately runnable RayQueryIntegrationTest reported 140 passes, four skips and four failures, with zero Vulkan validation errors. All failures were `TransparentLayersKeepTheirShadowsAndOpaqueReceiverGuides` in the translated-instance scenario. A separate build of the untouched engine and the pinned ZenTest commit reproduced all four failures. Resetting frame history and giving both ray paths equal preparation time also reproduced the pixel difference. The engine keeps the full-image equality requirement. Logs are in `build/library-split/rayquery/`, `forward-base.log` and `forward-history.log`. The recorded ray-query run preceded moving two backend-only invalid-build assertions to the standalone Vulkan suite. That suite passed with the assertions in their new home. Both the [post-reboot hardware run](../build/library-split/rayquery-recovery/RayQueryIntegrationTest.log) and the [post-review Release run](../build/library-split/review-ray-query/RayQueryIntegrationTest.log) reproduced exactly those four baseline failures across the inline/threaded and async-compute settings, with 140 passes, four skips and zero validation errors. In the post-review run the first mismatch was pixel 4084 in every mode. The ray-query output was `0xff6b2828` and the ray-tracing pipeline output was `0xff660505`. Metal and hosted CI still require their respective environments.

The first post-review Debug run overlapped compilation. `EditorRenderingTest` exceeded its 120-second limit, and `SceneAssetCache.ForcedRecookRejectsInputChangesDuringTemporaryWrite` missed the temporary-file write within its polling window. On the serialized rerun the editor suite passed in 98.47 seconds and the asset suite passed in 35.80 seconds. [First Debug run](../build/library-split/review-debug-tests.log), [serialized Debug run](../build/library-split/review-debug-verified-tests.log).

## Processor boundary

ZenAssetProcessor links Foundation privately. The migration includes import workers, glTF and FBX scratch storage, geometry permutation and quantization, texture and atlas planning, and build and cleanup plans. The worker queue keeps its bounded admission and shutdown behavior. Standard-library containers remain at public scene, reader and external codec interfaces, including buffers moved directly into a public scene. The independently installable reader and format libraries continue to build without Foundation.

The import, processing and processor implementation directories contain 181 direct `std::vector` uses after the migration, down from 322. The format, reader and writer retain their existing 61 uses and have no `HeapVector` dependency. The cook timings below measure wall-clock time separately.

### CPU and package measurements

Three alternating runs used the same New Sponza glTF source with `zenassetc cook --profile reference --workers 8`. The timing covers the entire CLI invocation and package write. The encoder benchmark recorded 10,000 draws and 20 samples per run.

| Measurement | Baseline, three runs | Split, three runs |
| --- | --- | --- |
| New Sponza cook, seconds | 7.822, 6.459, 6.720 | 6.440, 6.669, 6.306 |
| Encoder, ns/command pair | 21.910, 22.190, 21.940 | 21.650, 21.880, 22.050 |
| Encoder allocations per pair | 0 in every run | 0 in every run |

Each package contains 155 nodes, 115 meshes, 2,049,137 vertices, 11,241,054 indices, 28 materials and 72 images. Both packages are 5,164,547,824 bytes. A streaming byte comparison covered the entire files. All differences were confined to the 64-character processor build fingerprint in record 216 and its directory and header checksums. All 479 other directory entries and their payloads matched.

- Baseline SHA-256 `7b39a406d15a25e17552034c8097031049344a6c69b6ff392c8446d73cdfc21e`
- Split SHA-256 `a3fc7ce9690c4a4e9dae9d53c8c5a088f92297a0f96355d2c50a1950b4c6c875`

The benchmark inputs, raw timing runs and full hash results are available in `build/library-split/cpu-performance-final.json` and `payload-final.log`.

## Publication

Every component was set to version 0.2.0 before publication. The libraries were published on 2026-11-22 as single import commits; the history of their files before the split remains in ZenEngine at `887fa9da`.

| Repository | Published `main` | Before publication |
| --- | --- | --- |
| [ZenFoundation](https://github.com/proton991/ZenFoundation) | `4891003` | All 83 standalone tests passed. |
| [ZenRHI](https://github.com/proton991/ZenRHI) | `7b8ccd1` | The standalone Core, CPU-side Vulkan and public-API suites passed. ZenRHI configured against an installed ZenFoundation 0.2.0 and rejected a 0.3.0 checkout. |
| [ZenAssetProcessor](https://github.com/proton991/ZenAssetProcessor) | `5c3fed4` | The engine build compiled it from source against ZenFoundation 0.2.0. Its standalone build fetches ZenFoundation `v0.2.0`, which needs that tag. |

ZenEngine registers both libraries as submodules on `main`, pinned at the commits above. Remote CI results are not part of this record.
