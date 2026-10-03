# VoxelGI performance

## Sponza forward-movement FPS drop, 2026-10-03

Moving forward from the default camera without rotating it made the roof fill
the maximized 2560x1377 viewport and reduced the Release demo to about 42 FPS.
The expensive work was the fragment shader's environment-visibility traversal:
six rays per shaded pixel could walk the empty padding of the cubic voxel grid
after leaving Sponza's much shorter geometry bounds.

`VoxelGIRenderer` now supplies conservative occupied-cell bounds derived from the
current scene AABB. The bounds include committed instance transforms and vertex
deformation, are padded for closed triangle/cell contacts and rounding, and fall
back to the full grid for invalid input. The shader tightens only the exit side
of each axis, allowing rays originating in empty padding to enter and hit geometry.
Base-level occupancy, DDA increments, z/y/x tie priority and finite endpoints stay
unchanged. Compute injection and sky traversal are unchanged. This does not reduce
GI resolution, cone count, shadows or image quality.

Matched MSVC Release measurements on the RX 7900 XT use the same 256-cubed grid,
geometry voxelizer, six cones, owner reflectance and 2560x1377 G-buffer. They run
serially with 60 startup/warmup frames and 360 measured frames, fixed animation
steps, threaded RHI, async compute disabled, and VSync, validation and overlay
injection disabled. The baseline is commit `7f24d8ab`. Camera positions follow
the original viewing direction at 1, 0.75, 0.5, 0.25 and 0.05 times the initial
distance to the scene center. FPS is `1000 / mean CPU frame interval`, including
frame-slot backpressure. Roof results average two trials per variant in
baseline/fixed/fixed/baseline order; other positions have one matched pair.

| View | Before FPS | Fixed FPS | Before GPU median | Fixed GPU median |
| --- | ---: | ---: | ---: | ---: |
| Starting camera | 174.9 | 576.7 | 5.50 ms | 1.50 ms |
| Early approach (0.75) | 90.8 | 376.4 | 10.84 ms | 2.40 ms |
| Roof fills viewport (0.5) | 43.8 | 187.9 | 22.09 ms | 5.24 ms |
| Close roof (0.25) | 35.0 | 107.0 | 28.35 ms | 9.18 ms |
| Inside near center (0.05) | 92.1 | 101.8 | 10.77 ms | 9.38 ms |

At the roof, the SceneLighting median falls from 21.25 to 4.40 ms and CPU frame
p95 falls from 24.68-25.79 to 5.97-5.98 ms. Pass intervals overlap and must not
be summed. The [measurement record](../build/scene-stutter-investigation/performance-summary.json)
contains exact commands, camera positions and timings.

A normal launch was then maximized and moved with W, keeping the camera direction
unchanged and leaving VSync, validation and the installed overlay enabled. The
fixed roof view holds approximately 165 FPS at the display limit, the closer
roof view measures 109-110 FPS, and the interior view measures 102-103 FPS.
[Interactive observations and screenshots](../build/scene-stutter-investigation/observations.jsonl)
are separate from the controlled measurements above.

Verification:

- Debug and Release builds succeed. Each passes all 4 cone integration tests and
  564 enabled RenderCore tests; 7 existing RenderCore tests remain disabled.
  The integration suite checks 393,432 GPU rays per build against the frozen
  scalar traversal at 64/128/256 resolution in all four submission modes,
  including rays entering from empty padding, parallel rays and finite endpoints.
  [Native results](../build/scene-stutter-investigation/final-tests/results.json).
- All 17 matched image cases have byte-identical screenshots and HDR components:
  Sponza and room scenes with both voxelizers at 64/128/256, thin/slanted/cutout
  fixtures, transformed geometry and the Sponza interior. Full-resolution captures
  also match at all five approach positions.
  [Image results](../build/scene-stutter-investigation/images/results.json).
- All [162 GI regression cases](../build/scene-stutter-investigation/gi-regression/results.json)
  and [18 glTF rendering cases](../build/scene-stutter-investigation/gltf-smoke/report.json)
  pass with validation and synchronization validation enabled, including forward
  materials, transmission, skinning and morph animation.
- All five affected SPIR-V modules pass Vulkan 1.1 validation. Changed C++ files
  pass clang-format 19.1.5 verification and the owned-source no-exceptions check.

`Data/engine.cfg` is restored byte for byte. Logs, matched executables/shaders and
captures remain under `build/scene-stutter-investigation`. These results cover
this AMD workload; the improvement is smaller inside the scene, where rays have
less empty padding to traverse.

## RX 7900 XT environment visibility fix, 2026-09-30

