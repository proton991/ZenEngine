# Key-3 VoxelGI performance, 2026-09-25

The reported approximately 40 FPS / 30–40% GPU activity is reproducible in the Debug build after the gap fixes. The same workload in an optimized MSVC build reaches **150–157 FPS and 99% GPU activity**. The immediate bottleneck is CPU work in the unoptimized build, with additional Vulkan validation overhead. No shader, voxel resolution, lighting, shadow, or GI quality change is needed to meet the requested GPU activity target on this machine.

## Launch and reproduce

From the repository root:

```powershell
tools\run_voxel_gi_performance.cmd --width=1920 --height=1080
```

The launcher finds Visual Studio, configures/builds the `x64-windows-msvc-performance` preset, and starts mode 3 from the repository root. The current preset uses Release (`/O2`) and opts out of Vulkan validation for that launch. The measurements below were collected with RelWithDebInfo (`/O2`); they are historical measurements, not a new Release benchmark. Its executable is isolated at `build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe`; Debug builds use `build/x64-windows-msvc-debug/bin`, and ordinary validation defaults remain enabled. The launcher reads the current `Data/engine.cfg` and does not edit it. Resolution arguments are optional; without them the demo retains its normal window size.

Both Windows optimized presets now have matching build presets. For a direct build,
run `cmake --preset x64-windows-msvc-release`, then
`cmake --build --preset x64-windows-msvc-release --target scene_renderer_demo --parallel 8`
from an x64 Visual Studio developer shell. That executable is at
`build/x64-windows-msvc-release/bin/scene_renderer_demo.exe`. Model paths are resolved
relative to `Data/engine.cfg`, so launching from VS Code, the repository root or the
executable directory uses the same assets. See the [build instructions](../README.md#windows-release-build-and-run).

For a recorded benchmark, use a fresh output directory:

```powershell
python tools/benchmark_voxel_gi.py --executable build/x64-windows-msvc-performance/bin/scene_renderer_demo.exe --output build/voxel-gi-new-benchmark
```

The runner records 120 warmup frames followed by 1200 measured frames, at 1920×1080 with a 2048² G-buffer, threaded RHI and async compute enabled. It saves per-frame times, NVIDIA activity samples, configuration, exact command, GPU/driver details, executable/SPIR-V hashes, logs and a JSON summary. `--validation` enables Vulkan validation for a comparison. Overlay injection is disabled by the benchmark runner to control measurement conditions; the interactive launcher does not disable the user's overlay. Run GPU measurements serially and keep the camera unchanged.

## Measured conditions

- RTX 5080, driver 616.92; Ryzen 9 9950X3D; Windows 11 25H2.
- Current Sponza configuration: compute voxelization, 256³ grid, owner reflectance, six cones, five point lights, shadows, environment lighting and the animated fifth light.
- Key 3 resolves to **`cone`**, query backend `none`. It does not select the experimental `dynamic_voxel` method in this configuration.
- Three frames in flight; identical input configuration and shader binaries across the final Debug/performance comparisons. Configuration SHA-256: `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`.
- The benchmark's `--fixed-step` advances the demo light by 1/60 second per frame, so each build renders the same sequence of light positions. Interactive launches retain elapsed-time animation. This flag does not change the directional GI temporal-filter clock.

## Matched, warmed results

FPS is rendered throughput, `1000 / mean frame interval`, including frame-slot backpressure. It is not a measurement of display scanout or an isolated GPU timestamp. NVIDIA activity is sampled every 100 ms, excluding initialization, warmup and the first measured second. It measures time with GPU work active, **not shader multiprocessor occupancy**; it is not asserted to be the identical counter used by MSI Afterburner.

| Build / Vulkan validation | FPS | Mean / p95 frame ms | GPU activity, mean / median |
| --- | ---: | ---: | ---: |
| [Debug / on](../build/voxel-gi-performance-20260925/debug-timed-validation/summary.json) | 41.84 | 23.902 / 24.769 | 31.8% / 31% |
| [Debug / off](../build/voxel-gi-performance-20260925/debug-timed-no-validation/summary.json) | 47.08 | 21.239 / 22.057 | 35.8% / 37% |
| [RelWithDebInfo / on](../build/voxel-gi-performance-20260925/performance-validation/summary.json) | 139.56 | 7.166 / 8.166 | 96.9% / 99% |
| [Performance / off, run 1](../build/voxel-gi-performance-20260925/performance-1/summary.json) | 156.58 | 6.386 / 7.995 | 99% / 99% |
| [Performance / off, run 2](../build/voxel-gi-performance-20260925/performance-2/summary.json) | 155.83 | 6.417 / 8.039 | 99% / 99% |
| [Performance / off, run 3](../build/voxel-gi-performance-20260925/performance-3/summary.json) | 150.20 | 6.658 / 8.096 | 99% / 99% |

The median of the three performance runs is **155.83 FPS**, about **3.72×** the warmed Debug/validation result. Compiler optimization provides most of the improvement: even with Vulkan validation enabled, the optimized executable meets the GPU activity target. Disabling validation alone in Debug does not resolve the issue.

The earlier [900-frame reproduction](../build/voxel-gi-performance-20260925/initial-baselines.json) used normal elapsed-time light animation and included first-frame work: Debug/validation was 39.52 FPS at 31% median activity, Debug without validation was 44.69 FPS at 36%, and RelWithDebInfo without validation was 126.06 FPS at 99%. These support the interactive symptom, but must not be mixed with the warmed fixed-step table as an identical animation protocol.

## CPU and GPU evidence

Existing RDG instrumentation reports steady-frame compilation around 9.3–10.0 ms and execution/recording around 2.8 ms in the [Debug log](../build/voxel-gi-performance-20260925/debug-validation.log). The [optimized log](../build/voxel-gi-performance-20260925/release-no-validation.log) reports approximately 1.47 ms and 0.47 ms respectively. These are sampled CPU ranges, not a complete CPU profile or GPU execution times. Together with the controlled build comparison and low GPU activity, they establish CPU starvation in the reproduced Debug workload. This change does not remove render-graph checks or synchronization.

An independent [Nsight Graphics GPU Trace](../build/voxel-gi-performance-20260925/baseline-cone256-gpu/summary.json) records the corrected 256³ cone workload with threaded RHI and async compute: 60 frames after 40 warmup frames, **9.326 ms median / 9.697 ms p95 GPU frame time**. Nsight locks clocks to base and disables VSync; the ordinary benchmark uses normal boost clocks and a fixed light-step sequence. Do not equate these different protocols by taking reciprocal FPS.

| GPU pass range | Mean ms |
| --- | ---: |
| SceneLighting | 6.018 |
| VoxelInjectRadiance | 3.149 |
| SceneShadowFace_27 | 0.818 |
| OffScreen | 0.640 |
| SceneShadowFace_26 | 0.522 |

Pass ranges overlap across queues and must not be added to obtain total frame time. The trace reports 2164 MiB committed / 2151 MiB requested device memory; this is not a verified lifetime peak. The `.ngfx-gputrace`, exported frame/pass tables and reproduction information remain in the linked evidence directory.

Two shader experiments were evaluated and rejected. A conservative empty-block opacity lookup reduced the measured throughput to about 129 FPS; an initial variant also failed during initialization. Replacing DDA vector indexing with explicit component updates measured about 154 FPS and did not establish a repeatable gain over the 156 FPS baseline. Neither experiment is retained. Final shader sources and compiled SPIR-V match the pre-experiment snapshot byte for byte.

## Implementation and verification

The retained changes are the isolated performance preset/launcher, reusable benchmark runner, and opt-in demo flags `--warmup`, `--frame-times` and `--fixed-step`. Frame samples are buffered in memory and written after the measured loop. Normal launches keep their previous timing behavior; malformed benchmark arguments and failed timing-output writes return failure.

Debug and performance builds pass. The [verification record](../build/voxel-gi-performance-20260925/verification.json) covers identical frozen-scene images before/after and with split warmup, timing-output failure propagation, the existing mode-switch/resize/revoxelization smoke run, and the quick material/bounds/budget and static-GI suites with synchronization validation enabled. Logs contain no validation errors or reported memory leaks in successful runs. The expected file-output failure is recorded separately. The user configuration is restored byte for byte after fixture tests. Benchmark input failures are recorded in [CLI validation](../build/voxel-gi-performance-20260925/cli-validation.json).

This resolves the measured key-3 CPU starvation and exceeds the requested 90% activity target for the current cone configuration. The separate directional-GI [M8 gates](DynamicVoxelGIM8Profiling.md#remaining-m8-gates), including its post-gap-fix performance audit, remain open; these cone results do not promote automatic method selection or certify other scenes, resolutions or GPUs.
