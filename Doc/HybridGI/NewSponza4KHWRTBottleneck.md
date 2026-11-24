# New Sponza at native 4K with hardware ray tracing

## Standalone performance after optimizing the full-density probe grid

Two 3,000-frame release ZenApp runs per view on the RTX 5080 measured **43.0–43.3 FPS stationary and 46.7–47.1 FPS panning** after the cache warmed up. All runs used New Sponza, Kloofendal HDR lighting, hardware tracing, the Optimal preset and native 3840×2160 throughout. The framebuffer, renderer and GI extents agreed. Both views had 162,000 probe slots at 8-pixel spacing and a world cache of 1,048,576 cells during measurement. [Frame intervals, settings and input fingerprints](../../build/screen-probe-4k-20261124-152419/optimization-summary.json)

| Standalone application measurement | Stationary | Fixed-eye pan |
| --- | ---: | ---: |
| Original, frames 241–721 | 38.2–38.3 FPS | 43.3–44.4 FPS |
| Optimized, frames 241–721 | 39.4–39.6 FPS | 42.6–44.1 FPS |
| Original, frames 721–2881 | 39.4 FPS | 45.6 FPS |
| Optimized, frames 721–2881 | 43.0–43.3 FPS | 46.7–47.1 FPS |
| Optimized time per frame over the longer window | 23.11–23.27 ms | 21.24–21.39 ms |
| Time to save for 60 FPS | 6.45–6.60 ms | 4.57–4.73 ms |

Over the same 2,160 frames, standalone throughput improved **9.1–9.8% stationary and 2.4–3.2% panning**. The pan covers 27 full cycles. [Original stationary run](../../build/screen-probe-4k-20261124-152419/stock-direct/rates.json) · [Original pan](../../build/screen-probe-4k-20261124-152419/stock-direct-pan/rates.json) · [Optimized stationary run](../../build/screen-probe-4k-20261124-152419/final-direct-static/rates.json) · [Optimized pan](../../build/screen-probe-4k-20261124-152419/final-direct-pan/rates.json) · [Rebuilt executable, stationary](../../build/screen-probe-4k-20261124-152419/restored-direct-static/rates.json) · [Rebuilt executable, pan](../../build/screen-probe-4k-20261124-152419/restored-direct-pan/rates.json)

The hardware reflection prefilter now reads resolved hit records directly. This removes a second copy of radiance, direction and sampling density, saving **248,832,000 bytes (237.3 MiB)** of transient buffer space at this resolution. The probe scheduler continues sampling settled anchors every frame. With a stationary view, stable scene lighting and geometry, and at least 256 frames of effective probe history, it can use one 16-ray batch. Camera movement restores the existing motion budget immediately. [Hit-record consumer](../../Data/Shaders/VoxelGI/screen_probe_prefilter.glsl) · [Probe scheduler](../../Data/Shaders/VoxelGI/screen_probe_proposal.comp)

The fast-motion test compares moving frames with independently settled images at the same poses. Both builds used the same stock reference images. The mean percentage of pixels differing by more than 16 levels in any 8-bit RGB channel was **0.0327% → 0.0318%** during turns, **0.0153% → 0.0155%** during forward motion and **0.0122% → 0.0120%** during strafes. A second launch of the original build measured 0.0326%, 0.0155% and 0.0121% on the same three motions. The harness disables async compute for these quality runs. The FPS benchmarks request async compute and the frame graph reports a single graphics queue group on this workload. [Motion results and post-stop recovery](../../build/screen-probe-4k-20261124-152419/final-motion/common-reference.json) · [Repeated stock motion test](../../build/screen-probe-4k-20261124-152419/stock-repeat-motion/common-reference.json)

