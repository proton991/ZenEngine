# New Sponza: fullscreen 4K Compute GI versus HWRT

**Executable clarification:** these historical captures used `scene_renderer_demo`, temporarily copied to `bin/7zFM.exe` by the old launcher. They are not ZenApp measurements. Subsequent profiling must launch `ZenApp.exe` directly with startup arguments; the current `tools/profile_hybrid_gi.py` requires a ZenApp `.zenrender` setup. The archived runner below exists only to describe the original evidence, not the approved workflow for new captures.

The subsequent [ZenApp New Sponza 4K profile](ZenAppNewSponza4KNsightProfile.md) contains direct ZenApp captures for both backends with async compute enabled.

Measured 2026-11-06 on an RTX 5080 using NVIDIA Nsight Graphics 2026.3.1 GPU Trace. The camera is stationary in the center of the courtyard. Both backends rendered a verified 3840 × 2160 fullscreen framebuffer.

HWRT reduced the median frame interval from **28.544 ms to 18.292 ms**, a **35.9% reduction** or **1.56× frame throughput** under Nsight's base-clock capture settings. Diffuse and specular traversal account for the largest pass differences. These are whole-renderer backend comparisons, including their respective direct-shadow providers; they do not isolate a GI-only substitution.

| Measurement | Compute GI | HWRT |
| --- | ---: | ---: |
| Nsight frame median, median of three runs | 28.544 ms | 18.292 ms |
| Nsight frame p95, median of three run p95s | 29.195 ms | 18.454 ms |
| Native control throughput, 600 frames | 40.43 FPS | 66.00 FPS |
| Native control GPU frame median | 24.739 ms | 14.954 ms |

The native controls use normal GPU clocks and the engine's timestamp profiler without Nsight injection. They are one control run per backend. Their throughput comes from CPU frame intervals; it is not the reciprocal of the Nsight timing. Do not mix those conditions when calculating a speedup.

## Scene and capture conditions

- Scene: `E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf`, 3,747,018 triangles, imported materials and lights. Dynamic light animation, visible light markers, and UI are disabled.
- The courtyard center is authored position `(0, 1.59221947, 0)`, preserving the original camera's eye height and direction. Normalized engine eye is `(-0.051807632, -0.199212402, -0.068445134)`; target is `(0.912103148, -0.004898672, 0.113538636)`. This avoids the column beside the scene bounding-box center. See [camera.json](../../build/nsight-newsponza-4k-center-20261106/camera.json).
- Physical 4K display, borderless fullscreen at its native resolution; the engine logged `Fullscreen framebuffer: 3840x2160` for all six captures. Native snapshots: [Compute GI](../../build/nsight-newsponza-4k-center-20261106/native-compute/center.png), [HWRT](../../build/nsight-newsponza-4k-center-20261106/native-hardware/center.png). The original imported lighting is retained; the view is dark.
- Vulkan Release build; RHI thread and async compute enabled, VSync off, validation disabled, fixed timestep. Compute runs explicitly disable ray tracing; HWRT runs enable ray queries and acceleration structures.
- Probe spacing 16 pixels, diffuse budget 64 rays per probe, one specular sample per 2 × 2 pixel block, 32-frame history, temporal accumulation and filtering enabled. Radiance voxel resolution 128³; field resolution 64³, 60,419 allocated bricks, all 23 local volumes ready. No field-construction passes occur in the measured traces.
- Three independent captures per backend, 30 frames each, starting 60 seconds after launch. Capture order: Compute 0, HWRT 0, HWRT 1, Compute 1, Compute 2, HWRT 2.
- GPU clocks locked to base; API timestamps, hardware-event timestamps disabled, multi-pass metrics disabled. All six runs use identical executable and SPIR-V hashes. Per-run settings and complete launch commands are retained.
- RTX 5080 / GB203, driver 621.01, Ryzen 9 9950X3D, 32 GB system RAM, Windows 11 25H2 build 26300, hardware scheduling enabled. Nsight reports 12,339–12,341 MiB committed VRAM for Compute and 12,042 MiB for HWRT, with **0 MiB demoted** in every capture. These are capture-reported allocations, not lifetime peaks.

This uses the live [Nsight GPU Trace profiler](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html). The frame intervals and marker measurements come from its official exports. Each table entry below is a median of the three per-run medians unless stated otherwise. Pass intervals can overlap or include waits, so they are not added into a frame-time total or presented as exclusive frame shares.

