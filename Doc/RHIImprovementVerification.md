# RHI improvement verification

Executed on 2026-10-01 from `d22e0641`. Phases 0–6, 8 and 9 are implemented. Phase 7 was measured and reverted because it did not satisfy its acceptance gate. The optional small-pool redesign was not attempted. The original untracked improvement plan and the staging area are preserved; no changes were staged or committed. Error/failure handling and the other deferred designs remain outside this change.

The available Windows builds, full suites, renderer matrix and image checks pass. Hardware-dependent and cross-platform checks that could not be exercised are listed below. Performance results include the observed variation; they do not establish a GPU speedup.

## Environment and reproducibility

- Windows x64; VS 2022 Professional, MSVC 19.44.35222; Ninja; Vulkan SDK 1.4.304.0.
- AMD Radeon RX 7900 XT, 20,464 MiB device-local memory; separate graphics, compute and transfer queue families. The enumerated integrated AMD GPU was not selected by the native runs.
- Existing pinned dependency sources were reused with `FETCHCONTENT_FULLY_DISCONNECTED=ON` after the Basis Universal archive download repeatedly stalled. Dependency versions and hashes were not changed. Fresh configuration no longer declares or fetches SPIRV-Cross or TinyGLTF.
- Profiling uses Sponza, voxel resolution 256, 1280 × 720, fixed step, VSync off, validation off, 60 warm-up frames and 600 measured frames. Only CSV rows with `phase=measured` contribute to the tables. p95 uses the nearest-rank definition; medians average the two central samples when necessary.
- Correctness and renderer runs enable Vulkan validation and synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`). Implicit layers are excluded with `VK_LOADER_LAYERS_DISABLE=~implicit~`.

`Data/engine.cfg` was restored byte for byte after both image tools. Its baseline/final SHA-256 is `c09a51bce3cc624cd1b66d3d90fb26072089bf22aa8c6c4ad0ac5aa483bf7871`.

From an x64 VS developer shell, configure and build each available preset:

```powershell
cmake --fresh --preset x64-windows-msvc-debug -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build/x64-windows-msvc-debug -j 8
cmake --fresh --preset x64-windows-msvc-release -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build/x64-windows-msvc-release -j 8
cmake --fresh --preset x64-windows-msvc-performance -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build build/x64-windows-msvc-performance -j 8
```

The disconnected flag assumes the pinned source directories already exist. All three fresh configurations and default builds returned zero, including ZenCore, ZenUI/runtime UI, GLTFCorpusImport, the renderer and test executables. The later smart-pointer fixture update was rebuilt in all three configurations. Logs: [build results](../build/rhi-improvement/final/build-results.json), `build/rhi-improvement/final/*-configure.log`, `*-build.log`, and `build/rhi-final-smartptr-*-build.log`. Existing dependency diagnostics include GLI's `GLM_ENABLE_EXPERIMENTAL` redefinition and upstream CMake warnings; no new owned-code compiler warnings were identified.

Repeat a profiling combination with:

```powershell
.\build\x64-windows-msvc-release\bin\scene_renderer_demo.exe --mode=3 --frames=600 --warmup=60 --fixed-step --vsync=0 --disable-validation --rhi-thread=1 --profile=build/rhi-improvement/final/m3-t1
```

Repeat with modes 2/3 and RHI thread 0/1. Exact commands, exit codes, logs and CSV files are in each phase directory under `build/rhi-improvement/`. The final two batches ran without concurrent builds or other test batches. A Phase-8 batch that overlapped a Debug build was discarded and rerun.

## Implemented behavior and regression coverage

| Phase | Result and evidence |
| --- | --- |
| 0 | Permanent cumulative relaxed-atomic native draw/dispatch/submit, descriptor-cache and bindless-capture counters; per-frame deltas and RHI execution time in the existing profiler. `--rhi-counters=0/1` supports cost comparisons. |
| 1 | Transfer requests return independent owned contexts. Every declared format has a shared logical texel size; RGB8 is Vulkan value 23, BGRA is named, unknown formats return zero, combined D16S8/D32S8 have 3/5-byte logical texels (RDG memory budgets count them at padded 4/8 bytes), and aspect-specific copy rules remain distinct. Images whose tightly packed texels could fit the 4 KiB small pool are probed for their native 64-bit memory requirement; larger images go straight to VMA without a probe. Debug Printf is opt-in with INFO logging. Stage names/counts, empty flags, the attachment boundary and transition defaults are corrected. Transfer recreation and format/header regressions pass. All 51 concrete format numbers and texel sizes were cross-checked against the installed SDK's `vk.xml`; [registry evidence](../build/rhi-improvement/format-registry-verification.json). |
| 2 | Required extension features and supported optional core features (pipeline/sampler state, independent blend) are selected explicitly; optional capabilities are published, and capture/replay/multi-device bits remain disabled. Robust access defaults off with a startup debugging option. Viewport depth prefers D32, including the matching depth-only backbuffer range. Uniform rings use coherent host-visible memory with device-local placement preferred and VMA fallback; staging retains host preference. All compiled shader capabilities were inventoried before cleanup in [spirv-capabilities.json](../build/rhi-improvement/spirv-capabilities.json). |
| 3 | Epoch captures belong to command lists and follow reset, rollback and detach. Published epochs and empty pending-write checks use atomics. Later draws in an already captured epoch take no bindless-manager lock. All 56 affected bindless/upload cases passed, including 100-draw capture reuse, rollback and detached retirement. Captures fell from 64,200 to 600 per measured interval. |
| 4 | Typed command dispatch and checked Vulkan casts remove RTTI from translation. Invalid resource types retain the existing validation outcome. Ordered dynamic-offset metadata, workload/cache/uniform-aware descriptor reuse, last-ID retention checks, an index-buffer cache, arena-owned parameter arrays, fixed attachment arrays and small barrier storage are implemented. All 94 affected binding/recording/descriptor/bindless cases passed; the negative sampler binding regression passes. |
| 5 | Inline and reentrant Invoke run directly. Final resource/context releases dispatch in FIFO order. CPU-access buffers stay mapped; capability queries read immutable state. Completion waiting, progress publication and collection share one invocation. Stack fixtures and destruction assertions explicitly complete queued releases, including the smart-pointer policy regression. |
| 6 | Completion processing queries a timeline once per call and collects lifetime batches once per maintenance/discard batch. Native command-buffer reset keeps driver storage. Both timeline and fence paths pass the full queue/lifetime tests. |
| 7 | Steady-state misses opened the experiment's gate. Searching all slots, preserving compatible sets and reusing incrementally hashed keys did not lower misses/inserts or establish a repeatable frame-time benefit. The entire experiment was reverted before Phase 8; the original cache policy remains. Raw experiment measurements are retained in `phase7/`. |
| 8 | Buffer sizes, required sizes, clear ranges/offsets, indirect offsets and native allocation offsets are 64-bit. The existing `ZEN_BUFFER_WHOLE_SIZE` is the explicit whole-buffer sentinel. Specialization constants preserve scalar type and raw 32-bit bits through reflection, overrides, Vulkan payloads and RenderCore pipeline keys. Recorded wide clears and native fractional-float dispatches (4.5 and −2.75) pass. Vulkan's dynamic uniform/push-constant offset widths remain 32-bit. |
| 9 | The obsolete renderer/dependencies/samples/shaders and remaining legacy RHI paths are removed. Dynamic rendering is the sole path; pipelines consume rendering layouts during initialization without retaining a borrowed pointer. Usage searches, fresh configurations, default builds, full suites and identical captures validate the cleanup. Inventory below. |

Phase 1 correctness fixes preceded the performance baseline, as allowed by the plan's independent ordering. Per-frame RHI/counter CSV sampling was added before Phase 3, so `phase2-metrics/` is the directly comparable pre-Phase-3 reference for those fields. The first baseline has CPU/GPU CSV samples and cumulative counter snapshots in its logs.

The Phase-3 wording about zero locks for an entire frame is stronger than its specified capture-once-per-list algorithm. This implementation follows that algorithm: the first capture takes the manager/tracker lifetime locks; repeated draws in that epoch take none. The recorded interval contains one capture per frame, rather than claiming zero total recording locks.

The RHI README documents transfer ownership, retained parameter arrays, epoch protection, asynchronous release, persistent mappings, feature/depth/allocation policy, scalar widths and the dynamic-only rendering path.

## Final correctness results

Counts below are identical in Debug and Release:

| Suite | Passed | Capability skips | Failures |
| --- | ---: | ---: | ---: |
| CommonTest | 98 | 1 | 0 |
| RenderCoreTest | 526 | 0 | 0 |
| VulkanRHITest | 41 | 0 | 0 |
| VulkanRHIIntegrationTest | 292 | 6 | 0 |
| SmartPtrTest | 34 | 0 | 0 |
| FlatHashMapTest | 6 | 0 | 0 |
| LRUCacheTest | 12 | 0 | 0 |
| InputControllerTest | 7 | 0 | 0 |
| ConfigLoaderTest | 21 | 0 | 0 |
| UIDrawPacketTest | 8 | 0 | 0 |
| RuntimeUIIntegrationTest | 10 | 0 | 0 |
| SceneModelSwitchTest | 8 | 0 | 0 |
| ConeVoxelGIIntegrationTest | 4 | 0 | 0 |

RenderCoreTest also lists seven pre-existing `DISABLED_` benchmarks, which do not run. The six native skips are four cases requiring swapchain-maintenance presentation fences, one requiring the EXT surface-maintenance dependency, and one requiring D24S8. The timeline and fence submission variants both ran. Intentional fault-injection/invalid-input tests produce expected engine error logs; their assertions pass. Final native/UI runs have zero Vulkan/synchronization-validation errors.

All 12 mode/thread/async combinations passed in each configuration (24 runs): modes 1/2/3 × RHI thread 0/1 × async compute 0/1, 44-frame `--smoke-test` runs. This covers mode changes, repeated voxel updates, resize at frame 12, minimize/restore at frame 20, changing/removing lights and shutdown. Renderer logs have zero engine, Vulkan or synchronization-validation errors. [Suite/matrix results](../build/rhi-improvement/final/regression-results.json), [additional suite results](../build/rhi-improvement/final/additional-test-results.json).

```powershell
.\build\x64-windows-msvc-release\bin\scene_renderer_demo.exe --mode=3 --rhi-thread=1 --async-compute=1 --smoke-test --frames=44 --fixed-step --vsync=0
python -X utf8 -c "import sys; from pathlib import Path; sys.path.insert(0, 'tools'); import validate_voxel_gi as v; v.main(executable=Path('build/x64-windows-msvc-release/bin/scene_renderer_demo.exe').resolve())"
python -X utf8 tools/smoke_gltf_rendering.py --assets D:/Dev/glTF-Sample-Assets --renderer D:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/scene_renderer_demo.exe --output build/rhi-improvement/final/gltf-smoke
```

`validate_voxel_gi.py`: all 162 GPU cases and image assertions passed across geometry/compute voxelizers, execution/queue modes, direct/indirect/environment/emissive lighting, materials and surface layouts, shadow configurations and resolutions, light directions and moving/removable markers. The tool's room fixtures now explicitly enable configured-light overrides, so their `light_count=0/1` settings apply regardless of the user's baseline configuration. Local case/surface variables no longer overwrite the optional startup `overrides` parameter. The initial failed dark-room assertion was a fixture configuration failure, corrected without changing renderer behavior. [Image-check results](../build/voxel-gi-validation/results.json), [run log](../build/rhi-improvement/final/voxel-gi-validation.log).

`smoke_gltf_rendering.py`: all 18 representative assets passed with nonempty captures and zero validation errors, including Draco/Meshopt, BasisU/KTX, material/primitive variants, skinning, morph targets, animation, instancing and visibility. Representative BasisU and alpha-mode previews were inspected. These checks validate execution and capture integrity rather than certifying shading conformance. [glTF report](../build/rhi-improvement/final/gltf-smoke/report.json).

### Presentation overlay diagnosis

The baseline full native suite intermittently failed presentation with an invalid queue-family command-pool call/access violation, while isolated presentation runs passed. Existing local diagnosis from 2026-09-30 traces the presentation call into injected `RTSSHooks64.dll`; the engine has no command-pool creation on that stack. `DISABLE_RTSS_LAYER=1` alone does not disable the installed hook. The model-switch suite also produced synchronization errors referring to `vkCmdEndRenderPass`, which is absent from the final engine rendering path.

For affected suites, byte-identical copies named `7zFM.exe` use the already-installed RTSS exclusion (`EnableHooking=0`). SHA-256 identity was checked for every copy. No RTSS settings or engine presentation code were changed for this workaround. Both complete native suites and both model-switch suites pass with zero validation errors under that exclusion. Original overlay-hooked logs remain alongside the passing logs. [Prior diagnosis](../build/runtime-gi-full-suite-20260930/presentation-diagnosis/diagnosis.txt).

Native binary (`VulkanRHIIntegrationTest.exe`) SHA-256 before the post-review fixes:

- Debug: `e789a4b5e6dbbb8f0f6b0956b9aeaef214802f33f7398c62364a247ca4048807`.
- Release: `7448a867fe26dce66cd52bb7d7a5cb5f1c0f74c0aa28d7c1ed7c6ab0e7a11cca`.

After the post-review fixes:

- Debug: `668ea7687840d7c1ead154df2d9e35158bdaf2175521ab729d659c27c7b6072e`.
- Release: `37814e60b3328f1f5d6674dfb45788b33fad5f0698011453f38b821d43dc0b32`.

### Capture equivalence

Eight-frame fixed-step, validation-enabled captures are byte-identical between baseline, post-Phase-8, post-9.1 and the final tree in every mode. [Final comparison](../build/rhi-improvement/final/capture-comparison.json).

| Mode | SHA-256 |
| --- | --- |
| 1 | `c5533a5797fa9437766cccdd25e5a9cf86dfb71b66347ed44634e5ee22d891fa` |
| 2 | `3f1d60b1b0f553a4aa14f7058fe1c1f946f4ed5c9f71703a7421521d0d7b46f2` |
| 3 | `1b468e3890ec3a569ce9b64e94d963ff7194f5ad6e5f2aa41ebee082008d6a3b` |

## Performance observations

Each cell is median / p95 in milliseconds. Every interval contains 600 measured frames. Results use the same scene/configuration and profiler settings. The two final batches show run-to-run variation; neither is discarded or substituted for the other.

| Mode / RHI thread | Baseline CPU | Final CPU | Final repeat CPU | Baseline GPU | Final GPU | Final repeat GPU |
| --- | --- | --- | --- | --- | --- | --- |
| PBR / inline | 1.3958 / 1.7152 | 1.1471 / 1.3849 | 1.3075 / 1.6657 | 1.0981 / 1.1212 | 1.0883 / 1.1710 | 1.0877 / 1.1440 |
| PBR / threaded | 1.5935 / 1.8918 | 1.2229 / 1.3503 | 1.4159 / 1.6607 | 1.0981 / 1.1221 | 1.0897 / 1.1596 | 1.0885 / 1.1255 |
| GI / inline | 1.4876 / 1.8815 | 1.4567 / 1.7341 | 1.5133 / 1.8486 | 1.4012 / 1.4542 | 1.3984 / 1.4889 | 1.3916 / 1.4777 |
| GI / threaded | 1.6723 / 2.1675 | 1.4573 / 1.7159 | 1.4782 / 1.7767 | 1.3814 / 1.4228 | 1.3970 / 1.4896 | 1.3940 / 1.4692 |

The first final batch reduced CPU medians by 17.8%/23.3% in PBR and 2.1%/12.9% in GI (inline/threaded). The repeat shows smaller PBR gains (6.3%/11.1%), approximately neutral inline GI (1.7% slower) and improved threaded GI (11.6%). These are observed runs, not confidence intervals or isolated per-change speedups.

GPU medians remain within about 1.2% of baseline. GPU p95 is higher in these final batches, with variation between repeats; GPU tail neutrality or a GPU speedup has not been established. The kept device-configuration phase showed approximately neutral steady GPU frame/pass medians. CPU/RHI phase measurements below support the hot-path decisions; they do not establish that every individual optimization improves every mode. Small improvements and neutral results are accepted at this measurement resolution.

### Per-phase frame and RHI intervals

RHI execution measures elapsed ExecuteFrame batch time, including translation, submission, presentation and progress queries. It includes presentation waits and does not time separately queued release jobs. Its GI increase after Phase 5 was not isolated to a cause, and cannot be presented as a translation-time improvement. The baseline lacks per-frame RHI CSV sampling, so it is not assigned a fabricated median.

| Checkpoint | Mode / RHI thread | CPU median / p95 | RHI median / p95 | GPU median / p95 |
| --- | --- | --- | --- | --- |
| baseline | PBR / inline | 1.3958 / 1.7152 | not sampled | 1.0981 / 1.1212 |
| baseline | PBR / threaded | 1.5935 / 1.8918 | not sampled | 1.0981 / 1.1221 |
| baseline | GI / inline | 1.4876 / 1.8815 | not sampled | 1.4012 / 1.4542 |
| baseline | GI / threaded | 1.6723 / 2.1675 | not sampled | 1.3814 / 1.4228 |
| phase2 | PBR / inline | 1.3403 / 1.5702 | not sampled | 1.0871 / 1.1666 |
| phase2 | PBR / threaded | 1.1793 / 1.4838 | not sampled | 1.0867 / 1.2026 |
| phase2 | GI / inline | 1.4848 / 1.8380 | not sampled | 1.3960 / 1.5471 |
| phase2 | GI / threaded | 1.4944 / 1.8647 | not sampled | 1.3950 / 1.5492 |
| phase2-metrics | PBR / inline | 1.3212 / 1.5187 | 0.4115 / 0.4720 | 1.0837 / 1.1087 |
| phase2-metrics | PBR / threaded | 1.2612 / 1.4814 | 0.4075 / 0.5010 | 1.0841 / 1.1413 |
| phase2-metrics | GI / inline | 1.4522 / 1.7097 | 0.7005 / 0.9060 | 1.3962 / 1.4576 |
| phase2-metrics | GI / threaded | 1.4551 / 1.7592 | 0.5850 / 0.8210 | 1.3952 / 1.4591 |
| phase3 | PBR / inline | 1.3249 / 1.5492 | 0.4080 / 0.5710 | 1.0844 / 1.1308 |
| phase3 | PBR / threaded | 1.3416 / 1.5174 | 0.4170 / 0.5080 | 1.0871 / 1.1331 |
| phase3 | GI / inline | 1.4674 / 1.8248 | 0.5320 / 0.9310 | 1.3958 / 1.4906 |
| phase3 | GI / threaded | 1.4620 / 1.8217 | 0.4860 / 0.8270 | 1.3961 / 1.4779 |
| phase4 | PBR / inline | 1.3078 / 1.5165 | 0.4115 / 0.5290 | 1.0846 / 1.1314 |
| phase4 | PBR / threaded | 1.3517 / 1.5038 | 0.4200 / 0.5170 | 1.0855 / 1.1318 |
| phase4 | GI / inline | 1.4657 / 1.7854 | 0.5045 / 0.7770 | 1.3955 / 1.4789 |
| phase4 | GI / threaded | 1.4705 / 1.7892 | 0.4955 / 0.7940 | 1.3970 / 1.4878 |
| phase5 | PBR / inline | 1.1502 / 1.4246 | 0.4630 / 0.7260 | 1.0844 / 1.1677 |
| phase5 | PBR / threaded | 1.2349 / 1.4063 | 0.4320 / 0.6150 | 1.0861 / 1.1502 |
| phase5 | GI / inline | 1.4626 / 1.7980 | 0.7450 / 1.0590 | 1.3973 / 1.4818 |
| phase5 | GI / threaded | 1.4699 / 1.7781 | 0.6240 / 0.9370 | 1.3972 / 1.4911 |
| phase6 | PBR / inline | 1.1445 / 1.3936 | 0.5120 / 0.7460 | 1.0853 / 1.1684 |
| phase6 | PBR / threaded | 1.2313 / 1.5368 | 0.4405 / 0.7410 | 1.0866 / 1.1895 |
| phase6 | GI / inline | 1.4636 / 1.7645 | 0.7410 / 1.0250 | 1.3970 / 1.4948 |
| phase6 | GI / threaded | 1.4681 / 1.8645 | 0.6590 / 1.0580 | 1.3949 / 1.4982 |
| phase7 | PBR / inline | 1.1651 / 1.6147 | 0.5225 / 0.7900 | 1.0882 / 1.1727 |
| phase7 | PBR / threaded | 1.2020 / 1.6973 | 0.4350 / 0.7320 | 1.0877 / 1.1879 |
| phase7 | GI / inline | 1.4703 / 1.8241 | 0.7795 / 1.0510 | 1.3997 / 1.4944 |
| phase7 | GI / threaded | 1.4886 / 1.8602 | 0.6535 / 0.9860 | 1.3982 / 1.4949 |
| phase8 | PBR / inline | 1.1804 / 1.7900 | 0.4680 / 1.0640 | 1.0990 / 1.3734 |
| phase8 | PBR / threaded | 1.2271 / 1.7314 | 0.4275 / 0.9800 | 1.0964 / 1.3748 |
| phase8 | GI / inline | 1.5097 / 2.1318 | 0.7080 / 1.3890 | 1.4210 / 1.6968 |
| phase8 | GI / threaded | 1.4776 / 2.2779 | 0.6450 / 1.4480 | 1.4246 / 1.7602 |
| final | PBR / inline | 1.1471 / 1.3849 | 0.4570 / 0.6930 | 1.0883 / 1.1710 |
| final | PBR / threaded | 1.2229 / 1.3503 | 0.4250 / 0.5210 | 1.0897 / 1.1596 |
| final | GI / inline | 1.4567 / 1.7341 | 0.7330 / 1.0180 | 1.3984 / 1.4889 |
| final | GI / threaded | 1.4573 / 1.7159 | 0.6365 / 0.9360 | 1.3970 / 1.4896 |
| final-repeat | PBR / inline | 1.3075 / 1.6657 | 0.4720 / 0.6250 | 1.0877 / 1.1440 |
| final-repeat | PBR / threaded | 1.4159 / 1.6607 | 0.4900 / 0.5790 | 1.0885 / 1.1255 |
| final-repeat | GI / inline | 1.5133 / 1.8486 | 0.5900 / 0.9500 | 1.3916 / 1.4777 |
| final-repeat | GI / threaded | 1.4782 / 1.7767 | 0.5685 / 0.8840 | 1.3940 / 1.4692 |

Phase 3 removes repeated lifetime captures (107 → 1 per frame) and reduces the sampled GI RHI interval versus `phase2-metrics`; PBR frame samples are mixed. Phase 4 is approximately neutral, with lower inline GI execution time. Phase 5 lowers PBR whole-frame CPU time; GI whole-frame samples remain approximately neutral while the elapsed RHI batch interval increases. Phase 6 is approximately neutral versus Phase 5. The Phase-7 experiment retained 600 misses and 600 inserts in each interval and was rejected. Phase 8 and cleanup retain the established behavior. Raw machine-readable statistics: [frame summary](../build/rhi-improvement/measurement-summary.json), [pass summary](../build/rhi-improvement/pass-timing-summary.json).

### Steady GPU pass intervals

Each cell is median / p95 ms, 600 samples. Pass intervals can overlap and must not be summed into the GPU-frame interval. Startup/voxelization passes are retained in the raw CSV but are not treated as warmed steady-state comparisons.

| Mode / RHI thread | Pass | Baseline | Phase 2 | Final | Final repeat |
| --- | --- | --- | --- | --- | --- |
| PBR / inline | SkyboxDraw | 0.0194 / 0.0231 | 0.0179 / 0.0186 | 0.0180 / 0.0184 | 0.0179 / 0.0186 |
| PBR / inline | OffScreen | 0.7393 / 0.7590 | 0.7321 / 0.7548 | 0.7330 / 0.7524 | 0.7320 / 0.7511 |
| PBR / inline | SceneLighting | 0.6836 / 0.7052 | 0.6809 / 0.7113 | 0.6834 / 0.7168 | 0.6829 / 0.7169 |
| PBR / inline | LightMarkers | 0.6704 / 0.6890 | 0.6673 / 0.6888 | 0.6687 / 0.6874 | 0.6688 / 0.6885 |
| PBR / threaded | SkyboxDraw | 0.0194 / 0.0228 | 0.0179 / 0.0184 | 0.0180 / 0.0187 | 0.0180 / 0.0187 |
| PBR / threaded | OffScreen | 0.7404 / 0.7608 | 0.7314 / 0.7527 | 0.7336 / 0.7532 | 0.7329 / 0.7518 |
| PBR / threaded | SceneLighting | 0.6838 / 0.7042 | 0.6820 / 0.7208 | 0.6832 / 0.7141 | 0.6824 / 0.7068 |
| PBR / threaded | LightMarkers | 0.6707 / 0.6915 | 0.6664 / 0.6848 | 0.6693 / 0.6899 | 0.6688 / 0.6874 |
| GI / inline | SkyboxDraw | 0.0198 / 0.0205 | 0.0182 / 0.0186 | 0.0184 / 0.0187 | 0.0182 / 0.0186 |
| GI / inline | OffScreen | 0.7338 / 0.7526 | 0.7300 / 0.7519 | 0.7319 / 0.7508 | 0.7292 / 0.7506 |
| GI / inline | SceneLighting | 0.9923 / 1.0201 | 0.9901 / 1.0283 | 0.9951 / 1.0324 | 0.9856 / 1.0224 |
| GI / inline | LightMarkers | 0.9757 / 1.0013 | 0.9717 / 0.9955 | 0.9790 / 1.0006 | 0.9692 / 0.9980 |
| GI / threaded | SkyboxDraw | 0.0196 / 0.0240 | 0.0182 / 0.0186 | 0.0184 / 0.0187 | 0.0183 / 0.0186 |
| GI / threaded | OffScreen | 0.7324 / 0.7531 | 0.7292 / 0.7508 | 0.7310 / 0.7473 | 0.7299 / 0.7498 |
| GI / threaded | SceneLighting | 0.9709 / 0.9989 | 0.9884 / 1.0304 | 0.9922 / 1.0294 | 0.9905 / 1.0233 |
| GI / threaded | LightMarkers | 0.9567 / 0.9797 | 0.9688 / 0.9932 | 0.9760 / 0.9972 | 0.9748 / 1.0013 |

### Counters and their cost

Each final 600-frame interval records 63,600 native draws, no steady-state dispatches, 1,200 native submissions, 600 descriptor misses/inserts, zero ring-slot retirements and 600 bindless captures. PBR records 3,600 descriptor hits; GI records 4,800. Before Phase 3, the corresponding capture count was 64,200. The miss count explains why Phase 7's experiment was opened, and its unchanged result explains why it was reverted. Steady GI dispatches are zero because voxelization occurs during startup in this fixed scene; dynamic-update behavior is covered by the smoke and image suites.

Three alternating on/off pairs per threaded mode measured counter overhead without concurrent builds. Signs vary and timing ranges overlap, so no consistent counter cost was resolved by these runs. Counters remain enabled by default with a startup toggle; this small sample is not a statistical proof of zero overhead. [Counter cost data](../build/rhi-improvement/counter-cost/results.json).

| Mode | Repeat | Counters | CPU median ms | RHI median ms | GPU median ms |
| --- | ---: | --- | ---: | ---: | ---: |
| PBR | 0 | on | 1.2368 | 0.4600 | 1.1808 |
| PBR | 0 | off | 1.2010 | 0.4340 | 1.0865 |
| PBR | 1 | off | 1.2214 | 0.4330 | 1.0907 |
| PBR | 1 | on | 1.2272 | 0.4370 | 1.0966 |
| PBR | 2 | on | 1.2426 | 0.4425 | 1.0914 |
| PBR | 2 | off | 1.2578 | 0.4730 | 1.1748 |
| GI | 0 | on | 1.5263 | 0.6045 | 1.4108 |
| GI | 0 | off | 1.4927 | 0.6340 | 1.4194 |
| GI | 1 | off | 1.4719 | 0.6275 | 1.4025 |
| GI | 1 | on | 1.4755 | 0.6745 | 1.4012 |
| GI | 2 | on | 1.4751 | 0.5930 | 1.3988 |
| GI | 2 | off | 1.5493 | 0.6495 | 1.4101 |

## Cleanup inventory and final checks

115 tracked files were deleted in two groups:

| Group | Removed files |
| --- | ---: |
| Legacy wrapper (`Graphics/Val`) | 46 |
| Old RenderCore headers/sources outside V2 | 15 |
| TinyGLTF loader | 2 |
| Legacy applications, demos and shared application shell | 25 |
| Legacy-only/unreferenced shaders | 24 |
| RHIDefs and VulkanRenderPass header/source | 3 |

Phase 9.1 also removes the legacy build option/target blocks, SPIRV-Cross/TinyGLTF dependency declarations and links, and the active voxelizer's unused legacy include. Generated SPIR-V counterparts of deleted shaders were removed from the ignored output directory. Active shadow shaders and all textures are retained. Documentation preserves dated milestone history, notes the removed demos and updates the separate error-handling plan's legacy-cleanup item.

Phase 9.2 removes render-pass/framebuffer builders and caches, their viewport fields and handle casts, non-dynamic branches, the dynamic-rendering option, subpass/render-pass/clear types, write-only image-layout state, stored rendering-layout pointers and unused texture state. It also removes old handles, transition stubs, shader-source/compiler/hash/debug-print helpers, obsolete options/sampler duplication/ALLOCA/fence/secondary-command stubs and dead commented API. Active/tested helpers including ResolveTexture, RHIDebug and native buffer map/unmap helpers are retained. The formatting wrapper no longer carries legacy exclusions and ignores removed cached paths.

Searches for deleted paths/names find no active references outside the original plan, this report and preserved historical material. C++ sources in the active RHI/Vulkan backend contain no `dynamic_cast`. Production callers, fake contexts and native fixtures were updated with the contracts they exercise. Full suites pass after each cleanup group; all three mode captures match post-Phase-8 output.

All 66 modified owned C++ files were formatted with the repository `.clang-format` (clang-format 19.1.5 from VS 2022) and passed `clang-format --dry-run --Werror`. Whole-file formatting also re-indents a few untouched conditional expressions in those files, because HEAD was formatted with an earlier formatter version. Modified semantic functions were reviewed for terminal returns, explicit types, engine containers, lambda lifetimes and spacing. Existing unrelated functions were not rewritten. Both changed Python tools pass syntax checks and their relevant execution checks. `git diff --check` passes.

## Post-review fixes

An independent review of the finished change found the following; each is fixed in the working tree.

| Finding | Fix |
| --- | --- |
| Every image allocation created and destroyed a temporary `VkImage` to read its memory requirement, and logged a new error when that probe failed. | Allocation is the original single `vmaCreateImage` path again. A probe runs only when the image's tightly packed texel size (a lower bound on the native requirement) could fit the 4 KiB small pool. A failed probe falls back to the default pools, so failures report exactly as before the plan. |
| The shared texel table made RDG memory budgets count D16S8/D32S8 at 3/5 bytes instead of their padded 4/8. | `GetTextureFormatMemoryPixelSize` keeps the padded footprint for budgets; `GetTextureFormatPixelSize` stays the logical texel size. |
| `independentBlend` had become a new device requirement, undocumented in the RHI README. | It is optional again, enabled when supported and published as `RHIGPUInfo::supportIndependentBlend`. The README no longer mentions a legacy render-pass fallback. |
| Deletions left 35 runs of two or more blank lines, orphaned comments in `RHIResource.h` and `VulkanTexture.cpp`, a blank line inside an `if`/`else if` chain, and an edited commented-out render-pass declaration in `RenderDevice.h`. | Runs collapsed to one blank line, orphaned comments and the commented-out declarations removed. |
| This report counted 533 RenderCoreTest passes. | Corrected to 526; the seven disabled benchmarks do not run. |

Re-validation after the fixes, with Vulkan and synchronization validation enabled and every executable run from a `7zFM.exe` copy to exclude the RTSS overlay:

- Debug and Release build; the only compiler warning is the existing GLI `GLM_ENABLE_EXPERIMENTAL` redefinition. All 66 changed C++ files pass `clang-format --dry-run --Werror`.
- RenderCoreTest 526, VulkanRHITest 41, SmartPtrTest 34, CommonTest 98 (+1 skip) and VulkanRHIIntegrationTest 292 (+6 skips) pass in Debug and Release with zero validation errors. FlatHashMapTest, LRUCacheTest, InputControllerTest, ConfigLoaderTest, UIDrawPacketTest, RuntimeUIIntegrationTest, SceneModelSwitchTest and ConeVoxelGIIntegrationTest pass in Release.
- All 12 Release smoke combinations (modes 1/2/3 × RHI thread 0/1 × async compute 0/1) pass with no errors.
- The three 8-frame captures are byte-identical to the baseline, post-Phase-8 and final hashes above.
- Not repeated: performance captures, the Debug smoke matrix, `validate_voxel_gi.py` and `smoke_gltf_rendering.py`.

## Validation limits

- macOS/Clang and Linux/GCC presets were not configured or built on this Windows host.
- Native runs select the discrete GPU. Integrated-GPU uniform placement and exhaustion of a 256 MiB BAR heap without ReBAR were not exercised; allocation flags preserve VMA's coherent host-visible fallback.
- Swapchain-maintenance/presentation-fence and D24S8 cases skip on this driver. Both supported timeline/fence submission paths ran.
- The first bindless capture per command list still locks, as required by the chosen lifetime design. Per-draw locks and RTTI are removed.
- GPU p95 neutrality and statistically isolated per-optimization/counter overhead are not established by these short profile batches. Both final batches and every intermediate checkpoint are retained for inspection.
- The optional committed-memory comparison/custom-small-pool redesign was left out. Small-pool placement uses native requirements for images that could fit the pool.

No deferred error-handling, presentation redesign, Synchronization2 migration or submission-policy change was implemented.