A 16-pixel grid reached 63.8/66.6 FPS stationary/panning. It cut the probe count to 40,500 and the runtime granted a larger world cache. Even a 14-pixel grid increased errors on fast turns and strafes. A separate experiment that interpolated the gather across smooth surfaces slowed the stationary run to 30.5 FPS. The default retains the 8-pixel grid and the full-resolution gather. [Grid and gather trials](../../build/screen-probe-4k-20261124-152419/optimization-summary.json) · [14-pixel motion comparison](../../build/screen-probe-4k-20261124-152419/grid14-motion/common-reference.json)

Two fresh 80-frame Nsight GPU traces measured a **17.68 ms stationary median (18.26 ms p95)** and a **21.51 ms pan median (21.93 ms p95)**. All 160 captured frames exceeded the 16.67 ms budget. The full-resolution gather cost 4.20/3.83 ms stationary/panning. The combined probe hit sampling, hardware traversal, shading and accumulation passes cost 2.32/5.38 ms. Movement still exposes enough new surfaces to make that group the larger panning cost. [Stationary GPU trace summary](../../build/screen-probe-4k-20261124-152419/final-nsight-static/trace/summary.json) · [Pan GPU trace summary](../../build/screen-probe-4k-20261124-152419/final-nsight-pan/trace/summary.json)

All 660 RenderCore tests and 16 targeted Vulkan integration tests passed. A separate validated native-4K run completed 359 frames across turns, forward motion and strafes. The standalone glossy-only test disables environment lighting and compares each method with a sparse Monte Carlo reference. On this camera, both builds missed the retained renderer’s 0.680 RMS target on the 8-bit scale. The original probe renderer scored 1.092 and the optimized renderer scored 1.097 against the same frozen reference. [Vulkan integration tests](../../build/screen-probe-4k-20261124-152419/final-ray-tests.log) · [Validated motion run](../../build/screen-probe-4k-20261124-152419/final-validation.log) · [Original glossy result](../../build/screen-probe-4k-20261124-152419/stock-specular/report.json) · [Optimized glossy result](../../build/screen-probe-4k-20261124-152419/final-specular/report.json)

## Why the standalone stationary result was lower

The stock renderer measured **38.2 FPS standalone and 45.9 FPS launched through Nsight** over the same stationary frame-graph executions, 241 through 721. Both runs used the same ZenApp binary, camera, scene package, environment, 774 shaders, GI settings, native 4K extents and immediate presentation mode. The mode-dependent difference was 7.7 FPS. The GPU reported 100% utilization in both windows. Its median graphics clock was 3,015 MHz standalone and 2,970 MHz under Nsight. The exact driver or profiler behavior responsible for that difference remains unidentified. [Matched launches and clock samples](../../build/screen-probe-4k-20261124-152419/stationary-comparison.json)

Comparing the two Nsight launches, the historical 46.6 FPS became 45.9 FPS. Median GPU frame time moved from 21.03 to 21.43 ms and p95 from 22.00 to 22.22 ms. Both traces reported zero demoted VRAM. The older executable had a different hash. Its scene package, setup and shader hashes matched. Use standalone runs to qualify application FPS and matched profiler launches to compare GPU passes. [Earlier trace summary](../../build/hwrt-4k-default-20261124-132935/static/summary.json) · [Stock rerun](../../build/screen-probe-4k-20261124-152419/stock-nsight/trace/summary.json)

## Default preset timings before optimization

On 2026-11-24, ZenApp launched under Nsight recorded **46.6 FPS stationary and 43.9 FPS panning** with the default `Optimal` GI preset, an RTX 5080 and the Kloofendal HDR environment enabled. The steady-state bottleneck was GPU work in the screen-probe GI algorithm. The full-resolution gather was the largest individual pass. Sampling, tracing, shading, and accumulating probe hits together cost more than the gather during the pan. [Measurement summary](../../build/hwrt-4k-default-20261124-132935/bottleneck.json)