## Repeated frame measurements

| Run | Frame median | Frame p95 | Nsight capture |
| --- | ---: | ---: | --- |
| Compute 0 | 29.013 ms | 29.570 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/compute-0/7zFM_2026_11_06_16_42_55.ngfx-gputrace) |
| Compute 1 | 28.366 ms | 28.844 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/compute-1/7zFM_2026_11_06_16_49_09.ngfx-gputrace) |
| Compute 2 | 28.544 ms | 29.195 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/compute-2/7zFM_2026_11_06_16_51_09.ngfx-gputrace) |
| HWRT 0 | 18.292 ms | 18.454 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/hardware-0/7zFM_2026_11_06_16_45_29.ngfx-gputrace) |
| HWRT 1 | 18.353 ms | 18.486 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/hardware-1/7zFM_2026_11_06_16_47_18.ngfx-gputrace) |
| HWRT 2 | 18.278 ms | 18.356 ms | [GPU Trace](../../build/nsight-newsponza-4k-center-20261106/hardware-2/7zFM_2026_11_06_16_53_15.ngfx-gputrace) |

All captures contain 30 complete measured frames and the same 25 marker passes in every frame. Successful-run logs contain no engine errors, reported Vulkan errors, or trace-buffer overflow. Validation was disabled, so this is not a validation-layer correctness test. [comparison.json](../../build/nsight-newsponza-4k-center-20261106/comparison.json) retains exact values, environment metadata, and input hashes.

## Largest passes

| Nsight marker | Compute GI | HWRT | Compute minus HWRT |
| --- | ---: | ---: | ---: |
| `ProbeTrace` | 7.654 ms | 1.206 ms | +6.449 ms |
| `ProbeSpecularTrace` | 4.104 ms | 0.723 ms | +3.381 ms |
| `ProbeReconstruct` | 4.607 ms | 4.588 ms | +0.019 ms |
| `SceneLighting` | 5.362 ms | 5.638 ms | −0.276 ms |
| `ForwardTranslucent` | 2.341 ms | 2.196 ms | +0.145 ms |
| `OffScreen` | 1.588 ms | 1.554 ms | +0.033 ms |
| `ProbeAdaptive` | 0.520 ms | 0.520 ms | <0.001 ms |

The full 25-pass comparison is in [passes.csv](../../build/nsight-newsponza-4k-center-20261106/passes.csv). Diffuse traversal is about 6.35× faster with HWRT; specular traversal is about 5.68× faster. Reconstruction remains approximately 4.6 ms with either provider, and SceneLighting remains over 5 ms. Consequently, traversal is the first Compute-specific optimization target, while reconstruction and direct lighting are substantial shared costs at 4K.

These names are literal Nsight marker labels, not event IDs. Search them in either representative trace (Compute 2 and HWRT 0). The table export does not provide a stable GUI event-ID mapping. Source definitions are in [ScreenProbeGather.cpp](../../ZenCore/Source/Graphics/RenderCore/V2/ScreenProbeGather.cpp) and [DeferredLightingRenderer.cpp](../../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp).

## Counter evidence and interpretation

Counters are summarized from each run's `metrics.json`, derived from `BASE/D3DPERF_EVENTS.xls`. Percentages of sampled stall reasons are not percentages of elapsed frame time.

| Pass and backend | SM throughput | Active compute warps | SIMT efficiency | Main sampled dependency stall |
| --- | ---: | ---: | ---: | --- |
| Compute `ProbeTrace` | ~40% | ~29% | ~20% | L1TEX long scoreboard ~37% |
| Compute `ProbeSpecularTrace`, runs 1–2 | 63–65% | 57–58% | ~19% | L1TEX long scoreboard ~45% |
| HWRT `ProbeTrace` | ~18% | ~30% | ~70% | RT-core long scoreboard 34–35% |
| HWRT `ProbeSpecularTrace` | 27–28% | 36–37% | 66–67% | L1TEX ~36–37%, RT-core ~30% |
| `ProbeReconstruct`, both backends | ~50% | 58–60% | ~95% | L1TEX long scoreboard ~64% |

Compute diffuse traversal combines low lane utilization with dependent memory waits: L1TEX throughput is only about 7% and L2 throughput about 13%, despite the long-scoreboard samples. This supports investigating ray coherence, dependent march steps, and register pressure before assuming throughput saturation. Roughly 86% of the register file is allocated while only 29% of compute warps are active. These are profiling inferences, not isolated causal experiments.

