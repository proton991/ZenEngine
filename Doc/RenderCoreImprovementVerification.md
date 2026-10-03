# RenderCore improvement verification

Status: Phases 0–2 implemented, 2026-10-03, on top of `329d20be` (uncommitted). Phase 3 is not started; its gate is evaluated below. Implements [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) and closes item 5.5 of [RHIProductionTODO.md](RHIProductionTODO.md#55-per-frame-descriptor-miss).

## Result

Steady-state frames record zero descriptor-cache misses and zero render-graph pool misses at every measured size, including the 4096² stress case that Phase 2 removed. The deferred G-buffer now matches the viewport and is read with `texelFetch`.

| Phase | Outcome |
| --- | --- |
| 0 Baseline | Recorded (the "Phase 0" columns below). The demo profile now reports pool hits, misses and evictions. |
| 1 Steady working set | Implemented. Captures byte-identical to Phase 0. Descriptor misses 600 → 0 and pool misses about 1,200 → 0 per 600 frames. At a 4096² G-buffer the CPU frame fell from 7.3/9.2 ms to 1.3/2.9 ms (PBR/GI) and now tracks GPU time. Peak device-local memory fell by 256–1,282 MiB. |
| 2 Screen-sized G-buffer | Implemented. Images change by design; silhouettes no longer blend foreground and background. At 720p the G-buffer shrinks from 144 to 31.6 MiB per slot and the GPU frame falls 37% (PBR) and 14% (GI). At 2560 × 1421, GI costs 6–8% more GPU time; see [the 1440p GI trade-off](#1440p-gi-trade-off). |
| 3 Compact encoding | Not started. Its gate (G-buffer reads remain a measurable share after Phase 2) is discussed under [Phase 3 gate](#phase-3-gate). |

## Changes

### Phase 0: pool counters

`RDGMetricsSnapshot` carries the graph's cumulative `poolHits`, `poolMisses` and `poolEvictions`. They appear in the `[RDG metrics]` log line as `pool_total(...)` and in each graph record of the demo profile as `pool_hits`, `pool_misses` and `pool_evictions`. Subtract the first measured record from the last for an interval. [EngineProfiling.md](EngineProfiling.md) documents this.

### Phase 1: keep the steady working set

- `RDGPoolConfig::steadyBuilds` (default 0). During a trim, an available allocation last used by one of the last `steadyBuilds` builds, counting the current build, is kept regardless of the byte budget. It also does not consume the budget, so older idle allocations still get the full budget. `TrimPool(true)` (resize, teardown) releases everything as before.
- Deviation from the plan's wording: steady allocations are also exempt from the idle-age rule and from memory-pressure trims (`TrimIdlePoolEntries`). Evicting a set that the next frame slot reuses only raises peak memory: the retired copy still awaits its serials while a replacement is created. The idle-age exemption matters only if `maxIdleBuilds < steadyBuilds`. Older allocations follow the existing budget, idle-age and pressure rules.
- `RenderDevice::Init` configures the frame graph with the default budget and idle age and `steadyBuilds = numFrames` (3 in the demo). Standalone graphs keep the Phase 5 defaults, so the existing budget tests pass unchanged.
- `RDGResourceManager::GetPoolConfig()` exposes the configuration. The `RDGPoolConfig` comment documents that the budget bounds idle allocations only, so the frame graph can report available bytes above it.

Retention changes which allocations are kept, never when they may be reused: `AcquirePoolEntry` still skips entries whose queue serials are in flight.

### Phase 2: screen-sized G-buffer with exact reads

- `DeferredLightingRenderer::BuildGBufferGraph` sizes all six G-buffer targets and the render area to the viewport's width and height. It declares nothing while the viewport is zero-sized (suspended) or the scene uses forward materials. `GetGBufferExtent()` reports the last declared extent, or zero.
- `deferred_lighting.glsl` reads every G-buffer target with `texelFetch(map, ivec2(gl_FragCoord.xy), 0)`. The unused `inUV` input was removed; `deferred.vert` is unchanged and still feeds the skybox. The capture variants share the include. Environment maps keep their samplers.
- The G-buffer bindings keep `m_pColorSampler` and `m_pDepthSampler`: the shader still declares `sampler2D`, which needs a sampler in its descriptor, and both samplers serve other bindings. `texelFetch` ignores filtering and addressing.
- `RenderConfig::offScreenFbSize` and the demo's `--gbuffer-size` were removed, as the plan specified; the demo now rejects the option as unknown. `tools/benchmark_voxel_gi.py` and `tools/profile_dynamic_voxel_gi.py` lost their `--gbuffer` option and vary load with `--width`/`--height`. Docs that described the option say it was removed: [EngineProfiling.md](EngineProfiling.md), [VoxelGIPerformance.md](VoxelGIPerformance.md) and [DynamicVoxelGIM8Profiling.md](DynamicVoxelGIM8Profiling.md).
- The profile's `settings.gbuffer_size` and the lighting capture's `gbuffer_extent` changed from a scalar to the actual `[width, height]`; `[0,0]` means the frame had no G-buffer. No repository tool reads either field.
- No render-scale option was added (plan step 4).

Changed files: `RDGResourceManager.h/.cpp`, `RDGMetrics.h/.cpp`, `RenderDevice.cpp`, `RenderConfig.h`, `DeferredLightingRenderer.h/.cpp`, `deferred_lighting.glsl`, `SceneRendererDemo.cpp`, `SceneRendererDemoProfiling.cpp`, `SceneRendererDemoLightingCapture.cpp`, `RenderCoreTests.cpp`, the two profiling tools and the four docs named above.

## Tests

New tests in RenderCoreTest:

| Test | Covers |
| --- | --- |
| `SteadyPoolKeepsEveryFrameSlotWorkingSetAboveTheBudget` | Two and three simulated frame slots, two same-sized targets per frame (like the RGBA8 G-buffer pair), and a budget of 1.5 slots. Without retention, misses and evictions continue every frame. With `steadyBuilds` equal to the slot count, misses equal one set per slot, every later frame hits, and nothing is evicted, including under a memory-pressure trim before every build. Allocated bytes equal one set per slot and exceed the budget. |
| `SteadyPoolTrimsOlderAllocationsByBudgetAndIdleAge` | A target used only by build 0 stays pooled above the budget inside the window. It is evicted by the budget once it leaves the window, or by the idle age when the budget can hold it. |
| `FullTrimAndResizeReleaseTheFrameGraphsSteadyWorkingSet` | The device's frame graph has `steadyBuilds` equal to the frame count and default budget and age. With a zero budget, the steady set stays pooled; `TrimPool(true)` and `InvalidateRDGPassCompilerForResize` release it. |
| `GBufferFollowsTheViewportAndIsNotDeclaredWhileSuspended` | 8×8, odd 13×7, suspended 0×0, then restored 13×7: the G-buffer pass's six targets and render area equal the viewport, and a suspended viewport declares no pass. |

Updated: `RenderersRebuildCurrentBindingsTargetsAndSnapshotDrawData` now checks every G-buffer target and the render area against the viewport, including after its resize. `LightingCaptureRejectsInvalidTargetsAndRetriesWithoutCopyingStaleData` and the shared `AllocateRendererInputs` helper no longer set the removed field. The existing Phase 5 pool tests are unchanged and pass.

## Environment and method

Windows 10 x64, VS 2022 Professional (MSVC 19.44), Ninja, AMD Radeon RX 7900 XT, three frames in flight. Every GPU process ran from a byte-identical copy named `7zFM.exe` so the RTSS overlay does not hook it, with `VK_LOADER_LAYERS_DISABLE=~implicit~`. Profiles used Sponza, voxel resolution 256, 60 warm-up and 600 measured frames, and `--fixed-step --vsync=0 --disable-validation --async-compute=0 --gpu-memory-stats`; only `phase=measured` frames count. Each cell is the median of two runs in A-B-B-A order within one session. `Data/engine.cfg` was never changed (SHA-256 `9704d652…` before and after every stage). The in-hall captures appended a camera override for the run and restored the file byte for byte.

Phase 0 and Phase 1 ran from one executable, with a temporary environment switch that disabled retention; the switch was removed before the final build. The SPIR-V directory is compiled into the executable, so the runner installs each variant's four deferred fragment shaders before every run. Requesting 2560 × 1440 gives a 2560 × 1421 viewport on this desktop.

Raw logs, CSVs, JSON summaries and images are under `build/rendercore-improvement/`. `run.py` reproduces the profiles, captures and stress runs; `final.py` reproduces the final-tree correctness runs.

## Phase 0 → Phase 1

Captures for modes 1, 2 and 3 are byte-identical between Phase 0 and Phase 1 (`captures-p01.json`).

| Viewport / mode / RHI thread | CPU frame (ms) | GPU frame (ms) | OffScreen (µs) | SceneLighting (µs) | Descriptor misses | Pool misses / evictions | Peak device-local (MiB) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1280×720 / PBR / 0 | 1.045 → 0.883 | 0.418 → 0.399 | 350 → 335 | 131 → 124 | 600 → 0 | 1196 / 1196 → 0 / 6 | 1280 → 1024 |
| 1280×720 / PBR / 1 | 1.081 → 0.962 | 0.418 → 0.406 | 350 → 340 | 131 → 126 | 600 → 0 | 1188 / 1188 → 0 / 6 | 1280 → 1024 |
| 1280×720 / GI / 0 | 1.836 → 1.840 | 1.774 → 1.771 | 317 → 316 | 1510 → 1506 | 600 → 0 | 1198 / 1198 → 0 / 5 | 2107 → 1851 |
| 1280×720 / GI / 1 | 1.849 → 1.822 | 1.775 → 1.760 | 318 → 316 | 1508 → 1498 | 600 → 0 | 1197 / 1197 → 0 / 5 | 2107 → 1851 |
| 2560×1421 / PBR / 0 | 0.971 → 0.986 | 0.514 → 0.510 | 369 → 367 | 169 → 168 | 600 → 0 | 1196 / 1196 → 0 / 6 | 1536 → 1024 |
| 2560×1421 / PBR / 1 | 1.095 → 1.018 | 0.511 → 0.509 | 368 → 366 | 168 → 168 | 600 → 0 | 1192 / 1192 → 0 / 6 | 1408 → 1024 |
| 2560×1421 / GI / 0 | 5.430 → 5.474 | 5.387 → 5.410 | 355 → 354 | 4647 → 4677 | 600 → 0 | 1198 / 1198 → 0 / 6 | 2363 → 1851 |
| 2560×1421 / GI / 1 | 5.462 → 5.464 | 5.421 → 5.435 | 355 → 354 | 4661 → 4696 | 600 → 0 | 1198 / 1198 → 0 / 6 | 2363 → 1851 |

Stress case, `--gbuffer-size=4096` at 1280 × 720, RHI thread 0 (`stress-p01.json`):

| Mode | CPU frame (ms) | GPU frame (ms) | Descriptor misses | Pool misses / evictions | Peak device-local (MiB) |
| --- | --- | --- | --- | --- | --- |
| PBR | 7.284 → 1.349 | 1.255 → 1.266 | 600 → 0 | 2163 / 2164 → 0 / 2 | 3462 → 2436 |
| GI | 9.220 → 2.863 | 2.794 → 2.783 | 600 → 0 | 3386 / 3386 → 0 / 2 | 4545 → 3263 |

Notes:

- Phase 0 evicted and recreated two targets every frame at the default size and most of the 576 MiB G-buffer at 4096². Phase 1 recreates nothing.
- The few Phase 1 evictions are warm-up allocations ageing out under the 120-build idle rule (for example at frames 75 and 126 of a 660-frame run) and are never followed by a miss. During warm-up the GPU is slower and three slot sets are created; steady frames alternate between two.
- Peak memory fell because Phase 0 kept retiring copies alive while creating their replacements.
- GPU frame time is neutral; GI and 1440p differences are within run-to-run variation. CPU frame medians at 720p PBR were lower in every A/B pair, but this CPU-bound case varies about ±10% between repeats, so the size of the gain is uncertain. Exit criteria met: byte-identical captures, zero steady misses at 720p, 1440p and 4096², CPU no longer exceeding GPU at 4096², frame time neutral or better.

## Phase 1 → Phase 2

| Viewport / mode / RHI thread | CPU frame (ms) | GPU frame (ms) | OffScreen (µs) | SceneLighting (µs) | Descriptor misses | Pool misses / evictions | Peak device-local (MiB) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1280×720 / PBR / 0 | 0.882 → 0.958 | 0.400 → 0.254 | 336 → 195 | 124 → 78 | 0 → 0 | 0 / 6 → 0 / 6 | 1024 → 768 |
| 1280×720 / PBR / 1 | 0.949 → 0.943 | 0.401 → 0.256 | 337 → 198 | 125 → 78 | 0 → 0 | 0 / 4 → 0 / 6 | 1024 → 768 |
| 1280×720 / GI / 0 | 1.845 → 1.587 | 1.769 → 1.521 | 316 → 168 | 1505 → 1363 | 0 → 0 | 0 / 5 → 0 / 6 | 1851 → 1595 |
| 1280×720 / GI / 1 | 1.835 → 1.619 | 1.769 → 1.531 | 316 → 168 | 1506 → 1369 | 0 → 0 | 0 / 6 → 0 / 6 | 1851 → 1595 |
| 2560×1421 / PBR / 0 | 0.951 → 0.914 | 0.509 → 0.478 | 366 → 335 | 168 → 158 | 0 → 0 | 0 / 6 → 0 / 6 | 1024 → 1024 |
| 2560×1421 / PBR / 1 | 1.041 → 1.039 | 0.511 → 0.479 | 367 → 337 | 168 → 159 | 0 → 0 | 0 / 5 → 0 / 6 | 1024 → 1024 |
| 2560×1421 / GI / 0 | 5.449 → 5.884 | 5.405 → 5.837 | 354 → 327 | 4677 → 5144 | 0 → 0 | 0 / 6 → 0 / 6 | 1851 → 1851 |
| 2560×1421 / GI / 1 | 5.478 → 5.910 | 5.434 → 5.784 | 353 → 327 | 4687 → 5148 | 0 → 0 | 0 / 6 → 0 / 6 | 1851 → 1851 |

The transient payload per frame slot is 144 MiB before, and 31.6 MiB at 1280 × 720 and 124.9 MiB at 2560 × 1421 after. Peak device-local memory is allocated in blocks, so it shows the drop only at 720p. The 720p PBR CPU difference on RHI thread 0 is within that case's repeat spread (Phase 1 alone: 0.80–1.00 ms).

### 1440p GI trade-off

At 2560 × 1421 the old 2048² G-buffer covered the screen at 0.8× horizontally, so neighbouring pixels shared G-buffer texels, and bilinear filtering smoothed the remaining inputs. Cone tracing then saw more coherent surface positions and normals. Phase 2 evaluates GI for every pixel's own surface. A same-session check isolates the effect (`gi1440-check.json`, GI, RHI thread 0, two runs each):

| Configuration | Horizontal / vertical G-buffer sampling | SceneLighting (µs) | GPU frame (ms) |
| --- | --- | --- | --- |
| Phase 1, 2048² (old default) | 0.8× / 1.44×, bilinear | 4662, 4694 | 5.427, 5.442 |
| Phase 1, `--gbuffer-size=2560` | 1.0× / 1.8×, bilinear | 4922, 4950 | 5.856, 5.898 |
| Phase 2 | 1:1, `texelFetch` | 5110, 5118 | 5.746, 5.784 |

Removing the horizontal under-sampling accounts for about 60% of the SceneLighting increase; the rest comes from dropping the bilinear smoothing of positions and normals. Phase 2's whole frame is faster than the 2560² configuration, which also avoids under-sampling. The 1440p GI cost is therefore the cost of evaluating every pixel, which ray-query H1 requires, rather than overhead added by Phase 2. At 720p, where the old G-buffer over-sampled both axes, every pass got faster.

### Images

Mode 1 (voxel visualization) is byte-identical; it has no G-buffer. Modes 2 and 3 change by design (`captures-p12/`, plus in-hall views at 1280 × 720 and 2560 × 1421 in `hall-*/`). In the hall views 52–55% of mode-2 pixels and 35–39% of mode-3 pixels change; fewer than 0.75% change by more than 32 of 255. In side-by-side crops (`hall-*/crop-*.png`), Phase 1 shows light fringes along curtain, railing and column silhouettes, where bilinear reads blended foreground and background positions and normals (finding G3). Phase 2 edges are clean. High-frequency texture detail, such as the roof tiles in the default view, is sharper and slightly more aliased now that the vertical 2.8× over-sampling at 720p is gone. No new artifact was found in modes 2 or 3 at either size.

New capture hashes (default camera, 1280 × 720, `--frames=8 --fixed-step --vsync=0`), replacing the earlier baselines for modes 2 and 3:

| Mode | SHA-256 |
| --- | --- |
| 1 | `2f9890cdfddebd64dfb1da77d9f032236d219f5d2b5a9135e900090608f4cb4c` (unchanged) |
| 2 | `d7e2f24b4b7c7a674b99e36f572e372581e053bcf2824cd8d9c961f475884054` |
| 3 | `b05060597c79218d45bfbd4c5c78cf28a0a001ba07b31b673cee8ecf01bc27d4` |

The Phase 0 hashes for this configuration were `2f9890cd…`, `08b5aa62…` and `8d81caff…`. They differ from the R22 table in [RHIProductionVerification.md](RHIProductionVerification.md) because `Data/engine.cfg` has changed since.

### Phase 3 gate

After Phase 2, OffScreen takes 195 of 254 µs of the 720p PBR GPU frame (77%), 168 of 1521 µs in GI (11%), and 335 of 478 µs at 1440p PBR (70%). In PBR the G-buffer pass still dominates. Whether that comes from G-buffer bandwidth, which Phase 3 targets, or from geometry and material work was not measured, so the gate stays closed until a pass-level breakdown shows bandwidth-bound writes or reads.

## Correctness on the final tree

All runs used the final executables (temporary switch removed), with Vulkan validation and synchronization validation enabled (`VK_LAYER_VALIDATE_SYNC=1`) wherever validation is on. Logs are in `build/rendercore-improvement/final/`.

| Check | Debug | Release |
| --- | --- | --- |
| RenderCoreTest | 561 passed | 561 passed |
| VulkanRHITest | 48 passed | 48 passed |
| VulkanRHIIntegrationTest | 336 passed, 6 capability skips | 337 passed, 6 capability skips |
| CommonTest | 112 passed, 1 skip (directory symlinks) | 112 passed, 1 skip |
| ConeVoxelGIIntegrationTest, SceneModelSwitchTest, RuntimeUIIntegrationTest, UIDrawPacketTest | 4, 8, 10, 8 passed | 4, 8, 10, 8 passed |
| SmartPtrTest, FlatHashMapTest, LRUCacheTest, InputControllerTest, ConfigLoaderTest, ThreadPoolTest | All pass | All pass |
| Smoke: modes 1–3 × RHI thread 0/1 × async compute 0/1, with the smoke test's resize to 960 × 640, minimize/restore and shutdown | 12 of 12 pass | 12 of 12 pass |

- No log contains a `VUID-` or `SYNC-HAZARD` line.
- After the smoke test's resize and minimize/restore, the profiled final frame reports `gbuffer_size` `[960,640]`, equal to the viewport, in modes 2 and 3.
- `--gbuffer-size=1024` is rejected as an unknown option (exit 1).
- Final-tree captures equal the Phase 2 hashes above. A mode-3 lighting capture reports `gbuffer_extent` `[1280,720]` for a 1280 × 720 capture.
- `tools/smoke_gltf_rendering.py`: 18 of 18 assets pass.
- `tools/validate_voxel_gi.py`: all 162 GPU cases and image assertions pass.
- `tools/check_no_exceptions.py`, `git diff --check` and Python compilation of the two changed tools pass.
- Whole-file clang-format 19.1.5 `--dry-run --Werror` passes on the 12 changed C++ files.