The earlier **22.7 FPS** stationary result used a saved per-pixel ReSTIR configuration with a 1920×1080 GI target. The default preset on this GPU gathers screen probes at 3840×2160. In the earlier ReSTIR setup, the pre-enhancement and current RHI builds both reproduced the HDR slowdown. On the current build, enabling the HDR environment added 17.15 ms to the median GPU frame, including 16.03 ms in `HybridTrace`. [Preset implementation](../../ZenCore/Source/Graphics/RenderCore/V2/VoxelGISettings.cpp) · [Original run](../../build/new-sponza-env-4k-20261124-123823/results.json) · [RHI and environment comparison](../../build/sponza-rhi-attribution-20261124-125919/comparison.json)

| Measurement | Stationary | Panning |
| --- | ---: | ---: |
| Application FPS | 46.6 | 43.9 |
| GPU frame, median | 21.03 ms | 22.12 ms |
| GPU frame, p95 | 22.00 ms | 22.98 ms |
| Full-resolution probe gather | 5.03 ms | 3.86 ms |
| Probe hit sampling, traversal, shading, and accumulation | 4.23 ms | 5.60 ms |
| ↳ Hardware traversal pass within that group (`ScreenProbeTraceHits`) | 0.85 ms | 1.12 ms |
| Direct-light visibility | 2.46 ms | 2.28 ms |
| Rough-reflection filtering | 2.02 ms | 2.24 ms |

Stage timings are medians over 80 Nsight GPU Trace frames per view. Group timings use per-frame sums of their markers. GPU ranges can overlap. ZenApp measured application FPS separately over 480 frame-graph executions, covering six cycles of the fixed-eye ±20° pan at 1° per frame. Both captures used the same release executable, scene package, shaders, fullscreen 3840×2160 framebuffer and render target, threaded Vulkan RHI, GPU indirect culling, driver 621.35, and unaltered GPU clocks. [Stationary trace summary](../../build/hwrt-4k-default-20261124-132935/static/summary.json) · [Pan trace summary](../../build/hwrt-4k-default-20261124-132935/pan/summary.json) · [Setup and input hashes](../../build/hwrt-4k-default-20261124-132935/workload.json)

Separate instrumented runs measured 240 steady-state frames per view. The RHI worker spent a median **0.68 ms stationary / 0.62 ms panning** translating commands and **0.034 / 0.057 ms** submitting them. Pipeline and resource creation each measured **0 ms** at both median and p95. The worker spent **12.95 / 14.88 ms** waiting. These timings put the optimization priority on the GPU passes. [Stationary worker counters](../../build/hwrt-4k-default-20261124-132935/worker-static/summary.json) · [Pan worker counters](../../build/hwrt-4k-default-20261124-132935/worker-pan/summary.json)

## GPU budget in the original Nsight captures

The budget is **16.67 ms per frame**. Reaching it requires saving **4.37 ms stationary and 5.45 ms panning** at the median, or **5.34 ms stationary and 6.32 ms panning** at p95. All 160 captured GPU frames exceeded the budget. Native 4K at 60 FPS remains an optimization target whose feasibility at the current image quality needs an implementation and measurements.

1. **Reduce gather work on smooth surfaces.** The gather evaluates roughly 16–20 probes per pixel. Earlier profiling found instruction issue limited this pass. Test a simpler reconstruction where depth, normals, and lighting agree, retaining the full gather at discontinuities and thin geometry. Compare fine detail and rough reflections against the current preset. [Gather implementation](../../Data/Shaders/VoxelGI/screen_probe_gather.glsl) · [Earlier gather experiments](../NativeGIOptimization.md#o2-gather-done-2026-11-20-94--48-ms-15-ms-target-not-met)
2. **Test a frame-wide probe ray budget during motion.** The scheduler gives extra samples to young or relocated probes. Test redistributing a fixed budget toward recently exposed surfaces. Measure p95 GPU time during the same pan and compare noise on newly visible surfaces, glossy reflections, and convergence after the camera stops. [Probe scheduling implementation](../../Data/Shaders/VoxelGI/screen_probe_proposal.comp)