At 2560x1377, the configured Sponza starting camera now measures **74.53 FPS**
versus **42.66 FPS** with the original shaders, a **1.75x** throughput increase.
The retained change replaces dynamic vector indexing with vector selects in the
fragment shader's environment-visibility traversal. It preserves base-level voxel
occupancy, the existing z/y/x tie priority, finite-segment endpoints and occlusion.
The compute injection and sky shaders retain their original binaries.

Both variants use the same current MSVC Release executable, 256-cubed cone GI,
geometry voxelization, owner reflectance, six cones, five lights with the animated
fifth light, a 2048-square G-buffer and 1024-square shadow faces. Each accepted
trial has 120 warmup and 240 measured frames, fixed 1/60-second light-animation
increments, validation disabled and VSync disabled. Three accepted throughput
trials per variant run serially; FPS below is the median of those trials. A
separate matched pair collects GPU timestamps. Pass intervals overlap and must
not be summed.

| Measurement | Original shaders | Retained shaders |
| --- | ---: | ---: |
| Throughput, median FPS | 42.66 | 74.53 |
| Throughput, mean frame interval across trials | 23.42 ms | 13.38 ms |
| Profiled GPU frame, mean | 23.97 ms | 13.76 ms |
| SceneLighting GPU interval, mean | 18.81 ms | 8.77 ms |

A normal launch with validation and VSync enabled, without a `--mode` override,
measured 73.62 FPS using the same frame sequence. The original reported 30 FPS
was not exactly reproduced at the configured camera. The initial benchmark pair
overlapped native-test warmup and is explicitly excluded; the three accepted
pairs and separate profile pair are identified in the
[measurement record](../build/cone-visibility-fix/performance/results.json).

Validation on the retained implementation:

- [32 native tests pass](../build/cone-visibility-fix/final-native.json), including
  196,680 GPU comparisons against the frozen scalar traversal across 64/128/256
  grids and all four submission modes. These cover sparse black occluders, a
  one-cell wall, axis-aligned and nearly parallel rays, boundary ties, outside
  origins and finite endpoints, with independent known hit/miss cases.
- [15 frozen image comparisons](../build/cone-visibility-fix/images-final/results.json)
  are byte-identical for every HDR lighting component and final screenshot:
  Sponza/room at 64/128/256 with both voxelizers, plus thin/slanted/cutout fixtures.
- Five new or changed SPIR-V modules validate for Vulkan 1.1. The demo's existing
  mode-switch, resize/restore and revoxelization
  [smoke test passes](../build/cone-visibility-fix/smoke-result.json) with
  synchronization validation enabled using a byte-identical executable alias
  covered by the existing RTSS exclusion. The ordinary filename produces
  `PRESENT_AFTER_WRITE` diagnostics after resize in the installed overlay
  environment; both logs are retained. No global overlay setting was changed.
- [Window input verification](../build/cone-visibility-fix/keys-result.json)
  confirms VoxelGI at startup, key/numpad 1 for VoxelGI, key/numpad 2 for voxelization
  and no key-3 binding. Deferred PBR code and diagnostic `--mode=2` remain available;
  diagnostic mode IDs stay independent of keyboard bindings.

`Data/engine.cfg` is restored byte for byte. Exact commands, logs, shader snapshots
and image captures are retained under `build/cone-visibility-fix`. A broader
experiment that also changed compute traversal was rejected after small HDR
differences; its approximately 84 FPS result is not the retained implementation.
These measurements concern this AMD cone workload, separately from the M8
64/128-cubed directional-GI matrix and historical NVIDIA results below.

## Historical RTX 5080 investigation, 2026-09-25

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

The runner records 120 warmup frames followed by 1200 measured frames, at 1920×1080, threaded RHI and async compute enabled. It saves per-frame times, NVIDIA activity samples, configuration, exact command, GPU/driver details, executable/SPIR-V hashes, logs and a JSON summary. `--validation` enables Vulkan validation for a comparison. Overlay injection is disabled by the benchmark runner to control measurement conditions; the interactive launcher does not disable the user's overlay. Run GPU measurements serially and keep the camera unchanged. The G-buffer matches the window; the results below predate that change and used a 2048² G-buffer, which the runner's removed `--gbuffer` option selected.

## Measured conditions

- RTX 5080, driver 616.92; Ryzen 9 9950X3D; Windows 11 25H2.
- Current Sponza configuration: compute voxelization, 256³ grid, owner reflectance, six cones, five point lights, shadows, environment lighting and the animated fifth light.
- Key 3 resolves to **`cone`**, now the only supported voxel GI method.
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

This resolves the measured key-3 CPU starvation and exceeds the requested 90% activity target for the current cone configuration. The directional implementation has since been [retired](DynamicVoxelGIM8Measurements.md#retirement-evaluation-2026-09-30). These historical Cone results do not certify other scenes, resolutions or GPUs.
