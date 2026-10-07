# RenderCore improvement plan

Status: Phases 0–2 implemented 2026-10-03; Phase 3 implemented 2026-10-07 after fresh baseline and interleaved A/B measurements ([verification and remaining validation limits](RenderCoreImprovementVerification.md#phase-3-implementation-and-verification-2026-10-07)). Proposed 2026-10-02, based on `e79660ec` plus the uncommitted RHI section-5 changes described in [RHIDeferredWorkVerification.md](RHIDeferredWorkVerification.md). The findings below describe the code before Phase 1.

Size the deferred G-buffer to the screen, and stop the render graph's transient pool from recreating render targets that every frame uses. Together these remove per-frame texture creation and descriptor misses at every resolution, cut G-buffer memory, and give screen-space algorithms one G-buffer texel per screen pixel. The ray-query lighting work requires that.

Scope: `RenderCore/V2` (render-graph resource pool, `RenderDevice`, `DeferredLightingRenderer`), the deferred-lighting shaders, the scene renderer demo and its profiling tools, and their tests. RHI contracts are unchanged.

## 1. Relationship to other plans

| Plan | Relationship |
| --- | --- |
| [RDGRenderCoreAnalysisAndPlan.md](RDGRenderCoreAnalysisAndPlan.md) | Phase 5 introduced the pool budget (256 MiB of idle payload, 120 idle builds). This plan keeps that policy for every graph by default and adds a retention window only for the frame graph. Phase 4 (range-aware barriers) and the deferred Phase 6 items stay deferred; whole-allocation synchronization is unchanged. |
| [RHIDeferredWorkVerification.md](RHIDeferredWorkVerification.md) | Item 5.5 traced the per-frame descriptor miss to this pool churn; its measurements are the evidence below. |
| [HardwareRayQueryEnvironmentLightingPlan.md](HardwareRayQueryEnvironmentLightingPlan.md) (Hybrid Voxel GI plan) | P1 adds receiver identity and motion outputs; P3 adds temporal accumulation, reprojection and edge-aware filtering, all in screen space. Land Phase 2 of this plan before P1 so those inputs share a 1:1 screen-sized G-buffer. A half-resolution trace (P8) would use its own targets, not the G-buffer. |
| [EngineProfiling.md](EngineProfiling.md) | Documents `--gbuffer-size`, which Phase 2 removes. |

## 2. Findings

| ID | Problem | Evidence |
| --- | --- | --- |
| G1 | The G-buffer is a fixed 2048 × 2048 square (`RenderConfig::offScreenFbSize`), unrelated to the window. It has been a constant since the first deferred renderer (`8974ff89`, 2024-12-22), defined next to `shadowMapSize = 2048`; nothing records a reason for it. | [RenderConfig.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderConfig.h), [DeferredLightingRenderer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp) `BuildGBufferGraph` |
| G2 | The G-buffer pass rasterizes with the window's aspect ratio into the square, and the lighting pass reads it back with full-screen UVs. Sampling therefore differs per axis: at 1280 × 720 it is 1.6× horizontal and 2.8× vertical (4.6× the screen's pixels); at 2560 × 1440 it is 0.8× horizontal (under-sampled) and 1.4× vertical; at 3840 × 2160 it is 0.53× and 0.95×. | [deferred.vert](../Data/Shaders/SceneRenderer/deferred.vert), [deferred_lighting.glsl](../Data/Shaders/SceneRenderer/deferred_lighting.glsl) `SURFACE_SAMPLE` |
| G3 | The G-buffer is read through a linear, repeating sampler. When G-buffer and screen sizes differ, silhouette pixels can blend foreground and background positions and normals. Voxel GI uses exactly these values: cone origins (`DiffuseVoxelLighting(worldPos, N)`) and a geometric normal from `dFdx/dFdy(worldPos)` used for light visibility. This follows from the sampling setup; no artifact has been isolated to it. | `deferred_lighting.glsl`, `DeferredLightingRenderer::m_pColorSampler` (`CreateLinearRepeat`) |
| G4 | Cost. The G-buffer is about 36 bytes per pixel: position, normal and emissive/occlusion in RGBA16F, albedo and roughness in RGBA8, D32 depth. At 2048² that is 144 MiB per frame slot. At 1280 × 720 the G-buffer pass (`OffScreen`) takes 0.74 ms of a 1.14 ms PBR GPU frame (this session's baseline), shading 4.6× the screen's pixels. | `build/rhi-deferred/baseline/` |
| P1 | The frame graph's transient pool uses the default 256 MiB idle budget. A trim keeps the most recently used available allocations up to the budget, and allocations from frames still in flight count as available. One G-buffer set is 144 MiB and frames alternate between sets, so the working set (about 288 MiB) exceeds the budget. Each trim evicts the last-added `offscreen_albedo` and `offscreen_roughness`, and the next frame recreates them: two 16 MiB images, their allocations and views, every frame. | [RDGResourceManager.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp) `TrimPool`, `CreatePhysicalResource`; probe logs in `build/rhi-deferred/probe/` |
| P2 | Each recreated texture gets a new stable ID, so the deferred-lighting descriptor set misses the cache once per frame (600 misses per 600 frames). Raising the budget to 512 MiB removes every miss and recreation. Frame time is unchanged at the default size. | `build/rhi-deferred/probe-budget512/` |
| P3 | At `--gbuffer-size=4096` the whole G-buffer (about 576 MiB per slot) exceeds the budget and is recreated every frame: CPU frame 10.2 ms against 5.5 ms of GPU time. | `build/rhi-deferred/vsync-pacing/` probe |
| P4 | No fixed budget suits every resolution. Even screen-sized, one slot is 32 MiB at 720p, 127 MiB at 1440p and 285 MiB at 2160p; with three frames in flight that is 95, 380 and 854 MiB. | Arithmetic from G4 |

The frame graph never calls `SetPoolConfig`; only tests do. Pass viewport and scissor already come from the pass render area (`RenderGraph.cpp`), so resizing the G-buffer needs no change to draw recording.

## 3. Invariants to preserve

1. The [RHI README](../ZenCore/Include/Graphics/RHI/README.md) contracts and RDG Phase 5 retirement: a pooled allocation is never reused before every queue that used it has completed. Retention changes which allocations are kept, not when they may be reused.
2. Standalone graphs keep the exact Phase 5 pool semantics and pass the existing budget tests unchanged.
3. Whole-allocation synchronization and the deferred RDG Phase 4/6 scope stay as they are.
4. The forward-material path, which does not use the G-buffer, is unchanged.
5. Inline and threaded execution, and async compute on and off, behave the same.
6. Resize still resets the frame graph and trims its idle pool; a suspended (zero-size) viewport still renders nothing.
7. Repository C++ rules: one return at the end, explicit types, blank lines between statements, engine containers, whole-file clang-format 19.1.5.

## 4. Implementation phases

Each phase builds, passes its tests, and can be reviewed alone.

### Phase 0 — Baseline

1. Profile with the Phase 0 settings of [RHIImprovementPlan.md](RHIImprovementPlan.md) at 1280 × 720 and at the 2560 × 1440 desktop size, modes 2 and 3, RHI thread 0 and 1. Record CPU, GPU and per-pass medians, `descriptor_misses`, and peak memory from `--gpu-memory-stats`.
2. Record RDG pool hits, misses and evictions from `RDGResourceManager::GetPoolStats` in steady state. Add a profiling field if the demo does not yet report them.
3. Repeat with `--gbuffer-size=4096` as the churn stress case (P3) before Phase 2 removes the option.
4. Keep the three mode captures for comparison.

Run every GPU process unhooked (a `7zFM.exe` copy) and compare changes with interleaved same-session A/B runs: CPU timings on this machine drift by 10–15% over hours.

### Phase 1 — Keep the steady working set

1. Add `steadyBuilds` to `RDGPoolConfig` with a default of 0. During `TrimPool`, an available allocation whose `lastUsedBuild` falls within the last `steadyBuilds` builds is kept regardless of the byte budget. Older allocations follow the existing budget and idle-age rules. `TrimPool(true)` (resize, teardown) still releases everything available.
2. `RenderDevice` configures the frame graph after creating it: `{budgetBytes = 256 MiB, maxIdleBuilds = 120, steadyBuilds = number of frame slots}`. Standalone graphs keep the default and today's behavior.
3. Pool statistics may report available bytes above the budget for the frame graph; document that the budget bounds idle allocations only.
4. Tests (RenderCoreTest):
   - A frame graph whose per-frame working set across all frame slots exceeds the budget reaches zero misses after warm-up and evicts nothing while the working set is unchanged.
   - Allocations unused for more than `steadyBuilds` builds are trimmed to the budget and still respect the idle age.
   - `TrimPool(true)` and resize release retained allocations.
   - Existing Phase 5 budget tests pass unchanged.

Exit: images unchanged (captures byte-identical); zero steady-state pool misses and descriptor misses at 720p, 1440p and `--gbuffer-size=4096`; at 4096² the CPU frame no longer exceeds the GPU frame by the recreation cost; frame time neutral or better in an interleaved A/B.

### Phase 2 — Screen-sized G-buffer with exact reads

1. In `DeferredLightingRenderer::BuildGBufferGraph`, size every G-buffer output and the render area to the viewport's width and height, the same extent as the lighting pass. Do not declare the G-buffer while the viewport is suspended.
2. In `deferred_lighting.glsl`, read the G-buffer with `texelFetch(map, ivec2(gl_FragCoord.xy), 0)` instead of sampling with full-screen UVs. The capture variants (`deferred_capture.frag`, `voxel_gi_capture.frag`) include the same file. Environment maps keep their samplers. Remove the G-buffer bindings' dependency on `m_pColorSampler` and `m_pDepthSampler` if nothing else needs them.
3. Remove `RenderConfig::offScreenFbSize` and `--gbuffer-size`. Update `tools/benchmark_voxel_gi.py`, `tools/profile_dynamic_voxel_gi.py` and [EngineProfiling.md](EngineProfiling.md) to vary load with `--width`/`--height`. Report the actual G-buffer extent in the profile and lighting-capture JSON (`gbuffer_size`, `gbuffer_extent`).
4. Add a render-scale option only together with a pass that upscales the lit result. Until then the G-buffer stays 1:1 with the screen.

Exit: the G-buffer extent equals the viewport extent in every mode and after resize and minimize/restore; zero steady-state pool misses and descriptor misses; `validate_voxel_gi.py` and `smoke_gltf_rendering.py` pass; visual review of modes 1–3 at 720p and 1440p, comparing silhouettes and GI edges against Phase 0 captures; capture hashes rebaselined, since output images change by design; G-buffer memory and `OffScreen` pass time recorded against Phase 0.

### Phase 3 — Compact G-buffer encoding (optional, gated)

Start only if, after Phase 2, the G-buffer pass or the lighting pass's G-buffer reads remain a measurable share of GPU time at the target resolution.

1. Reconstruct world position from D32 depth and the inverse view-projection matrix, removing the RGBA16F position target (−8 bytes per pixel).
2. Store normals as two-channel octahedral RG16 (−4 bytes per pixel).
3. Check GI precision: cone origins and the derivative normal must match Phase 2 within the image-tool thresholds.

This takes the G-buffer from 36 to about 24 bytes per pixel. Reprojection inputs added by ray-query H1 must be included in the same layout review.

Implemented with RG16 UNORM octahedral normals and D32 reconstruction. The inverse matrix uses a nearby world origin so GI derivatives are calculated before adding translation. Normal debug views and the voxel material-calibration capture consume the same encoding. The current renderer has no receiver-identity or motion attachments; adding those remains part of the separate Hybrid Voxel GI plan.

## 5. Expected effects

| Effect | 1280 × 720 | 2560 × 1440 |
| --- | --- | --- |
| G-buffer memory per frame slot | 144 → 32 MiB | 144 → 127 MiB |
| G-buffer sampling | 4.6× over-sampled → 1:1 | 0.8× horizontally → 1:1 |
| Per-frame texture recreation and descriptor misses | Removed (Phase 1) | Removed (Phase 1) |

The `OffScreen` pass shades 4.6× fewer pixels at 720p; its time saving is not yet measured. Phase 1 alone removes the churn, including at today's 2048² default, without changing images.

## 6. Validation

| Scope | Validation |
| --- | --- |
| Each phase | Debug and Release builds; RenderCoreTest, VulkanRHITest, VulkanRHIIntegrationTest, CommonTest and the UI/scene suites; smoke matrix (modes 1–3 × RHI thread 0/1 × async compute 0/1, with resize, minimize/restore and shutdown) with validation and synchronization validation. |
| Phase 1 | Captures byte-identical to Phase 0; pool and descriptor counters; interleaved A/B frame time; peak memory. |
| Phase 2 | Image tools, visual review and rebaselined captures; counters at 720p and 1440p; resize and minimize/restore; interleaved A/B frame time; peak memory. |
| Phase 3 | GI precision comparison and image tools; G-buffer pass and lighting pass timing. |

Record environment, commands, counters, images and measurements in `Doc/RenderCoreImprovementVerification.md`.

## 7. Order and size

| Phase | Size | Depends on | Notes |
| --- | --- | --- | --- |
| 0 Baseline | S | — | Required before Phases 1 and 2. |
| 1 Steady working set | S | 0 | Behavior-neutral for images; fixes churn at every resolution. |
| 2 Screen-sized G-buffer | M | 1 | Changes images by design; land before ray-query H1. |
| 3 Compact encoding | M | 2 | Optional; only if its gate opens. |
