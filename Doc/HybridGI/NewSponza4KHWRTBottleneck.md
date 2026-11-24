# New Sponza at native 4K with hardware ray tracing

## Indirect lighting from secondary metal hits

These results cover hardware screen probes with world-cache feedback. Per-pixel hardware GI and receiver lighting use diffuse-only secondary-hit shading. Compute field traversal uses its own material evaluation.

Screen probes and the hardware world cache used only diffuse reflectance when shading a secondary hit. A lit metal surface therefore returned zero reflected light. A Vulkan regression reproduces this with lit rough-metal cards, a diffuse backdrop and a black environment. It reads the bounced irradiance on the backdrop. The original shader returned exactly zero. [Regression test](../../tests/integration/CommonTest/GICacheTests.inl) · [Before](../../build/dark-indirect-20261124-195003/checkpoint/metallic-red.log) · [After](../../build/dark-indirect-20261124-195003/final/metallic-green.log)

The two shading passes now use the hit material’s normal-incidence reflectance and roughness to integrate a GGX reflection using the existing BRDF lookup table. The cache stores irradiance divided by π, which supplies a local mean incident radiance for this approximation. It adds no traversal and cannot resolve a sharp directional reflection at the secondary hit. The deferred lighting pass also integrates diffuse Fresnel over the incident hemisphere and applies the glossy lookup table’s Fresnel coefficients once. [Secondary-hit shading](../../Data/Shaders/VoxelGI/ray_hit_bounce.glsl) · [Deferred lighting](../../Data/Shaders/SceneRenderer/deferred_lighting.glsl)

The diffuse Fresnel weight averages `(1 − V·H)⁵` over cosine-weighted light directions, with `H = normalize(V + L)`. The shader uses a cubic in `1 − N·V`. An endpoint-constrained fit over 201 view angles, using 192-point quadrature in elevation and 512 uniform azimuth samples, gave a maximum absolute weight error of 0.00011. The coefficients depend only on the material model.

The center comparison uses 518,227 matched 4×4 receiver regions at native 4K, after 3,072 ZenApp frames. The reference uses 256 paths per pixel and up to ten path vertices. Both images use vertex normals. Bias measures mean linear luminance relative to the reference. RGB RMS measures spatial error relative to the reference signal. [Checkpoint measurements](../../build/dark-indirect-20261124-195003/checkpoint/truth.json) · [Revised measurements](../../build/dark-indirect-20261124-195003/final/truth.json)

| Center view | Mean bias before | Mean bias after | RGB RMS before | RGB RMS after |
| --- | ---: | ---: | ---: | ---: |
| Bounced irradiance | −29.5% | −8.5% | 49.5% | 43.7% |
| Diffuse indirect lighting | −27.5% | −5.7% | 44.0% | 37.4% |
| Specular indirect lighting | −0.03% | −21.5% | 70.1% | 46.6% |
| Final lighting | −7.9% | −2.6% | 4.35% | 3.81% |

The center-view diagnostic preview brightens each row before tone mapping. It multiplies final lighting by 16.67 (+4.06 stops) and isolated diffuse indirect lighting by 100.66 (+6.65 stops), using the same exposure for all three columns within a row. The table uses the original linear radiance. With normal maps enabled and the renderer’s ordinary display output, mean 8-bit luma rises from 14.39 to 15.60 after the fix. [Before and after at display exposure](../../build/dark-indirect-20261124-195003/sponza-display-before-after.png) · [Full-resolution ZenApp frame](../../build/dark-indirect-20261124-195003/final/sponza-display-native-4k.png)

A second view, rotated 20° from center, used a fresh 32-path-per-pixel reference and 129,506 matched 8×8 regions. Diffuse indirect lighting went from **27.8% to 6.0% below reference**. Final lighting went from 7.3% to 2.4% below. Glossy reflections remain a separate limitation in both views. Their spatial RMS decreased on the rotated view from 83.7% to 50.5%, while mean specular luminance moved from 4.0% above reference to 19.8% below. [Second-view reference](../../build/dark-indirect-20261124-195003/angle-reference.json) · [Before](../../build/dark-indirect-20261124-195003/checkpoint/angle/report.json) · [After](../../build/dark-indirect-20261124-195003/final/angle/report.json) · [Center-view image comparison](../../build/dark-indirect-20261124-195003/lighting-before-after.png)

Motion and noise tests used the normal-mapped Optimal preset. Stationary frame-to-frame RGB RMS over 32 frames changed from **0.0368 to 0.0377** on the 0–255 display scale. Against separately settled images at the same camera poses, RMS rose from **0.812 to 0.826** during the benchmark pan, 1.086 to 1.107 during fast turns, 0.694 to 0.765 moving forward, and 0.627 to 0.667 strafing. Isolated bright errors on the pan fell from **0.00337% to 0.00258% of pixels**, and also declined in all three fast movements. [Baseline stability measurements](../../build/gi-quality-20261124-191254/summary.json) · [Stationary](../../build/dark-indirect-20261124-195003/final/static-noise.json) · [Pan](../../build/dark-indirect-20261124-195003/final/pan-noise.json) · [Fast motion and recovery](../../build/dark-indirect-20261124-195003/final/moving-noise.json)

