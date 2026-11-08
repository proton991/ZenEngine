# Sponza center-view FPS investigation

Measured on 7 November 2026, Asia/Seoul. This report records the Release ZenEditor investigation prompted by the reported 20–30 FPS after moving into the center of Sponza.

## Result and remaining issue

The slowdown was reproduced. In the tested center view, Release ZenEditor delivered approximately **31–32 FPS while stationary and 26–27 FPS during continuous camera movement**. Disabling emissive GI made little difference. The earlier approximately 132 FPS result used a different camera and a smaller render extent, so it was not representative of this view.

A matched-camera, matched-render-extent **standalone ZenApp** Nsight GPU Trace measured a **34.680 ms median GPU frame**, with **19.906 ms in `ForwardTranslucent`**. Transparent rendering is the dominant measured GPU pass. The center view looks toward the cypress foliage, whose source `LeafSpring` material is alpha blended and covers 1,354,570 triangles.

One small shader optimization was retained: helper fragment invocations skip shadow visibility evaluation. It does **not** resolve the main transparency bottleneck. The unsuccessful depth experiments and all temporary editor profiling code were removed. No texture-quality reduction, leaf alpha-mode conversion, mesh LOD change, or permanent GI-quality reduction was retained from this investigation.

## Scene and measurement conditions

Scene: [five-model Sponza assembly](E:/Dev/ZenAssetProcessor/build/multi-model-assembly-desktop/sponza.zenscene), comprising main architecture, curtains, ivy, trees, and candles.

| Condition | Editor measurements | Standalone GPU trace |
|---|---|---|
| Application | Release `zen_editor.exe`, temporary profiling build | Release `ZenApp.exe` |
| GPU | NVIDIA GeForce RTX 5080 | NVIDIA GeForce RTX 5080 |
| CPU / driver | Same machine | Ryzen 9 9950X3D / NVIDIA 621.01, recorded by Nsight |
| Backend | Vulkan, hardware ray queries, threaded RHI | Vulkan, hardware ray queries, threaded RHI |
| Framebuffer | 3840 × 2088 | 2360 × 1264 |
| Scene render extent | **2360 × 1264** | **2360 × 1264** |
| Window | Maximized, not fullscreen | Windowed, not fullscreen |
| VSync | Enabled | Disabled by startup override |
| Frames in flight | 2 | 2 in supplied setup |
| Async compute | Disabled | Disabled; confirmed in runtime log |
| Validation during measurements | Disabled | Disabled |
| GPU timing instrumentation | Editor RDG GPU timing enabled by probe | Nsight Graphics 2026.3.1 GPU Trace |
| GPU clocks | No clock override applied by editor runner | `unaltered` |
| Culling | Renderer default GPU culling enabled | `--gpu-culling=1`, runtime confirmed `gpu_indirect` |

The user explicitly approved Release ZenEditor FPS profiling, including matched-camera and before/after checks, as an exception to the repository's normal ZenApp-only profiling rule. The Nsight capture itself used the required standalone executable and `tools/profile_hybrid_gi.py`. `Data/engine.cfg` was not changed for profiling.

The principal camera uses scene-normalized coordinates:

| Camera property | Value |
|---|---|
| Eye | `(0, 0, 0)` |
| Initial target | `(0, 0, -1)` |
| Vertical FOV | 58.51198 degrees |
| Aspect | 1.8670886 |
| Near / far | 0.0027588583 / 275.88583 |
| Projection | Perspective |
| Navigation scene scale | 1.3156621 |

The assembly normalization center is `(1.8778648, 8.813047, 2.4809227)` with scale `0.027588582`. This distinguishes the normalized camera position from the source models' coordinates.

GI settings were resolution 128, probe gather, probe spacing 16, four samples, adaptive sampling, 32 history frames, temporal accumulation and filtering enabled, hardware ray shadows enabled, and shadow-map resolution 1024. Indirect diffuse, specular occlusion, and analytic lighting remained enabled in the final emissive comparison. Scene environment lighting was disabled after imported lights replaced it; the skybox remained visible.

There were 24 submitted scene lights. The candle assembly instance has `submitLights=false`, so its 10,000 authored point-light nodes were not enabled as 10,000 runtime lights. The emissive toggle controls emission's GI contribution; the visible emissive flame material remains visible in both cases.

## Final editor emissive comparison