Compute specular traversal also has low lane utilization and substantial local-memory traffic in runs 1–2. Inspecting register pressure and generated code is a candidate next step. Reconstruction has much higher lane utilization, but L1TEX dependency stalls dominate its samples, with roughly 55–57% L2 throughput. Reducing redundant full-resolution reconstruction accesses is another candidate. No speedup is claimed for unimplemented changes, and no image-quality equivalence test was performed between providers.

**Repeat variability:** Compute specular traversal takes 5.168 ms in run 0 versus 4.095 and 4.104 ms in runs 1–2. Run 0 also has different occupancy and register counters despite identical executable and SPIR-V hashes. Nsight reports cached Vulkan shader recompilation enabled; this is consistent with profiler/compiler variability, but the cause was not isolated. All three runs remain in the reported statistics. Nsight's GUI reports missing shader debug information, limiting source-level call-graph attribution; pass-level timings and counter exports remain available.

## Excluded pilot and scope limits

An initial pilot with async compute disabled and multi-pass metrics enabled encountered `VK_ERROR_DEVICE_LOST` during field preparation, before a valid trace was recorded. The Windows NVIDIA event contained `SKEDCHECK36_DEPENDENCE_COUNTER_UNDERFLOW`. Its [launch log](../../build/nsight-newsponza-4k-center-20261106/failed-compute-async0-multipass/launch.log) and [driver events](../../build/nsight-newsponza-4k-center-20261106/failed-compute-async0-multipass/driver-events.xml) are retained. The final protocol enabled async compute and disabled multi-pass metrics; all six captures completed. Since two settings changed, this does not establish the fault's cause or demonstrate an engine fix.

The results describe a settled, stationary courtyard view. Camera movement, cold construction, dynamic geometry, and other viewpoints were not measured in this comparison. Earlier display probes and native async-off pilots are retained separately and excluded from the tables above.

## Artifacts and reproduction

Evidence root: [build/nsight-newsponza-4k-center-20261106](../../build/nsight-newsponza-4k-center-20261106). Each successful run contains the native `.ngfx-gputrace`, official `BASE/` exports, `summary.json`, `metrics.json`, `engine.cfg`, `command.json`, `inputs.json`, and `launch.log`. Native controls retain frame/pass CSVs, profile JSON, logs, and 4K snapshots. [native-comparison.json](../../build/nsight-newsponza-4k-center-20261106/native-comparison.json) holds their exact measurements.

The capture executable is frozen at [frozen-bin/scene_renderer_demo.exe](../../build/nsight-newsponza-4k-center-20261106/frozen-bin/scene_renderer_demo.exe), SHA-256 `9304dcbaf10b6a8de85cc9f57b8c3e1b3fb9c7e183bba7d0d8da1a4798a0b2a2`. The engine revision was `3ea1e7f9a2f74e40bb9fbfcdcbc2f1b2b219f324`; revision records, launch-only fullscreen patches, and shader hashes are preserved in the evidence directory. The glTF SHA-256 is `e04c4c540c74bdddcbd3f590a85c14119bfd5839702246a23b3a737c0cce2400`; this identifies the glTF file, not a hash of every external asset.

To capture additional repeats, enable the physical 3840 × 2160 display, preserve the recorded scene/shader inputs, and run from the repository root with a new repeat index:

```powershell
python build/nsight-newsponza-4k-center-20261106/run.py trace --backend compute --repeat 3 --delay 60000
python build/nsight-newsponza-4k-center-20261106/run.py trace --backend hardware --repeat 3 --delay 60000
```

The archived runner uses the frozen executable and archived fullscreen-capable profiler helper, and restores `Data/engine.cfg` after each run. `analyze.py` aggregates and verifies the original three repeats per backend. The traces open directly in Nsight Graphics; the `7zFM` filename comes from the repository's executable-launch workaround.

Temporary fullscreen source changes, the engine configuration, and the original display arrangement were restored after profiling. The ordinary Release demo was rebuilt successfully. See [restoration.json](../../build/nsight-newsponza-4k-center-20261106/restoration.json) and [restore-build.log](../../build/nsight-newsponza-4k-center-20261106/restore-build.log). No rendering or GI algorithm changes were retained.