Three standalone ZenApp launches per view on the RTX 5080 measured a median **54.4 FPS stationary** (54.2–56.2) and **46.1 FPS panning** (44.1–46.6). The checkpoint measured 55.7 and 47.7 FPS over the same steady-state frame intervals. The secondary-reflection prototype that traced another ray for one in eight probe hits measured 43.3 FPS panning and left a 20.4% bounced-irradiance deficit. [Per-run intervals, settings and provenance](../../build/dark-indirect-20261124-195003/summary.json)

All 660 RenderCore tests and 36 targeted Vulkan integration tests passed, including the rough-metal regression in all four submission modes. The 359-frame motion smoke run also passed with Vulkan synchronization validation enabled. [RenderCore](../../build/dark-indirect-20261124-195003/final/render-tests.log) · [Ray tracing and GI](../../build/dark-indirect-20261124-195003/final/ray-tests.log) · [Material and GI integration](../../build/dark-indirect-20261124-195003/final/material-tests.log) · [Validation](../../build/dark-indirect-20261124-195003/final/validation.log)

## Moving repeated gather work into the probe filter

On the RTX 5080, repeated standalone ZenApp launches with the revised gather measured **55.6–56.0 FPS stationary and 47.7–47.9 FPS panning**. The local checkpoint `d3a6f7b5` measured **43.1 and 47.1 FPS** on the same New Sponza setup. Both used the Optimal preset, HDR environment lighting, hardware tracing, an 8-pixel probe grid, 1,048,576 steady-state world-cache cells, and native 3840×2160 GI and presentation. After restoring and rebuilding the release executable, the final runs measured 55.8 FPS stationary and 47.9 FPS panning. Those frame times leave 1.25 ms and 4.21 ms respectively to reach 60 FPS. [Standalone intervals, configuration and GPU measurements](../../build/probe-gather-20261124-174240/comparison.json)

| Measurement | Stationary, checkpoint → revised | Panning, checkpoint → revised |
| --- | ---: | ---: |
| Standalone FPS over the same 720 frames | 43.18 → 55.66 | 47.06 → 47.72 |
| Nsight median probe gather | 4.20 → 4.13 ms | 3.83 → 3.54 ms |
| Nsight median GPU frame | 17.68 → 17.63 ms | 21.51 → 21.42 ms |
| Nsight p95 GPU frame | 18.26 → 18.02 ms | 21.93 → 22.24 ms |

The stationary comparison uses frames 1201–1921. The pan uses frames 961–1681 and covers nine complete camera cycles. Restoring the checkpoint shaders while retaining the larger filter buffer reproduced **43.3/47.1 FPS**. Nsight and the engine’s own GPU timestamps both measured a smaller stationary improvement than the standalone frame rate implies. The cause of that difference remains unresolved. All 160 captured frames exceeded 16.67 ms. [Stationary trace](../../build/probe-gather-20261124-174240/trace-static/trace/summary.json) · [Pan trace](../../build/probe-gather-20261124-174240/trace-pan/trace/summary.json) · [Stationary shader control](../../build/probe-gather-20261124-174240/buffer-only-static/rates.json)

In separate launches, the engine’s GPU timestamp queries measured the gather over 240 warmed-up frames. The stationary median fell from 3.54 to 3.32 ms and the pan from 3.79 to 3.53 ms. The 3 ms gather target remains open. During the Nsight pan, sampling, tracing, shading and accumulating probe hits took 5.43 ms together. Hit shading accounted for 2.41 ms. [Stationary timestamps](../../build/probe-gather-20261124-174240/nodes-gather-static/node-summary.json) · [Pan timestamps](../../build/probe-gather-20261124-174240/nodes-gather-pan/node-summary.json)

The vertical filter computes the sky and bounce variance per unit of history, the effective history age and the anchor-normal projection once per probe. Each receiver still evaluates the full gather with its own surface normal, footprint and motion sampling budget. The statistics use one extra 16-byte vector per probe, or 2.47 MiB across 162,000 slots. [Probe filter](../../Data/Shaders/VoxelGI/screen_probe_filter.glsl) · [Gather](../../Data/Shaders/VoxelGI/screen_probe_gather.glsl)

Across 36 native-4K motion and recovery captures compared with the same settled images, the mean share of moving pixels differing by more than 16 display levels changed from **0.03181% to 0.03286%** on turns, **0.01550% to 0.01542%** moving forward and **0.01197% to 0.01149%** strafing. Turn RMS moved from 1.077 to 1.080 on the 8-bit scale. Separate 359-frame motion runs passed Vulkan validation with reflections enabled and in diffuse-only mode. All 660 RenderCore tests and 16 targeted Vulkan integration tests passed. [Motion scores](../../build/probe-gather-20261124-174240/motion/report.json) · [Vulkan validation](../../build/probe-gather-20261124-174240/validation.log) · [RenderCore tests](../../build/probe-gather-20261124-174240/render-tests.log) · [Vulkan integration tests](../../build/probe-gather-20261124-174240/ray-tests.log)

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