These results include the retained helper-shadow change. Each phase lasted 600 scene frames. The camera was reset to the same pose at each phase boundary. Movement phases applied `CameraInput.look.x = 0.01` every frame: a small continuous yaw, approximately 1.38 degrees over 600 frames. This is a reproducible navigation workload, not an exact replay of the user's input.

| Emissive GI | Camera | Sampled frame interval, ms | Approximate FPS |
|---|---|---:|---:|
| On | Stationary | 31.904 | **31.34** |
| On | Moving | 37.840 | **26.43** |
| Off | Stationary | 31.146 | **32.11** |
| Off | Moving | 37.711 | **26.52** |

Method: the editor smooths its wall-clock frame interval as `0.9 × previous + 0.1 × current`. The table averages three logged smoothed intervals per phase and reports `1000 / mean_interval_ms`. Samples were taken at phase-local frames 240, 360, and 480, before the screenshot capture at frame 480. Loading, the initial settling samples, and capture-induced stalls were not included. These are approximate editor-loop throughput observations, not a complete presentation-timing distribution, GPU-only timings, or latency measurements.

| Phase | Global sample frames | Logged smoothed intervals, ms |
|---|---|---|
| On, stationary | 240 / 360 / 480 | 31.733 / 33.244 / 30.736 |
| On, moving | 840 / 960 / 1080 | 38.501 / 37.722 / 37.296 |
| Off, stationary | 1440 / 1560 / 1680 | 31.118 / 31.158 / 31.162 |
| Off, moving | 2040 / 2160 / 2280 | 38.207 / 37.349 / 37.577 |

The on/off stationary difference is about 0.76 ms, and the moving difference about 0.13 ms. These were sequential phases of one run with unlocked GPU clocks, not repeated randomized trials; they do not establish a precise emissive performance penalty. They do establish that disabling emissive GI does not remove this slowdown.

Evidence: [results JSON](E:/Dev/ZenEngine/build/editor-fps-20261107/results.json), [editor log](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.log), and [process identity, arguments, and resident-memory samples](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.measurement.json).

Images: [emissive on, stationary](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.zenrender.0.png), [emissive on, moving](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.zenrender.1.png), [emissive off, stationary](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.zenrender.2.png), [emissive off, moving](E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.zenrender.3.png).

## Camera dependence and the earlier higher FPS result

The initial outside “Frame All” view was approximately 144 FPS. A center sweep before the helper-shadow change produced the following approximate stationary values, calculated from the same phase-local 240/360/480 samples:

| Normalized eye | Direction | Mean sampled interval, ms | Approximate FPS |
|---|---|---:|---:|
| `(0, -0.18, 0)` | +X | 10.220 | 97.8 |
| `(0, -0.18, 0)` | +Z | 8.430 | 118.6 |
| `(0, -0.18, 0)` | -X | 17.789 | 56.2 |
| `(0, -0.18, 0)` | -Z | 28.147 | 35.5 |
| `(0, 0, 0)` | +X | 10.009 | 99.9 |
| `(0, 0, 0)` | +Z | 8.556 | 116.9 |
| `(0, 0, 0)` | -X | 13.761 | 72.7 |
| `(0, 0, 0)` | -Z | 32.100 | 31.2 |

See [camera sweep log](E:/Dev/ZenEngine/build/editor-fps-20261107/center-sweep.log) and the [slow-view setup](E:/Dev/ZenEngine/build/editor-fps-20261107/center-sweep.zenrender.7.actual.zenrender). “GI ready” indicates lighting preparation has completed; it does not imply a low steady-state rendering cost.

The earlier emissive experiment reported approximately 132.2 FPS on / 137.6 FPS off in **ZenApp at 1920 × 1080**, with a different courtyard camera, three frames in flight, VSync disabled, and completed-scene-frame throughput measured against wall time. It must not be presented as this editor center-view result. Its original evidence remains in [the earlier measurement](E:/Dev/ZenEngine/build/candle-emissive-20261107/emissive-on.measurement.json).

## Standalone GPU Trace breakdown

The trace used the slow center camera and the editor's 2360 × 1264 scene extent, before the retained shader change. It excludes editor UI work and uses different presentation settings as listed above. Its GPU frame time is a separate metric from the editor's smoothed wall-clock interval.

| GPU frame statistic | Milliseconds |
|---|---:|
| Captured frames | 30 frames |
| Median | **34.68025** |
| Mean | 34.67270 |
| P95 | 34.92930 |
| Maximum | 35.04700 |

