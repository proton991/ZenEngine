# RenderCore improvement verification

Status: Phases 0–2 implemented 2026-10-03 on top of `329d20be`; Phase 3 implemented 2026-10-07 on top of `8a72369a` and the pre-existing local GI changes. The original P0–P2 evidence below is historical; [the P3 section](#phase-3-implementation-and-verification-2026-10-07) records the current machine, checks, and remaining validation limits. Implements [RenderCoreImprovementPlan.md](RenderCoreImprovementPlan.md) and addresses item 5.5 of [RHIProductionTODO.md](RHIProductionTODO.md#55-per-frame-descriptor-miss).

## Result

In the original P0–P2 measurements, steady-state frames recorded zero descriptor-cache misses and zero render-graph pool misses at every measured size, including the 4096² stress case that Phase 2 removed. The deferred G-buffer matches the viewport and is read with `texelFetch`. The P3 rerun's exceptions to the zero-miss result are recorded below.

| Phase | Outcome |
| --- | --- |
| 0 Baseline | Recorded (the "Phase 0" columns below). The demo profile now reports pool hits, misses and evictions. |
| 1 Steady working set | Implemented. Captures byte-identical to Phase 0. Descriptor misses 600 → 0 and pool misses about 1,200 → 0 per 600 frames. At a 4096² G-buffer the CPU frame fell from 7.3/9.2 ms to 1.3/2.9 ms (PBR/GI) and now tracks GPU time. Peak device-local memory fell by 256–1,282 MiB. |
| 2 Screen-sized G-buffer | Implemented. Images change by design; silhouettes no longer blend foreground and background. At 720p the G-buffer shrinks from 144 to 31.6 MiB per slot and the GPU frame falls 37% (PBR) and 14% (GI). At 2560 × 1421, GI costs 6–8% more GPU time; see [the 1440p GI trade-off](#1440p-gi-trade-off). |
| 3 Compact encoding | Implemented 2026-10-07: 24 bytes/pixel, depth reconstruction, RG16 UNORM octahedral normals. Fresh A/B GPU-frame savings: about 6%/12% in 720p/1440p PBR and 1.5%/2.4% in GI. See the P3 evidence and limits below. |

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

At the original 2026-10-03 evaluation, OffScreen took 195 of 254 µs of the 720p PBR GPU frame (77%), 168 of 1521 µs in GI (11%), and 335 of 478 µs at 1440p PBR (70%). Its bandwidth versus geometry/material cost was not isolated, so implementation was deferred. The explicit 2026-10-07 P3 request was followed by a fresh baseline satisfying the plan's measurable-pass-share gate and a controlled compact-layout A/B experiment, recorded below. No hardware-counter claim of bandwidth saturation is made.

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

## Phase 3 implementation and verification, 2026-10-07

### Implementation

P0–P2 were already present when this work started. Their pool retention, exact texel reads, viewport extents, suspension handling and removed `--gbuffer-size` option were inspected; the existing pool and resize regression tests were rerun. Historical P0/P1 binaries and the removed square-G-buffer stress option were not recreated.

The G-buffer now contains RG16 UNORM octahedral normal, RGBA8 albedo, RGBA8 metallic/roughness, RGBA16F emissive/occlusion and depth. D32 gives **24 bytes/pixel**, down from 36: **21.09 MiB at 1280×720** and **84.38 MiB at 2560×1440** per slot. The position attachment and its sampled descriptor are removed. All four deferred/capture fragment variants share the same decode path.

`BuildGBufferUniformData` inverts the actual rasterization matrix in double precision on the CPU. It subtracts the world-space near-plane center before converting the inverse to floats, and carries that origin separately. Fragment reconstruction uses Vulkan's zero-to-one device depth and the existing camera Y flip. GI geometric derivatives operate on the relative position, then the origin is added for lighting and cone tracing. A plain float inverse/world-position derivative prototype failed the new near-camera precision test; this version passes without relaxing its tolerances.

`Common/gbuffer.glsl` also supplies normal encoding/decoding for the dedicated normal debug shader and voxel material-calibration capture. The debug view uses depth to keep background black. Calibration retains its last-fragment policy with always-pass depth writes and reconstructs the same diagnostic positions; its record format is unchanged. G-buffer draw recording is shared between production and calibration. Forward materials and the pool/queue retirement policy are unchanged.

### Environment and reproduction

Windows x64, MSVC 19.51, Ninja, NVIDIA GeForce RTX 5080, driver 617.14, Vulkan SDK 1.4.357.0. Both full Debug and Release builds completed. The first Debug build encountered a stale simdjson PCH charset mismatch; regenerating that build artifact resolved it. No source workaround was needed.

Evidence and runners are in `build/rendercore-p3/`:

- `p2/` preserves the starting executable and SPIR-V; `p3/` preserves the final ones. GPU runs use byte-identical `7zFM.exe` copies with `VK_LOADER_LAYERS_DISABLE=~implicit~`; correctness runs also set `VK_LAYER_VALIDATE_SYNC=1`.
- `run.py`, `ab.py`, `performance.json`: initial gate measurements, then interleaved P2–P3–P3–P2 profiles at both resolutions, modes 2/3 and RHI thread 0/1. Settings: `--no-ui --fixed-step --vsync=0 --async-compute=0 --disable-validation --gpu-memory-stats --warmup=60 --frames=660`. This demo executes 60 warm-up frames plus **660 measured frames**. The existing config selects Sponza, compute voxelization, voxel resolution 64 and three frame slots; its configured camera sees an outer wall. These timings describe that view, not the authored hall view below.
- `hall.py`, `compare.py`, `hall-comparison.json`: additional authored hall-camera comparisons at both sizes in modes 1–3. `diagnostic.py` temporarily compiles capture-only position/geometric-normal diagnostics, restoring the saved production shaders afterward.
- `test.py`, `tests-debug/`, `tests-release/`, `acceptance.py`, `smoke-debug/`, `smoke-release/`: executable suites and 24 smoke runs.
- `voxel-gi-images/`, `gltf-images/`, `cone-images/`, `calibration/`, `calibration-p2/`: image-tool and material-calibration evidence.

The configuration was restored byte-for-byte after each override: SHA-256 `1c40ff0126df9ad97ca42c60dbb5128b403f1496bf9ae31b1f88d5b97b6ee450`. Pre-existing source and documentation edits were preserved.

### Performance and retention

The initial P2 baseline put OffScreen at 122/178 µs of the 720p PBR GPU frame and 349/522 µs at 1440p (69%/67%), meeting the plan's measurable-cost gate. The table gives the median of two run medians in the interleaved comparison; each row is P2 → P3. Peak memory is VMA commitment, not residency.

| Width / mode / RHI thread | CPU frame ms | GPU frame ms | OffScreen µs | SceneLighting µs | Peak device-local MiB |
| --- | --- | --- | --- | --- | --- |
| 1280 / PBR / 0 | 0.640 → 0.645 | 0.178 → 0.167 | 121.4 → 109.0 | 30.8 → 32.8 | 622.8 → 589.0 |
| 1280 / PBR / 1 | 0.655 → 0.644 | 0.178 → 0.167 | 121.5 → 108.9 | 30.8 → 32.8 | 622.8 → 589.0 |
| 1280 / GI / 0 | 0.745 → 0.734 | 0.737 → 0.726 | 123.1 → 110.6 | 587.6 → 589.5 | 770.8 → 737.0 |
| 1280 / GI / 1 | 0.752 → 0.761 | 0.736 → 0.726 | 123.1 → 110.6 | 587.6 → 589.4 | 770.8 → 737.0 |
| 2560 / PBR / 0 | 0.649 → 0.648 | 0.521 → 0.457 | 347.6 → 287.0 | 120.6 → 117.2 | 949.0 → 814.0 |
| 2560 / PBR / 1 | 0.670 → 0.644 | 0.521 → 0.457 | 347.2 → 287.3 | 120.5 → 117.3 | 949.0 → 814.0 |
| 2560 / GI / 0 | 2.649 → 2.598 | 2.618 → 2.557 | 352.7 → 292.8 | 2208.9 → 2208.6 | 1097.0 → 962.0 |
| 2560 / GI / 1 | 2.665 → 2.602 | 2.619 → 2.556 | 352.6 → 292.6 | 2209.6 → 2208.6 | 1097.0 → 962.0 |

OffScreen improves about 10% at 720p and 17% at 1440p. Reconstruction adds about 2 µs to 720p lighting; the whole GPU frame still improves. CPU differences in the CPU-bound cases are small and mixed.

The original per-frame recreation is absent. Most runs have zero measured pool misses, but the literal zero-miss exit criterion is **not universal on this machine**. Threaded 1440p PBR intermittently needs a third set after it has been unused for over 120 builds: P2 records 6/12 misses in its two runs; P3 records 5/15 (one to three sets). Graph records show idle-age eviction followed later by renewed demand, not budget eviction of each frame's active set. Available idle memory is below budget. A few other P3 runs have 1–6 descriptor misses with zero texture-pool misses; the counts and each raw run are retained in `performance.json`. The policy and its mandatory idle-age tests remain unchanged; this work does not claim every descriptor stays resident indefinitely.

### Precision and images

The new GPU regression runs all four execution/queue combinations. It tests 64 normal directions including all six axes, RG16 UNORM quantization, translated/rolled finite perspective, infinite perspective and orthographic cameras, and distances 0.01, 1 and 10. Normal-vector error stays below 0.0001 (about 0.006°), reconstructed position stays within the declared D32 distance-dependent bound, and planar geometric normals have dot product above 0.9999 with their analytic direction. The RenderCore regressions verify the 24-byte layout, resize/suspension, and that the inverse/origin uniforms retain the recorded camera snapshot.

The hall capture-only probe compares 921,600 covered pixels. World-position difference from the former half-float target is 0.000100 median, 0.000160 p95 and 0.000269 maximum in renderer units. Geometric-normal differences are larger (2.13° median, 21.25° p95, 38.74° p99, 179.73° maximum): derivatives of the old quantized position texture are not a precision reference. The analytic GPU test above checks reconstruction independently. This is a real input change, not a byte-identical encoding substitution; strict geometric-normal equivalence with P2 is not achieved.

Hall image comparisons and visual review found no broad silhouette or GI-edge regression. PBR mean absolute channel differences are 0.020/0.021 of 255 at 720p/1440p. GI differences are 0.133/0.099; 0.023%/0.010% of pixels differ by more than 32, concentrated at individual surface/shadow boundaries. GI scene-linear combined relative RMS is 2.11%/4.53%, diffuse relative RMS 2.69%/2.72%, and mean combined bias −0.106%/−0.152%. Images are intentionally not claimed byte-identical. Mode 1, which does not consume the G-buffer, differs at only a handful of pixels between independent runs; the separate entrance-view repeats were byte-identical within each phase. Raw captures, comparison crops and all component statistics remain in the evidence directory.

### Current checks and remaining limits

| Check | Debug | Release |
| --- | --- | --- |
| RenderCoreTest | 568 pass | 568 pass |
| VulkanRHITest | 48 pass | 48 pass |
| VulkanRHIIntegrationTest | 342 pass, 1 native-window failure | 343 pass, 1 native-window failure |
| CommonTest | 112 pass | 112 pass |
| ConeVoxelGIIntegrationTest | 12 pass, including 4 compact-layout precision cases | 12 pass |
| SceneModelSwitch / RuntimeUIIntegration / UIDrawPacket | 8 / 10 / 8 pass | 8 / 10 / 8 pass |
| UIRendering / UIPlatform / WindowPlatform | 5 / 2 / 4 pass | 5 / 2 / 4 pass |
| EditorInput / EditorWindowChrome / EditorModel / EditorRendering / EditorPreview | 22 / 2 / 41 / 39 / 16 pass | 22 / 2 / 41 / 39 / 16 pass |
| SmartPtr / FlatHashMap / LRUCache / InputController / ConfigLoader / ThreadPool | All pass | All pass |
| Modes 1–3 × thread 0/1 × async 0/1 smoke | 12/12 pass | 12/12 pass |

The smoke sequence cycles render modes and finishes with a 960×640 G-buffer after resize/minimize/restore. No validation or synchronization hazard was found in the smoke/image runs. Release image tools: `validate_voxel_gi.py` **162/162**, `smoke_gltf_rendering.py` **18/18**, and `validate_cone_lighting.py` **10/10** with diffuse additivity error below 0.041%.

Two additional validation limits remain:

- `RHIWindowThreadingTest.SentResizesDeferAndCoalesceCallbacksUntilWindowUpdate` expects width 140 but this Windows environment produces width 176, in both configurations. It exercises unchanged native-window/RHI-thread code and has no renderer dependency. No unrelated window fix was included.
- The optional material-atlas calibration reports four emission differences above 0.016 per voxelizer (0.017578–0.019531). Repeating it with the saved P2 executable and shaders produces the **same cells, values and failures**. Both phases match 1,637 surface samples, with zero color/normal errors in the failing cells and no missing/extra voxels or owner mismatches. The compact calibration path adds no failure; the existing emission threshold discrepancy remains open.

Whole-file clang-format 19.1.5, the no-exceptions check, and `git diff --check` pass for the changed source. All 32 interleaved profiles pass `validate_engine_profile.py --require-gpu --require-frame-gpu`; eight affected SPIR-V modules pass `spirv-val --target-env vulkan1.2` and match the restored final snapshot. These results validate this Windows/NVIDIA configuration; they do not establish AMD or MoltenVK performance or precision for P3.
