# Brixelizer GI profiling evidence

Status, 2026-10-28: final qualification is in progress. The replacement and update paths are implemented, but the GI pass-cost target remains open. The repeated native measurements below supersede the older checkpoint. [Final-code Nsight GPU Trace captures](#final-code-nsight-gpu-trace) of the still camera against the shadow-map baseline, camera motion and dynamic geometry follow them; complete transition traces are still outstanding.

## Corrected baseline and current native measurements

The retained executable's global Compute selector enabled software direct shadows despite its legacy `rt_shadows=false` setting. The former 47% whole-frame improvement therefore was **not a matched shadow-map comparison**. A baseline override of `voxel_gi_shadows_backend=hardware`, together with `--disable-rt`, selects its shadow-map fallback while preserving Compute GI. The capture helper now preserves explicit per-feature overrides. Native manifests confirm `hardware_shadows=false`; source inspection confirms this baseline provider choice resolves to maps.

| Measurement | Current, three repeats | Corrected baseline, three repeats |
| --- | ---: | ---: |
| GPU frame median | 7.823 / 7.897 / 7.850 ms | 7.497 / 7.512 / 7.521 ms |
| GI pass median sum | 3.598 / 3.594 / 3.598 ms | 2.965 / 2.965 / 2.966 ms |
| Throughput from CPU frame intervals | 127.0 / 125.2 / 125.8 FPS | See linked run summaries |

The GI pass sum is about 21% above the corrected baseline; the 10% target is approximately 3.262 ms. Whole-frame GPU medians are about 4–5% higher. Sum-of-pass medians are a comparison metric, not a frame-time share. A matched async-compute-disabled pilot still measures 3.614 versus 2.966 ms, so queue overlap does not explain the gap. [Current repeated runs](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/workloads/finish-final), [corrected baseline](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/finish-matched-maps), [serialized comparison](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/finish-serialized).

| Current workload | FPS, three repeats | GPU frame medians, ms |
| --- | --- | --- |
| Camera motion | 114.0 / 114.0 / 117.7 | 8.693 / 8.681 / 8.544 |
| Dynamic geometry | 21.0 / 20.7 / 24.6 | 49.521 / 49.338 / 45.918 |
| Old Sponza | 383.4 / 383.5 / 383.6 | 2.368 / 2.369 / 2.367 |

Each repeat retains 300 measured frames plus the complete cold and warmup sequence, with no dropped records. Dynamic motion preserves static generation 1 and updates the dynamic volume separately. Its cost includes radiance-cache refresh and shadow work. Native frame/graph/node/queue anchors, preparation timings and peak owned memory are in [finish-evidence.json](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/finish-evidence.json); these are native IDs, not Nsight event IDs.

Retained optimizations include a hierarchy over local-volume bounds, leaf-first empty-space traversal, reusable atlas coordinates and reciprocal directions, skipping bricks whose samples cannot meet the hit band, and branchless half-open cell crossing tests. Sample counts, tolerance, field resolution and local-volume extents are unchanged. A direct-scene special case, relaxed layout checks and transposed probe strata were rejected after timing showed no improvement or a regression.

Old Sponza's bounded coverage scheduler previously revisited thousands of completed triangles. Skipping at most 1,024 completed queue entries per preparation dispatch and retaining full alpha refinement reduces the build from the earlier 283.0 seconds to 31.43 seconds in final repeat 0 (98,589 to 10,546 submitted graphs). It publishes 54,549 bricks with eight explicitly unavailable cells. Repeat 0's largest field pass is 0.594 ms; the maximum sum of field intervals in one frame is 1.456 ms, with peak owned memory 71,668,336 persistent bytes and 20,973,680 scratch bytes. All three repeats stay below 1.579 ms for the field interval sum. [Old Sponza final repeat 0](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/workloads/finish-final/old-0/native.profile.json).

New Sponza still exceeds the construction target: cold field intervals sum to at most 3.246 ms, and dynamic repeat 1 reaches 4.954 ms; `FieldSnapshotGeometry` alone spans 4.167 ms in frame 5907 (graph 6207, node 3, compute queue 1, submission group 3). These are elapsed intervals under concurrent rendering, not isolated shader service time. The 2 ms target remains open. Peak owned field storage for the recorded still and dynamic cases remains below 256 MiB persistent and 128 MiB scratch.

## Final-code Nsight GPU Trace

Nsight Graphics 2026.3.1 GPU Trace of the Release build of 20:10 (unchanged sources and SPIR-V; ninja had no work), clocks locked to base, API timestamps, GPU markers, validation off, 1600×900, async compute available. [m7_profile.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/m7_profile.py) `2026-10-28-final current|baseline --repeats 3 --start-after-ms 90000 --skip-uninstrumented` traces 30 frames per repeat, 90 s after launch. Every current repeat logged all 24 field publications (scene field and 23 lamp volumes) 44–57 s after launch, before its trace. The baseline is the archived milestone 0 executable and SPIR-V with `voxel_gi_shadows_backend=hardware` and `--disable-rt`, so both builds light with shadow maps; its launch log shows the override loaded, and the shader swap was restored by hash. [nsight_compare.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/nsight_compare.py) writes [compare.json](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/compare.json); [nsight_metrics.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/nsight_metrics.py) writes each repeat's `metrics.json`. Every folder under [2026-10-28-final](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final) keeps the `.ngfx-gputrace`, `BASE/` exports, command, configuration, input hashes and launch log. Every pass was recorded in every traced frame.

| Repeat | GPU frame median / p95 | GI pass group | ProbeTrace | ProbeSpecularTrace | SceneLighting |
| --- | ---: | ---: | ---: | ---: | ---: |
| current 0 | 9.405 / 9.696 ms | 3.753 ms | 1.553 ms | 1.172 ms | 2.096 ms |
| current 1 | 9.347 / 9.638 ms | 3.445 ms | 1.570 ms | 0.809 ms | 2.074 ms |
| current 2 | 9.425 / 9.747 ms | 3.420 ms | 1.556 ms | 0.809 ms | 2.090 ms |
| baseline 0 | 8.634 / 8.938 ms | 2.928 ms | 1.128 ms | 0.483 ms | 1.872 ms |
| baseline 1 | 8.602 / 8.844 ms | 2.999 ms | 1.121 ms | 0.541 ms | 1.866 ms |
| baseline 2 | 8.568 / 9.018 ms | 2.977 ms | 1.126 ms | 0.540 ms | 1.863 ms |

At base clocks the current frame is 9.0% longer (median 9.405 against 8.602 ms) and the GI pass group (`Probe*`, `Voxel*`, `Hybrid*`, `Field*` medians summed) 15.7% larger (3.445 against 2.977 ms; the native runs above give 21%). The two field traces carry the difference: ProbeTrace +0.43 ms (+38%) and ProbeSpecularTrace +0.27 ms (+50%). ProbeIntegrate (−0.10 ms) and ProbeFilter (−0.16 ms) offset part of it. The baseline's 0.16 ms ProbeFilter range runs at 3.5% SM throughput with 88% of samples at barriers, so it mostly waits on preceding work; small ranges are not individually reliable. SceneLighting is 0.22 ms longer although its shader differs only in preprocessor guards; the cause was not investigated.

Against the morning traces of the earlier implementation ([2026-10-28-still](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still), same harness, 75 s start), ProbeTrace fell from 3.76–4.50 to 1.55–1.57 ms, ProbeSpecularTrace from 1.74–1.87 to 0.81 ms, and the frame from 12.4–13.2 to 9.35–9.43 ms.

Per-pass counters over each repeat's 30 frames; ranges cover the three repeats, except that current ProbeSpecularTrace excludes repeat 0 (see below). SIMT efficiency is the share of active threads per executed instruction. Texture quads count 3D texture requests: atlas samples for the field, occupancy samples for the bricks.

| Pass, build | SM / L1TEX / L2 throughput | Warps active | SIMT efficiency | L1TEX long scoreboard | 3D texture quads per frame | L1 texture hit / latency |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| ProbeTrace, baseline | 58 / 6 / 13% | 31% | 18% | 24–25% | 0.75 M | 10–12% / 452–455 cycles |
| ProbeTrace, current | 42–43 / 8–11 / 14–19% | 30–32% | 21% | 38–39% | 8.2 M | 61–64% / 274–283 cycles |
| ProbeSpecularTrace, baseline | 64–67 / 6–7 / 13–16% | 32–38% | 16% | 18–19% | 0.38–0.39 M | 10–16% / 675–975 cycles |
| ProbeSpecularTrace, current | 49 / 8 / 14% | 33% | 20% | 35% | 4.9 M | 69% / 264–265 cycles |

Findings:

1. **Dependent march steps, not bandwidth.** The field traces issue 11–13 times the bricks' 3D texture requests. Each request hits L1 more often and returns sooner, but each depends on the previous step's distance, so the L1TEX long-scoreboard share rises from 18–25% to 35–39% and SM throughput falls from 58–67% to 42–49%. L1TEX and L2 throughput stay below 20%. Fewer steps per ray (larger safe steps from coarser hierarchy levels, earlier termination for visibility-only rays) is the lever; faster individual fetches are not.
2. **Divergence is shared with the baseline.** SIMT efficiency is 20–21% for the field and 16–18% for the bricks: on average about six of 32 lanes execute. Per-thread marching of a probe's 64 directions diverges in either representation, so this is not a regression. It is the largest inefficiency of both, and the candidates are ray binning by direction or length, or splitting the march from hit shading so shading runs coherent.
3. **Occupancy is unchanged.** Both builds keep 30–38% of compute warps active with 65–92% of the register file allocated; the traces stay register-limited.
4. **Driver compilation varies between Nsight launches.** Identical SPIR-V received different register allocations. Current ProbeSpecularTrace in repeat 0 allocated 65% of registers and took 1.172 ms, against 73% and 0.809 ms in repeats 1 and 2. ProbeTrace in repeat 1 allocated 72% instead of 90% and showed local-memory traffic, at nearly the same time (1.570 against 1.553–1.556 ms); the morning repeats show the same pattern. The SPIR-V declares no function-scope arrays, so that local traffic is register spilling by the driver compiler, not a shader array. The native repeats above agree within 4 µs, so the variance appears specific to Nsight's recompilation. Use medians of repeats, never a single Nsight repeat.
5. **Source-level samples need the GUI.** [current-shaderprofile](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/current-shaderprofile) was traced with `--real-time-shader-profiler` (`tools/profile_hybrid_gi.py --shader-profiler "Blackwell GB20x"`). `--auto-export` writes only the pass tables, so its source-level hot spots must be read in Nsight Graphics.

**Camera motion and dynamic geometry.** One 60-frame trace each, same setup, `--workload camera|dynamic` ([camera](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/current-camera-trace/summary.json), [dynamic](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-final/current-dynamic-trace/summary.json)). Nsight rejects `--max-duration-ms` of 10,000 or more, so these use 9,000.

- Camera motion: frame median / p95 10.404 / 11.057 ms. ProbeTrace has a median of 2.005 ms and a maximum of 2.814 ms; ProbeSpecularTrace 0.811 / 1.498 ms; ReadProbeDemand 0.490 ms (0.146 ms still). No field pass runs. The trace counters match the still camera (SIMT efficiency about 20%, L1TEX long scoreboard 35–39%).
- Dynamic geometry: frame median / p95 45.987 / 46.580 ms. The field is not the cost. `FieldAdvance` has a median of 0.063 ms and a maximum of 0.459 ms; `FieldPrepare` 0.026 / 0.263 ms. The frame is dominated by the radiance-cache voxelizer: `VoxelizationComp` re-voxelizes all 3.75 M triangles every frame in 58 dispatches, 32.9 ms. Shadow-map re-rendering adds 13.9 ms (29 `SceneShadowFace_*` cube faces), and `VoxelInjectRadiance` 2.2 ms. `ComputeVoxelizer.cpp` is unchanged since the baseline, so this cost is not specific to the field. Each dynamic field generation publishes after about 3.0 s and 62 graphs, so at this frame rate the moving volume's field lags about 65 frames. The per-frame field work is far below the 2 ms construction budget, so a larger per-frame share would shorten that lag. This trace's counter export contains no values.

Remaining Nsight gaps: complete cold-preparation, coverage-expansion, warm-switch and scene-switch transitions; old Sponza; a baseline dynamic workload (milestone 0 recorded none); and source-level hot spots from the shader-profiler trace.

The ordered optimization work that follows from these findings is in [the optimization plan](BrixelizerGIOptimizationPlan.md).

## New Sponza camera approach and the construction fault, 2026-10-29

Reported: walking from the outside of New Sponza toward its center halves the frame rate with both Compute GI and HW RT. Measured with the demo's `camera_pose_eye` / `camera_pose_target` keys, which override scene cameras, on the field-matched configuration at 1600×900. Stops lie on PhysCamera001's view line from beyond it (−2, −1, a close wall) through PhysCamera001 (0) to the hall center (1). Driver and reports: [approach.py and table.py](E:/Dev/ZenEngine/build/compute-gi/approach).

| GPU frame median, ms | Wall (−2) | Hall end (0) | Hall center (1) |
| --- | ---: | ---: | ---: |
| Compute, before | 2.12 | 7.64 | 6.83 |
| Compute, after | | 7.27 | 6.42 |
| HW RT, before | 1.48 | 5.93 | 5.70 |
| HW RT, after | | 5.17 | 4.82 |

- **Cause.** The drop is the hall view's per-pixel workload, not a GI regression. Within the hall the frame rate stays flat. From the wall to the hall, every per-pixel pass grows: direct lighting with 22 unbounded, shadowed lamps (HW RT 2.7–3.0 ms of shadow rays), Compute's probe traces (about 2.4 ms against HW RT's 0.5 ms), the dirt decal layer (up to 1.5 ms) and the G-buffer.
- **HW RT lamp shadow rays now start at the light** (`scene_shadows.glsl`). Segment visibility is symmetric, and 98% of New Sponza's lamp rays end at the lantern glass beside the lamp. A receiver-side any-hit ray first crosses the hall's BVH; a light-side ray meets the glass at once. `SceneLighting` drops 0.7–0.8 ms. Against the previous build's capture, 3 of 1.44 M pixels change, by at most 6 of 255; two captures of the new build are identical. A 1e-5 end margin, tried first, leaked light onto 3 lantern-trim pixels, where New Sponza's surfaces lie within 0.25 mm of each other. The final ray excludes only one ulp at the receiver end, behind the existing origin offset.
- **Compute decals reuse the receiver's exact shadow-map results** (`hybrid_gi_visibility.frag`, `deferred_lighting.glsl`, `forward_material.glsl`). The receiver visibility bits that HW RT decals already used now mark a light exact when its shadow visibility is 0 or 1; layers recompute penumbra lights themselves. Ray results are always exact, so HW RT is unchanged. Lighting captures keep evaluating every light in the layer. `ForwardTranslucent` drops about 0.4 ms. The image changes by at most 6 of 255 in 26–63 pixels, none above 8: the decal's offset from its receiver at shadow-map edges, which the HW RT reuse already accepts.
- **Measurement hazard.** With the editor open, the 16 GB adapter held about 15.2 GB during these runs (desktop compositor about 3 GB; the editor 5.4 GB dedicated plus 4.4 GB shared). The G-buffer pass then varied from run to run (1.0 and 2.7 ms native, 12.6 ms under Nsight) from residency changes. New Sponza's 72 textures at 4096² are uploaded uncompressed (5.25 GB). Close the editor before timing, or treat G-buffer spikes as residency noise.

**Construction fault (fixed).** Short runs with async compute off lost the device about 0.35 s after the scene field published: `nvlddmkm` Xid 13, `Graphics Exception: SKEDCHECK36_DEPENDENCE_COUNTER_UNDERFLOW`, then `VK_ERROR_DEVICE_LOST`. It happened in both backends, because HW RT keeps the field current. The repro (`build/compute-gi/fault/repro.py`) lost the device in 7 of 8 runs. Synchronization validation reported no hazard over the whole build. Only the GPU's current phase has work, so most construction slices dispatched empty indirect grids between barriers on the graphics queue. The scheduler now launches one group for an idle slice, which exits on `ZEN_FIELD_CONTROL_WORK_IDLE`. The result is 0 of 19 runs lost (Compute, HW RT, async on), with an unchanged field (60,419 bricks, 0 unavailable cells, the same lamp volumes) and unchanged GPU time across the build (1,541 against 1,554 ms over the warmup frames).

Validation (Release, all three changes): RenderCoreTest 728, RayQueryIntegrationTest 92, ConeVoxelGIIntegrationTest 112, EditorRenderingTest 59, SceneModelSwitchTest 8 and EditorPreviewTest 16 pass, with no validation messages ([logs](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/hall-perf-release)). The CUDA bounce gates were not rerun. Lighting captures keep per-layer shadow evaluation, the field builds the same bricks, and HW RT shadow rays are not part of the Compute gates.

The first launch after a shader change recompiled the ten field programs (570 ms in the first construction frame) on every run, because this machine's NVIDIA shader disk cache (4.9 GB) stored nothing new. The engine's `VkPipelineCache` is not persisted, so construction readiness here depends on that driver cache.

### Lamp shadow work

With the editor closed, an upper bound came from a temporary shader that treats every point and spot light as fully occluded without shadow work. It saves 1.7–2.1 ms in the hall: Compute 7.26 → 5.36 ms and HW RT 5.15 → 3.12 ms at the hall end. Lighting the 22 lamps without shadows costs only 0.2 ms (the GI-off frame is 1.0–1.2 ms). Two changes now remove most of that work:

- **Compute: exact shadow-map early-out** (`scene_shadows.glsl`, `scene_shadow_max_depth.comp`). Each rendered shadow face is reduced to the deepest depth of every 4×4 texel block. Every tap's plane-corrected reference is a distance to a point of the receiver plane, so none is nearer than the plane. Away from grazing incidence, where every tap ray meets the plane ahead of the light, a plane beyond the deepest depth under the taps means every tap is shadowed and the filter is exactly 0. Four fetches replace sixteen filtered taps. A diagnostic build that still ran the filter and amplified any disagreement found none in either view. The remaining image differences, at most 6 of 255, come from GI timing: lamp volumes publish at different frames when frames are faster. The shadow memory estimate includes the reduction, 1/16 of the maps.
- **HW RT: lamp fixture masks** (`LightFixtureMasks`, `light_fixture_mask_build.comp`, `light_fixture_mask_dilate.comp`). Each shadowed point or spot light gets a 6×64×64 cube. A texel stores the farthest closest hit of 16 rays traced from the light within a shell of 2.5% of the scene extent, or "open" when any ray escapes. It then takes the maximum over its 3×3 neighbourhood across faces, so openings stay traced at their borders. A receiver beyond that distance in that direction is occluded without a ray. Masks rebuild only when a light moves, becomes shadowed, or a shadow caster changes within its shell. New Sponza's 22 masks build once in 0.92 ms. Captures at the hall end and center are bit-identical to the previous build. A gap narrower than the 16 sampled rays per texel in the middle of a blocked region would be missed; New Sponza has none that changes a pixel.

| GPU frame median, ms (FPS) | Wall (−2) | Hall end (0) | Middle (0.5) | Center (1) |
| --- | ---: | ---: | ---: | ---: |
| Compute, before both | 2.13 (360) | 7.26 (137) | | 6.44 (155) |
| Compute, after | 2.08 (357) | 5.96 (168) | 5.69 (176) | 5.21 (193) |
| HW RT, before both | 1.51 (202) | 5.15 (157) | | 4.81 (165) |
| HW RT, after | 1.47 (199) | 3.86 (191) | 3.56 (190) | 3.23 (192) |

Validation (Release): RenderCoreTest 728, including the shadow-cache tests, which now also check one reduction pass per frame that renders faces. RayQueryIntegrationTest 92, ConeVoxelGIIntegrationTest 112, EditorRenderingTest 59, SceneModelSwitchTest 8, EditorPreviewTest 16, EditorModelTest 47 and VulkanRHIIntegrationTest 416 pass, with no validation messages or new render-graph warnings ([logs](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/fixture-masks-release3)). Fixture-mask invalidation under moving geometry has no dedicated test yet.

`SceneLighting` is now about 0.9 ms on both backends, against the 0.5 ms bound. HW RT frame rate is flat from the wall to the center (190–199 FPS): its GPU frame stays below the CPU frame interval. Compute's remaining drop is mostly its GI probe traces (about 2.2 ms in the hall against 0.4 ms at the wall).

## Historical checkpoint — superseded

The remainder records an earlier implementation and an unmatched direct-shadow baseline. It remains useful for explaining the optimization investigation, but its 47% whole-frame and 89% GI comparisons are not current acceptance results.

## Workload and setup

New Sponza (`NewSponza_Main_glTF_003.gltf`) at 1600×900 on the RTX 5080, driver 621.01, Release, three frames in flight, threaded RHI, available async compute, VSync off, validation off, fixed animation step, mode 3. Compute GI is forced with HW ray queries disabled. Both builds use probe gather (spacing 16), the high preset, a 128-voxel radiance cache, shadow maps and the scene's 23 imported lamps. Each run starts from the milestone 0 run's own `engine.cfg`. For the current build, `voxel_gi_compute_traversal=bricks` and `voxel_gi_brick_resolution=128` are replaced by `voxel_gi_field_resolution=64`, so the field has 64 cells and 23 three-voxel lamp volumes. The baseline executable embeds this checkout's `Data/SpvShaders` path, so its replays ran with the archived SPIR-V swapped in and the original folder restored afterwards, verified by hash ([swap record](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/shader-swap.json)).

The plan's Compute baseline casts per-pixel software-BVH shadow rays from the deferred lighting pass. The integrated renderer has no software BVH, so its Compute GI renders shadow maps once for these static lights and reuses them. Whole-frame comparisons therefore include that change; GI comparisons use the GI passes alone.

## Uninstrumented matched runs

[profile_matched.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/profile_matched.py) runs `tools/benchmark_voxel_gi.py --profile` three times: 100 warm-up and 300 measured frames, with frames rendered while the field prepares or refines counted as warm-up. [Summary](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/perf/field-matched.summary.json); per-run native profiles are in the sibling `perf/field-matched-<n>/` folders. The baseline reruns use the archived milestone 0 executable and SPIR-V on the same day ([run 0](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/baseline-run-0/summary.json), with sibling `baseline-run-1` and `-2`); the archived run is its original milestone 0 measurement.

| Measurement | Current, three runs | Baseline reruns, three runs | Archived baseline |
| --- | ---: | ---: | ---: |
| GPU frame median | 10.063 / 9.826 / 10.198 ms | 19.242 / 19.063 / 19.054 ms | 18.658 ms |
| GPU frame p95 | 10.433 / 10.226 / 10.523 ms | 20.488 / 20.363 / 19.785 ms | 20.063 ms |
| GI pass group (`Probe*`, `Voxel*`, `Hybrid*`, `Field*` medians summed) | 6.125 / 5.967 / 6.129 ms | 3.253 / 3.253 / 3.255 ms | 3.243 ms |
| ProbeTrace | 3.215 / 3.218 / 3.216 ms | 1.374 / 1.376 / 1.380 ms | 1.103 ms |
| ProbeSpecularTrace | 1.493 / 1.332 / 1.496 ms | 0.421 / 0.420 / 0.420 ms | 0.694 ms |
| ProbeRetrace | — | 0.149 ms | 0.145 ms |
| ProbeTemporal | 0.149 ms | 0.001 ms | 0.001 ms |
| SceneLighting | 1.630 / 1.623 / 1.809 ms | 10.799 / 10.645 / 10.637 ms | 10.488 ms |
| ForwardTranslucent | 1.467 / 1.391 / 1.394 ms | 4.363 / 4.351 / 4.359 ms | 4.191 ms |
| VMA peak device-local | 9,128,782,352 B | 8,823,050,752 B | 8,823,050,752 B |

The GI pass group is a comparison aid, not a share of frame time: passes can overlap on a queue, and attribution moves between ProbeTrace and ProbeSpecularTrace across baseline runs, while their group sum stays at 3.25 ms. The other probe passes match within a few microseconds, so the difference is the field traces: **6.13 ms against 3.25 ms, +89%, where the plan's target is within 10%.** The whole frame is 47% faster because SceneLighting no longer casts software-BVH shadow rays and ForwardTranslucent no longer traces them either. ForwardBackground reads 1.5–1.7 ms against 0.78 ms because it overlaps the probe trace on the async queue.

## Cold preparation

Launch to published scene field takes 26.8–27.1 s, of which the field's GPU build is 22.6 s over 3,743 graphs. The 23 lamp volumes then publish 14.2 s later. The demo renders PBR with shadow maps until the scene field publishes, then Compute GI on the scene field while the volumes build. Of 5,161–5,163 frames with field work, the field passes per frame have a median of 0.075 ms, a p95 of 0.58–0.60 ms and a maximum of 2.87–3.46 ms. The maxima are `FieldPrepare` plus `FieldAdvance` frames, which run on async compute beside rendering, so per-pass times can include overlapping graphics work. The 2 ms construction target needs an isolated check of those frames.

## Nsight GPU Trace, still camera

Nsight Graphics 2026.3.1 GPU Trace, clocks locked to base, API-timestamp timing (hardware events off), GPU markers on, validation off. Each repeat starts 75 s after launch, once the field and every lamp volume are ready (about 41 s in the uninstrumented runs), and records 30 consecutive frames. [tools/profile_hybrid_gi.py](E:/Dev/ZenEngine/tools/profile_hybrid_gi.py) launches each repeat through [m7_profile.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/m7_profile.py). Every folder under [2026-10-28-still](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still) keeps the `.ngfx-gputrace`, the `BASE/` exports, `command.json`, `engine.cfg`, `inputs.json` (executable, Nsight and SPIR-V hashes), `launch.log`, `summary.json` and `metrics.json`. Every repeat exported 30 frames with every pass recorded in every frame.

| Repeat | Build | GPU frame median / p95 / max | ProbeTrace | ProbeSpecularTrace | SceneLighting | ForwardTranslucent |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| [current 0](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/current-trace-0/summary.json) | current | 13.203 / 13.622 / 14.159 ms | 4.495 ms | 1.744 ms | 2.088 ms | 1.828 ms |
| [current 1](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/current-trace-1/summary.json) | current | 12.441 / 12.902 / 13.564 ms | 3.809 ms | 1.865 ms | 2.086 ms | 1.810 ms |
| [current 2](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/current-trace-2/summary.json) | current | 12.459 / 12.748 / 12.818 ms | 3.762 ms | 1.741 ms | 2.098 ms | 1.810 ms |
| [baseline 0](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/baseline-trace-0/summary.json) | baseline | 24.251 / 24.956 / 25.250 ms | 1.472 ms | 0.482 ms | 13.481 ms | 5.429 ms |
| [baseline 1](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/baseline-trace-1/summary.json) | baseline | 24.320 / 24.636 / 24.780 ms | 1.431 ms | 0.490 ms | 13.637 ms | 5.379 ms |
| [baseline 2](E:/Dev/ZenEngine/build/compute-gi/brixelizer-nsight/2026-10-28-still/baseline-trace-2/summary.json) | baseline | 24.738 / 25.969 / 26.737 ms | 1.666 ms | 0.482 ms | 13.699 ms | 5.685 ms |

Base clocks lengthen every pass relative to the uninstrumented runs, so these times are compared only with each other. The first current repeat ran ProbeTrace at a higher register allocation with fewer active warps than the other two; the ranges below include all three repeats.

Per-pass hardware metrics, averaged over each repeat's 30 frames ([nsight_metrics.py](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/nsight_metrics.py)). Throughputs are percentages of peak sustained; stalls are shares of the pass's PC samples.

| Pass, build | SM / L1TEX / L2 throughput | Compute warps active | Registers allocated | Largest stall reasons |
| --- | ---: | ---: | ---: | --- |
| ProbeSpecularTrace, baseline 0 | 69 / 7 / 16% | 39% | 74% | not selected 24%, wait 20%, long scoreboard L1TEX 18% |
| ProbeSpecularTrace, baseline 1 | 78 / 8 / 17% | 44% | 82% | not selected 24%, wait 21%, long scoreboard L1TEX 18% |
| ProbeSpecularTrace, baseline 2 | 74 / 8 / 17% | 41% | 80% | not selected 24%, wait 21%, long scoreboard L1TEX 18% |
| ProbeSpecularTrace, current 0 | 58 / 8 / 13% | 38% | 72% | long scoreboard L1TEX 29%, wait 27%, selected 13% |
| ProbeSpecularTrace, current 1 | 57 / 8 / 13% | 38% | 72% | long scoreboard L1TEX 28%, wait 27%, selected 13% |
| ProbeSpecularTrace, current 2 | 58 / 8 / 13% | 39% | 73% | long scoreboard L1TEX 29%, wait 27%, selected 13% |
| ProbeTrace, baseline 0 | 50 / 6 / 11% | 27% | 79% | long scoreboard L1TEX 25%, wait 22%, selected 16% |
| ProbeTrace, baseline 1 | 49 / 5 / 11% | 26% | 78% | long scoreboard L1TEX 24%, wait 23%, selected 16% |
| ProbeTrace, baseline 2 | 42 / 5 / 8% | 20% | 71% | wait 26%, long scoreboard L1TEX 24%, selected 17% |
| ProbeTrace, current 0 | 32 / 5 / 10% | 22% | 86% | long scoreboard L1TEX 36%, wait 26%, selected 12% |
| ProbeTrace, current 1 | 40 / 7 / 14% | 30% | 68% | long scoreboard L1TEX 34%, wait 25%, selected 11% |
| ProbeTrace, current 2 | 40 / 7 / 14% | 31% | 69% | long scoreboard L1TEX 34%, wait 25%, selected 11% |

At base clocks the current build's frames take 12.4–13.2 ms against the baseline's 24.3–24.7 ms. ProbeTrace takes 3.8–4.5 ms against 1.4–1.7 ms, and ProbeSpecularTrace 1.7–1.9 ms against 0.48 ms. Against the bricks, the field traces run at lower SM throughput with a larger share of L1TEX long-scoreboard stalls (28–36% against 18–25%): dependent memory latency per march step dominates, not arithmetic. Neither build is bandwidth-bound; L1TEX and L2 throughput stay below 18%.

## Findings and prioritized opportunities

The GI passes take 2.9 ms more than the baseline's. A one-run attribution with the lamp volumes disabled ([summary](E:/Dev/ZenEngine/build/compute-gi/brixelizer-integration/perf/field-novolumes.summary.json); same workload, otherwise unchanged build) splits that increase:

| Pass | Baseline (bricks) | Scene field only | Field with 23 lamp volumes |
| --- | ---: | ---: | ---: |
| ProbeTrace | 1.103 ms | 2.469 ms | 3.216 ms |
| ProbeSpecularTrace | 0.694 ms | 0.705 ms | 1.493 ms |
| GPU frame median | 18.658 ms | 8.497 ms | 10.063 ms |

The scene field's specular trace costs the same as the bricks, and its diffuse probe trace costs 1.37 ms more. The lamp volumes add 1.57 ms per frame, split evenly between the two traces. The remaining probe passes cancel: ProbeTemporal (+0.15 ms) replaces the deleted ProbeRetrace (0.15 ms), and ProbeFilter is 0.03 ms shorter.

Prioritized opportunities, from the measurements above:

1. **Local volumes in the composite trace (1.57 ms measured).** Two costs make up this figure, and their split is not measured. Every march segment tests each published volume's authority box: up to 23 box spans per segment for every ray in the scene, including rays nowhere near a lamp. Rays inside a volume also march its finer bricks. A coarse occupancy mask of the volumes over the scene field's cells, or a rejection test against their union, would remove most box tests; the goal is the scene-field-only cost away from lamps.
2. **Register-limited occupancy in ProbeTrace.** The pass allocates 68–86% of the register file while keeping only 22–31% of peak compute warps active, much like the brick trace (71–79%, 20–27%). The field's march, however, spends more of its samples waiting on L1TEX long scoreboard (34–36% against 24–25%), with fixed-latency waits at 25–26%: a dependent-fetch loop without enough warps to hide its latency. Reducing live state (the per-level descriptor carried through the loop, the retry path), or splitting tracing from hit shading, should raise occupancy, and this helps the field more than it helped the bricks. ProbeSpecularTrace runs at 38–39% active warps with the same stall mix.
3. **Fetch chain per march step (about 1.4 ms in the diffuse trace).** Each step reads the hierarchy, then the cell map, then the atlas. Reusing the current brick across consecutive steps and skipping empty space from coarser hierarchy levels would shorten the chain.
4. **Worst construction frame.** Isolate `FieldPrepare`/`FieldAdvance` on async compute (milestone 2 headless maximum: 1.39 ms). If in-render frames really exceed 2 ms, scale the heavy-phase repetitions per frame down while Compute GI renders.

Measurements and hypotheses are kept apart: the throughput, warp and stall figures are measured; the fixes and their expected effect are not.

## Missing evidence

- Camera-motion, dynamic-geometry, coverage-expansion and warm-switch workloads were not traced; old Sponza was not compared. Warm switching is covered only by the editor suite's frame count (at most two submitted frames).
- Cold preparation was measured from uninstrumented native profiles, not from a trace of the whole transition.
- The baseline's construction and BVH preparation were not traced, so construction cost has no matched comparison.
- Hardware-event timing was not used, so queue-overlap attribution inside a frame rests on API timestamps.
- Shader Profiler source-level hot spots were not collected; the opportunities above come from pass-level counters and the attribution run.

## Milestone 2 construction measurements

The full New Sponza streaming checkpoint constructs 3,747,018 triangles into 60,419 bricks. Its configuration uses one global 64³ cell map, a 320³ R8 atlas, initial triangle quota 4,096, 1,024 references per cell, 256 cells per emission dispatch, and four heavy-phase repetitions. Hardware ray queries and validation are disabled; GPU markers are enabled; execution mode 0 uses inline RHI and the graphics queue. This is a headless builder, with no GI gather or material composition.

| Measurement | Result |
| --- | ---: |
| Completed construction graphs with GPU timestamps | 5,253 |
| GPU envelope median / p95 / maximum | 0.458 / 0.612 / 1.389 ms |
| Maximum individual pass | 0.796 ms |
| Persistent field allocation | 35,014,960 bytes |
| Peak field scratch, including snapshot | 109,601,648 bytes |
| Snapshot payload | 98,422,480 bytes |
| Capacity retries | 1,687 |
| Headless preparation loop, including CSV instrumentation | 20,108.671 ms |

All measured timestamps are available. Two completion-poll frames have no submitted GPU interval. The first envelope excludes one dedicated-transfer interval; pass CSVs retain transfer timing separately. Frame envelopes are not sums of pass ranges. Existing source/topology buffer retention peaks at 400,056,144 bytes and excludes retained textures, allocator pools and residency. [Summary and source hashes](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-stream-full-profile.summary.json), [XML](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-stream-full-profile.xml), [frame CSV](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-stream-full-profile-frames.csv), [pass CSV](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-stream-full-profile-passes.csv).

The retry count identifies a concrete construction inefficiency: the current scheduler doubles a reduced quota after two successful batches, repeatedly exceeding capacity in dense regions. Require sufficient measured job/reference headroom before increasing the quota, then repeat the same complete workload. The frame maximum is below 2 ms in this isolated run; interactive readiness, competing rendering work, dynamic updates, local detail and repeated-run behavior remain unqualified.

## Milestone 2 Nsight workflow diagnostics

Nsight Graphics 2026.3.1 on the RTX 5080, driver 621.01, captured the builder with base clocks, validation disabled, and GPU markers enabled. Both requests begin after 150 queue submissions and capture the next 100 submissions, with a 1,000 ms maximum interval. These are **not presented frames**. Sandbox launches could not establish the profiler attachment; the same bounded launch outside the sandbox connected and exported successfully. Each manifest records the exact command, executable/shader hashes, unchanged-input check and trace path.

| Capture | Timing mode | Exported capture interval | Marker evidence |
| --- | --- | ---: | --- |
| [Hardware-event trace](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-unsandboxed/RayQueryIntegrationTest_2026_10_27_10_47_23.ngfx-gputrace) | Hardware events, 10,000 KiB storage | 498.644 ms | 6,700 rows; 100 identical ordered 67-marker blocks |
| [API-timestamp trace](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-api-timestamps/RayQueryIntegrationTest_2026_10_27_10_57_09.ngfx-gputrace) | API timestamps | 507.894 ms | 6,700 rows; 100 identical ordered 67-marker blocks |

Each block contains 22 `FieldPrepare` and 22 `FieldAdvance` ranges, four repetitions each of Coverage/Count/Compact/Emit, and one each of BatchClear/Jobs/Scan/ScanSums/Allocate/Hierarchy/status readback. Inactive indirect phases still have markers, so marker count is not active-dispatch count. Exported interval time includes the headless submission gaps; dividing it by 100 does not produce a rendered-frame measurement. The captures are separate runs and have different executable hashes because the committed-range/fixture build occurred between them.

[Hardware-event manifest](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-unsandboxed/manifest.json), [export audit](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-unsandboxed/diagnostic-summary.json), [API-timestamp manifest](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-api-timestamps/manifest.json), [export audit](E:/Dev/ZenEngine/build/compute-gi/brixelizer-execution/m2-nsight-construction-api-timestamps/diagnostic-summary.json). Their sibling `BASE` folders retain the original marker and counter exports; logs preserve attachment and export results. The profiler terminates the test after capture, so these captures do not provide a successful full-construction test result.

The tabular exports establish marker completeness and ordering. They do not independently establish absence of hardware-event overflow, exact per-queue range boundaries, or correct attribution of each dispatch. Until those checks are made in the trace, the exported per-marker timings and counters are diagnostic data, not qualified occupancy or bottleneck findings. No pass ranges are summed into a frame time.