| GPU pass | Median interval, ms |
|---|---:|
| **ForwardTranslucent** | **19.90600** |
| ProbeTrace | 5.79677 |
| OffScreen | 2.87368 |
| ProbeReconstruct | 2.21645 |
| SceneLighting | 1.82042 |
| ProbeSpecularTrace | 0.73734 |
| ProbeAdaptive | 0.27512 |
| ForwardBackground | 0.17918 |
| ReadProbeDemand | 0.11550 |
| ProbeFilter | 0.07926 |

`ForwardTranslucent` alone is approximately 57% of the reported median frame interval. Pass intervals can include barriers and overlap; independently calculated pass medians must not be summed into an asserted frame time.

All 30 GPU frame samples, in milliseconds:

```text
34.4360 34.5115 34.1875 34.7210 34.4708 34.4513 34.3122 34.3856 34.8307 34.7253
34.8186 34.6017 34.7946 34.5891 34.6583 34.5615 34.8905 34.7981 34.9422 34.6585
35.0470 34.9173 34.6216 34.8741 34.7655 34.8565 34.7020 34.9293 34.5251 34.5975
```

Evidence: [trace summary](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/summary.json), [GPU trace](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/ZenApp_2026_11_07_20_44_05.ngfx-gputrace), [per-pass export](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/BASE_UNLOCKED/D3DPERF_EVENTS.xls), [frame export](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/BASE_UNLOCKED/FRAME.xls), and [hardware/driver metadata](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/BASE_UNLOCKED/REPRO_INFO.xls).

The capture succeeded, but the profiling script's summarization step expected `BASE/FRAME.xls`. With clocks set to `unaltered`, Nsight exported `BASE_UNLOCKED/FRAME.xls`. The stored summary was generated by reading that original export directory. The initial script failure was an export-path assumption, not a missing trace or substituted CPU measurement.

## Bottleneck analysis and isolation experiments

The inspected [cypress glTF](E:/Dev/main_sponza/pkg_c_trees/NewSponza_CypressTree_glTF.gltf) marks `LeafSpring` as `BLEND` and double sided. Its 1,354,570 triangles enter the forward transparency path. The source 4096 × 4096 leaf texture has approximately 93.58% zero-alpha, 6.18% fully opaque, and 0.24% partial-alpha texels at the base level. Those source-image percentages are not screen-space coverage measurements: filtering, mips, compression, and overlapping leaves affect actual fragment work.

The forward shader evaluates material shading, indirect lighting where a deferred receiver cannot be reused, and shadow visibility for contributing lights. The measured pass cost and direction-dependent slowdown identify this path as the main GPU bottleneck. The trace does not separately quantify how much of its total comes from each material, vertex processing, fragment shading, or overdraw.

Camera movement also invalidates the cached triangle order. [OrderForwardDraws](E:/Dev/ZenEngine/ZenCore/Source/Graphics/RenderCore/V2/ForwardDrawCache.cpp:177) sorts the prepared blended triangles and rebuilds sorted indices when the view changes. This is a confirmed code path and a plausible contributor to the additional movement cost. Its CPU contribution was not separately timed; the observed movement penalty must not be attributed entirely to sorting because screen-space and GI workloads can also change.

Lighting isolation used the same stationary center view. The following are editor smoothed wall-clock intervals, averaged from phase-local frames 240, 360, and 480:

| Temporary lighting configuration | Mean sampled interval, ms |
|---|---:|
| Indirect diffuse and shadows enabled | 31.875 |
| Indirect diffuse disabled | 28.194 |
| Shadows disabled, indirect diffuse enabled | 24.174 |
| Both disabled | 20.393 |

These switches affect more than one pass, and their differences are not independent additive costs. They were diagnostic experiments, not retained optimizations. See [lighting isolation log](E:/Dev/ZenEngine/build/editor-fps-20261107/lighting-cost-probe.log) and its phase setup snapshots.

Two depth experiments did not provide a material improvement and were removed:

| Experiment | Stationary interval, ms | Moving interval, ms |
|---|---:|---:|
| Original motion baseline | 32.257 | 38.061 |
| Explicit early fragment tests | 32.176 | 38.874 |
| Depth prepass for fully opaque samples in blended materials | 32.015 | 38.104 |

