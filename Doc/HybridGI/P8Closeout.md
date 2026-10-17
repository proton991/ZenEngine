# P8 close-out — 2026-10-17

**P8 is closed with documented shortfalls, by user decision of 2026-10-17.** The per-tier performance targets are not met. The plan permits closing P8 with the shortfall documented and resolved by an explicit preset decision, not a hidden quality reduction ([Definition of done](../HardwareRayQueryEnvironmentLightingPlan.md#definition-of-done), item 6). The user also stopped the final qualification run partway as too long. Everything below states whether it ran on the final build, comes from an earlier build, or was not done.

Final build: Windows/MSVC `x64-windows-msvc-performance`, RTX 5080, driver 617.42. `scene_renderer_demo.exe` SHA-256 `59e32b554b986feb6d8da03161b2273c6bff860681e56c8c5e8d3114d6c6c21f`. Evidence is under `build/p8-closeout/` (ignored, local): `closeout.json` records the executable and every SPIR-V hash, the stages, their commands and logs.

## Decisions

- **Performance shortfall accepted.** Neither tier meets the P0 GI budget, and Compute misses 60 FPS at 1080p (below). No preset hides it: defaults keep the qualified quality.
- **Compute keeps full-resolution tracing by default.** It passes every quality gate. Half-resolution diffuse stays opt-in (`voxel_gi_diffuse_resolution=half`, or `auto` for Compute only): 1.7–1.8× faster at 1080p, but it fails the frozen P0 limits on 5 of 10 shipping views ([measurements](NsightOptimizations.md#half-resolution-compute-diffuse-2026-10-16)).
- **Presets are unchanged.** `low` is 64³ with Compute and no reflections; `medium` is 64³ with the automatic provider; `high` and `ultra` are 128³ and 256³ and prefer HW RT. All use four adaptive diffuse rays, 32 history frames, temporal accumulation and the five-pass filter. Their time and memory were not measured per preset on the final build.
- **Automatic selection is capability-based** (user decision of 2026-10-15): `auto` resolves to HW RT exactly when the device reports usable ray queries and to Compute otherwise. Device and driver identity never select the tier.
- **Update policies stay as implemented.** Any geometry revision re-voxelizes the whole scene and, when a feature uses Compute, rebuilds the software BVH. Hardware acceleration structures keep their current build and update path. Partial voxel updates, generation amortization, AS refit/compaction, scratch pooling and async builds move to later candidates.
- **No GI memory budget or preflight is enforced.** The last whole-renderer measurement is in the [P8 measurements](P8.md#measurements) from an earlier build; a default budget is a later candidate.
- **Untested platforms stay unverified:** RX 7900 XT, MoltenVK/Apple Silicon and a GPU without ray queries. The AMD Radeon iGPU in the test machine was not measured for P8.

## Correctness on the final build

| Check | Result |
| --- | --- |
| Native tests (`build/p8-closeout/tests/`) | Pass: 583 RenderCore, 24 ConeVoxelGIIntegration, 16 RayQueryIntegration, 43 EditorRendering, 41 EditorModel, 21 ConfigLoader |
| SPIR-V validation of the hybrid, sky-cache, injection, shadow and counting modules | Pass |
| 22 fixtures and 20 shipping views, unchanged P0 limits (`static/report.json`) | Pass. Worst composed diffuse: HW RT abs. bias 0.279%, RMS 4.035%, P99 16.682%; Compute 0.274%, 4.180%, 17.216%; no unexpected zeros |
| HW RT motion and settling, Papermill top/hall (`motion/hardware/report.json`) | Pass. 16 gated settling cases, worst 0.253% / 3.354% / 12.304%; 8 continuous-motion cases, maximum reprojection 0.002872 px against 0.01 px, no identity mismatches |

The static shipping comparisons reuse the archived 65,536-ray references in `build/p8-nsight-optimizations/final-quality/`, validated against their archived executable and shaders.

**Not rerun on the final build** (stopped by user decision): Compute motion/settling, the 59 mixed-provider cases, the bounce and reflection gates, the settled-budget light-change probe and the CUDA ground-truth sweep. The latest passing results for these come from builds of 2026-10-15/16 and are recorded in [NsightOptimizations.md](NsightOptimizations.md#qualification-runner-updates): Compute motion/settling, all 59 mixed-provider cases, both providers' bounce and reflection gates, and HW RT on all 18 CUDA ground-truth views; Compute's CUDA sweep last passed on 2026-10-15 ([results](ComputeTriangleVisibility.md#cuda-ground-truth--2026-10-15)). Their evidence directories were deleted to free disk space on 2026-10-16. Since those runs, the default trace changed through the settled-budget [review fixes](NsightOptimizations.md#review-fixes-2026-10-16) and the half-resolution restructuring of the shared trace shader. The static and HW RT motion gates above cover the resulting default path; the budget's light-change probe never ran.

## Performance against the targets

RTX 5080 at 1920×1080, boost clocks, 64³, frozen Papermill top/hall views. Measured 2026-10-16 on the build before the ray counters below; the counters add only diagnostic shader variants and capture plumbing, so these timings were not repeated.

| Tier | Top | Hall | GPU frame median, top / hall |
| --- | ---: | ---: | ---: |
| HW RT | 87.13 FPS | 76.35 FPS | 11.21 / 12.83 ms |
| Compute | 23.92 FPS | 22.39 FPS | 38.14 / 40.87 ms |
| Compute, opt-in half resolution | 39.90 FPS | 40.59 FPS | 24.65 / 23.72 ms |

The same view rendered as PBR without GI takes 0.34 ms of GPU time (hall, final build), so nearly the whole frame is GI.

| P0 target (1080p, all GI passes) | Target | Measured, approximately | Status |
| --- | ---: | ---: | --- |
| HW RT, static | ≤ 3.0 ms | 10.9–12.5 ms | Missed |
| Compute, static | ≤ 4.0 ms | 37.8–40.5 ms | Missed |
| Overall frame | ≤ 16.7 ms | HW RT 11.2–12.8 ms; Compute 38.1–40.9 ms | HW RT met; Compute missed |
| HW RT faster than Compute | — | 3.4× (hall) to 3.6× (top) | Met |

Moving-light, rigid and deforming workloads, larger grids and the light-count matrix were not measured on this implementation; their earlier results in [P8.md](P8.md) predate software-triangle Compute visibility. The Compute shortfall comes from software BVH traversal, about 90% of the Compute frame. Experiments that did not close it are in [NsightOptimizations.md](NsightOptimizations.md), including the [Lumen-style near/far trace](NsightOptimizations.md#lumen-style-nearfar-compute-trace-2026-10-17).

## Issued rays

New diagnostic counting variants of the sky-cache, radiance-injection and receiver-shadow shaders complete the issued-ray coverage that P8 required (`gi_ray_totals.glsl`). They run only during a ray-count capture, which now reports them next to the hybrid trace's per-feature counts. `tools/capture_hybrid_ray_counts.py` captures one counted frame of a frozen view, optionally with the benchmark's light ring, a moving light, or an animated scene from `benchmark_hybrid_updates.py --prepare-only`.

Final build, hall view, 960×540, rays per pixel in the counted frame (`build/p8-closeout/ray-count-smoke*/`):

| Workload | Sky | Specular | Sky cache | Injection shadows | Deferred shadows |
| --- | ---: | ---: | ---: | ---: | ---: |
| Compute, 4 static lights | 5.660 | 0.986 | 0 | 0 | 1.843 |
| Compute, 4 lights, one moving | 5.694 | 0.986 | 0 | 0.060 | 1.834 |
| HW RT, 4 lights, one moving | 5.694 | 0.986 | 0 | 0.060 | 1.834 |
| Compute, rigid update, 1 light | 5.697 | 0.986 | 1.723 | 0.015 | 0.700 |

Bounce and reflections reuse the sky and specular rays and add none here. The sky cache and injection trace only on frames that change geometry, environment or lighting; the capture records whether the counted frame did. The legacy sky cache of scenes without voxel triangles marches voxel occupancy and is not counted.

## Later candidates

- Faster Compute traversal: a wide (4–8 child) BVH to cut the per-query root descent that dominates short and long rays alike.
- A signed-distance far field, if Compute ever needs Lumen-style tracing; binary occupancy failed by 27–39% bias.
- Lower internal resolution with a temporal upscaler for the Compute tier.
- A GI memory budget with preflight and per-preset reporting.
- Partial voxel updates, generation amortization, AS refit/compaction, scratch pooling and async builds, decided from moving-scene measurements.
- Final-build reruns of the gates listed above as not rerun, per-preset time and memory, and measurements on the untested platforms.
