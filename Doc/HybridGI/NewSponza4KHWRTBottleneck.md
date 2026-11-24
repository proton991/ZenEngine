# New Sponza at native 4K with hardware ray tracing

On 2026-11-24, the default `Optimal` GI preset on an RTX 5080 delivered **46.6 FPS stationary and 43.9 FPS panning** with the Kloofendal HDR environment enabled. The steady-state bottleneck was GPU work in the screen-probe GI algorithm. The full-resolution gather was the largest individual pass. Sampling, tracing, shading, and accumulating probe hits together cost more than the gather during the pan. [Measurement summary](../../build/hwrt-4k-default-20261124-132935/bottleneck.json)

The earlier **22.7 FPS** stationary result used a saved per-pixel ReSTIR configuration with a 1920×1080 GI target. The default preset on this GPU gathers screen probes at 3840×2160. In the earlier ReSTIR setup, the pre-enhancement and current RHI builds both reproduced the HDR slowdown. On the current build, enabling the HDR environment added 17.15 ms to the median GPU frame, including 16.03 ms in `HybridTrace`. [Preset implementation](../../ZenCore/Source/Graphics/RenderCore/V2/VoxelGISettings.cpp) · [Original run](../../build/new-sponza-env-4k-20261124-123823/results.json) · [RHI and environment comparison](../../build/sponza-rhi-attribution-20261124-125919/comparison.json)

## Default preset timings

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

## What 60 FPS requires

The budget is **16.67 ms per frame**. Reaching it requires saving **4.37 ms stationary and 5.45 ms panning** at the median, or **5.34 ms stationary and 6.32 ms panning** at p95. All 160 captured GPU frames exceeded the budget. Native 4K at 60 FPS remains an optimization target whose feasibility at the current image quality needs an implementation and measurements.

1. **Reduce gather work on smooth surfaces.** The gather evaluates roughly 16–20 probes per pixel. Earlier profiling found instruction issue limited this pass. Test a simpler reconstruction where depth, normals, and lighting agree, retaining the full gather at discontinuities and thin geometry. Compare fine detail and rough reflections against the current preset. [Gather implementation](../../Data/Shaders/VoxelGI/screen_probe_gather.glsl) · [Earlier gather experiments](../NativeGIOptimization.md#o2-gather-done-2026-11-20-94--48-ms-15-ms-target-not-met)
2. **Test a frame-wide probe ray budget during motion.** The scheduler gives extra samples to young or relocated probes. Test redistributing a fixed budget toward recently exposed surfaces. Measure p95 GPU time during the same pan and compare noise on newly visible surfaces, glossy reflections, and convergence after the camera stops. [Probe scheduling implementation](../../Data/Shaders/VoxelGI/screen_probe_proposal.comp)