These are single-run observations using the same three-sample method, not evidence of statistically significant gains or regressions. Logs: [baseline](E:/Dev/ZenEngine/build/editor-fps-20261107/motion-baseline.log), [early tests](E:/Dev/ZenEngine/build/editor-fps-20261107/early-depth-probe.log), [blend-depth prepass](E:/Dev/ZenEngine/build/editor-fps-20261107/blend-depth-probe.log). The discarded prepass patch is retained only as an investigation artifact at [blend-depth-experiment.patch](E:/Dev/ZenEngine/build/editor-fps-20261107/blend-depth-experiment.patch).

## Retained change, quality check, and validation

[forward_material.glsl](E:/Dev/ZenEngine/Data/Shaders/SceneRenderer/forward_material.glsl:353) now excludes `gl_HelperInvocation` from shadow visibility evaluation. Material/normal derivative calculations remain before that guard. Shadow visibility results are not consumed by subsequent derivative calculations in this path. Helper invocations exist to support derivatives and do not contribute fragment output; see the [Vulkan HelperInvocation contract](https://docs.vulkan.org/refpages/latest/refpages/source/HelperInvocation.html).

The original motion baseline averaged 32.257 ms stationary / 38.061 ms moving, versus 31.904 / 37.840 ms in the final emissive-on phases. This suggests only a small benefit in these observations; the runs are insufficient to establish a precise percentage improvement. The main slowdown remains.

The stationary baseline capture and changed capture were compared within the scene region `(x=720..3039, y=210..1439)` of the full editor image, excluding UI counters. Maximum absolute channel difference was **1/255**; mean absolute difference was **0.0000039716 in 8-bit channel units**. No compared pixel differed by more than two channel levels. This supports visual parity for this tested view, not universal image equivalence across all materials; GI sampling can vary between runs.

Validation completed:

- Release `zen_editor`, `ZenApp`, `ConeVoxelGIIntegrationTest`, and `RayQueryIntegrationTest` targets built successfully. Building the last target is not a claim that its suite was run.
- All four parameterizations of `EquivalentBlendNodesPreserveSortedPixelsAndReduceNativeDraws` passed. They protect existing blend ordering/draw behavior; they are not exhaustive coverage of hardware-shadow helper handling.
- Shader formatting and `git diff --check` passed.
- Temporary editor instrumentation was removed and Release rebuilt afterward. At the end of the FPS investigation, the only retained source change was the helper-shadow guard.

Evidence: [final build log](E:/Dev/ZenEngine/build/editor-fps-20261107/final-build.log), [test XML](E:/Dev/ZenEngine/build/editor-fps-20261107/blend-tests.xml), and [test log](E:/Dev/ZenEngine/build/editor-fps-20261107/blend-tests.log).

## Memory result

The final editor run recorded **4,250,722,304 bytes peak process working set: 4.251 GB decimal / 3.959 GiB**. This includes loading and the subsequent editing/navigation phases and was below the requested 5 GB resident-memory target for this run. Samples used `GetProcessMemoryInfo`, including `PeakWorkingSetSize`, with approximately 100 ms polling. This is a process resident-memory measurement; it is not private commit, and it is not asserted to match every Task Manager column's accounting exactly.

The editor separately displayed approximately **13,233 MiB of GPU committed allocations**. This is not the CPU resident-memory figure. The result does not establish a universal sub-5-GB guarantee for other scenes or configurations. Nsight's instrumented process memory is also not a substitute for the editor loading/editing memory check.

## Recorded commands and executable identities

Working directory was `E:/Dev/ZenEngine`. The final editor profiling run used the following arguments, with temporary environment variables `ZEN_EDITOR_PROFILE_SETUP=E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.zenrender` and `ZEN_EDITOR_PROFILE_GPU=1`:

```text
E:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/zen_editor.exe
--scene=E:/Dev/ZenAssetProcessor/build/multi-model-assembly-desktop/sponza.zenscene
--settings-dir=E:/Dev/ZenEngine/build/editor-fps-20261107/settings
--mode=gi --rhi=threaded --disable-validation --frames=100000
--capture=E:/Dev/ZenEngine/build/editor-fps-20261107/helper-shadow-probe.ppm
```

The probe terminated after 2,400 loaded-scene frames, before the `--frames=100000` capture trigger. The actual screenshots are the numbered phase captures linked above, not the unnumbered command-line capture path. The exported setup's `run.gpuTimings=false` was overridden by the probe's `SetGPUTimingsEnabled(true)` call. Movement-phase setup snapshots contain the starting pose, not every animated pose.

These environment variables were investigation-only controls and are no longer supported by the cleaned-up editor. [run_editor.py](E:/Dev/ZenEngine/build/editor-fps-20261107/run_editor.py), [final-probe-code.patch](E:/Dev/ZenEngine/build/editor-fps-20261107/final-probe-code.patch), and the measurement JSON preserve how the run was made. Re-running the script against the cleaned-up editor will not reproduce the scripted camera or phase termination without restoring compatible probe instrumentation.

The GPU trace was launched with:

```text
python E:/Dev/ZenEngine/tools/profile_hybrid_gi.py
--setup=E:/Dev/ZenEngine/build/editor-fps-20261107/center-sweep.zenrender.7.actual.zenrender
--backend=hardware
--output=E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace
--no-fullscreen --width=2360 --height=1264 --async-compute=0
--clocks=unaltered --start-after-ms=45000 --frames=30
```

This produced the following actual ZenApp argument list:

```text
E:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/ZenApp.exe
--setup=E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/setup.zenrender
--backend=hardware --fullscreen=0 --width=2360 --height=1264
--rhi=threaded --async-compute=0 --gpu-culling=1 --vsync=0 --validation=0
--fixed-step --no-ui --gpu-markers --frames=90030
```

The full Nsight command, including capture buffers and layer overrides, is in [command.json](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/command.json); the exact application argument array is in [application-command.json](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/application-command.json). A repeat capture needs a fresh output directory. The current executable and shaders may differ from these historical hashes after cleanup or later development.

| Recorded item | SHA-256 |
|---|---|
| Editor profiling executable, 10,019,328 bytes | `63531062534f77ac27492faebb9ddf607729e0fd1abc8ceb9bf7bff0c83a6b4c` |
| ZenApp trace executable, 5,647,360 bytes | `ed2230a07a1c7a871cca5a75b840f4f0186bb43511e3e2b7d5832ee7f380edd8` |
| Editor probe input setup | `9100af13aaeb9cccc74aca76ec40b37b4aa073dc20913c8d86c153878807eed0` |
| Trace input setup | `7bed31f8467775c6b025fe2a5c1c4a56c3e82b982cbf18fc23d6ea0b9475663c` |
| Sponza assembly manifest | `ace2b845216e8f466b9c2bb6c40593a1419a46e24951bb3274de37da2e8c719c` |
| `Data/engine.cfg` | `5a545653a4cfe696f9e167a16d642949fe60dca1db52f48f934c7c721d168b87` |
| Retained forward shader source | `89fc063a531223f8a32564a848789bc39e5c931376b62f09673ed55a1bc7dcb0` |

Native model inputs, all under `E:/Dev/ZenAssetProcessor/build/multi-model-assembly-desktop/models/`:

| File | Bytes | SHA-256 |
|---|---:|---|
| `main-a29ba3986072b473-0f104b39e4d725f2.zenasset` | 2,097,605,372 | `54fb71017b610b87ce477f99d238956cc4f933fadedf7ed43a6f2d8342f47965` |
| `curtains-3166356e1b2258cc-43d137c21b53331d.zenasset` | 241,138,797 | `394622ea96e81a2fba0d2f298f465e2ab00f67adb97dd6402cff0bbe24ae085e` |
| `ivy-c85c97533e749eda-cc1dacb8f9ceb8a4.zenasset` | 125,329,440 | `8b1d00b791edba21dcc78f15a2d0dea7bfd32144f68712097e1b67c790ee7c10` |
| `trees-0f962e8206a5b7e0-02f287b7ab52f088.zenasset` | 186,837,677 | `84334bb0d9b7d68d814dc16eab0baf652c4d6647c4df30679ee28e61bdf2a1c2` |
| `candles-604bd4da629f1d9d-5b0b0f70230baec4.zenasset` | 166,536,505 | `854821d8445f6be76e55493daad2b7ba354e3b7b0966ab1583aa2fffc70cec5a` |

Full manifests: [initial editor identity and input hashes](E:/Dev/ZenEngine/build/editor-fps-20261107/baseline-editor.json), [trace executable, setup, package, and shader hashes](E:/Dev/ZenEngine/build/editor-fps-20261107/center-trace/inputs.json), and [final model and forward-shader hashes](E:/Dev/ZenEngine/build/editor-fps-20261107/final-inputs.json).

The linked raw evidence lives under `build/` and may be removed by build cleanup. This report embeds the principal results, conditions, frame samples, and hashes; it does not copy the approximately 1.25 GB Nsight trace into `Doc`.
