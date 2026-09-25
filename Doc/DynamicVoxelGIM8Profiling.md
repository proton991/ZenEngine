# M8: Nsight Graphics profiling

2026-09-25. **In progress; work stopped before final acceptance.** Current progress and validation boundaries are recorded below. `auto` still selects cone tracing; prior M7 acceptance does not certify the latest M8 gather changes.

The historical sections beginning with "Capture conditions and reproduction" predate the [runtime material/bounds/budget fixes](DynamicVoxelGIGapVerification.md). Preserve those numbers and test counts as earlier evidence only. The following stop-point section records the later work on the corrected implementation.

The subsequent [key-3 performance investigation](VoxelGIPerformance.md) profiles the corrected **256³ cone** configuration and reproduces the reported Debug CPU starvation: 41.8 FPS / 31% GPU activity versus 150–157 FPS / 99% in the isolated optimized build. It includes a launcher, repeatable benchmark and a new cone GPU trace. This is a separate workload from the directional results below and does not close the remaining M8 gates.

## Current work at the stop point (2026-09-25)

Current artifacts: [`build/dynamic-voxel-m8-current-20260925/`](../build/dynamic-voxel-m8-current-20260925/). These runs use the RTX 5080 and MSVC release preset (RelWithDebInfo), with RT and validation disabled for performance measurements. Correctness runs enable synchronization validation. Each run retains its settings and reproduction metadata; compare like capture modes, warmup, scene and submission settings.

Implemented: two-stage receiver selection, 8-byte compact DDA hits alongside the 96-byte decoded reference, startup 32/64/128-ray selection, VMA allocation-peak tracking, traversal capture, deferred cached-position reconstruction, and a latest 64-thread-per-receiver gather. Compact records preserve cell/class identity and full floating-point distance bits. These changes are saved locally; the latest version has not completed M8 acceptance.

| Measurement | Earlier result | Later result / evidence |
| --- | --- | --- |
| Static Sponza 64 cubed, GPU median; 60-frame hardware-event traces | [9.185 ms](../build/dynamic-voxel-m8-current-20260925/baseline-sponza-64-comp/summary.json) | [6.800 ms after receiver selection](../build/dynamic-voxel-m8-current-20260925/selection-sponza-64-comp/summary.json); predates parallel gathering |
| Moving fixture, compute producer, GPU median / p95; 20-frame API-timestamp traces | [53.099 / 54.304 ms, serial gather](../build/dynamic-voxel-m8-current-20260925/motion-comp-api/summary.json) | [13.294 / 14.333 ms, parallel gather](../build/dynamic-voxel-m8-current-20260925/parallel-motion-comp/summary.json) |
| Static Sponza, unprofiled FPS / mean GPU activity | [168.8 FPS / 89.2%, intermediate compact version](../build/dynamic-voxel-m8-current-20260925/fps-compact-128/summary.json) | [153.2 FPS / 63.9%, latest parallel version](../build/dynamic-voxel-m8-current-20260925/fps-parallel-mailbox/summary.json); regression remains unresolved |
| Moving fixture, latest parallel version, unprofiled FPS / mean GPU activity | No matched earlier throughput result recorded here | [133.4 FPS / 88.4%](../build/dynamic-voxel-m8-current-20260925/fps-parallel-motion-mailbox/summary.json) |
| Static Sponza, peak device-local VMA commitment | [7.58 GiB decoded](../build/dynamic-voxel-m8-current-20260925/fps-decoded-128/summary.json) | [3.43 GiB compact](../build/dynamic-voxel-m8-current-20260925/fps-compact-128/summary.json), before parallel gathering |

GPU trace frame times are separate from unprofiled throughput, which uses CPU frame intervals including frame-slot backpressure. NVML GPU activity is not SM occupancy. The approximately 90% activity goal is not consistently met. Submission/presentation effects are still hypotheses for the latest static regression. VMA diagnostics track peak committed allocator blocks, including retained pools/resources, but exclude driver/private/swapchain allocations and do not establish total physical residency.

**Validation boundary:** the intermediate compact version passed [136 native tests](../build/dynamic-voxel-m8-current-20260925/native-compact.json), [56 quality profiles plus four stationary summaries](../build/dynamic-voxel-m8-current-20260925/quality-compact-128/results.json), and [68/68 byte-identical static-face capture comparisons](../build/dynamic-voxel-m8-current-20260925/compact-image-equivalence.json). The latest parallel version has passed [16 focused native tests](../build/dynamic-voxel-m8-current-20260925/native-parallel.json) and [484 RenderCore tests, seven disabled](../build/dynamic-voxel-m8-current-20260925/rendercore-parallel.json). Full native, quality and stationary validation of that latest version is pending. Do not transfer earlier full-suite acceptance to it.

**Sample presets:** 128 remains the default. Before parallel gathering, [64 rays failed 20 quality profiles at 128 cubed](../build/dynamic-voxel-m8-current-20260925/quality-compact-64/results.json); the [partial 32-ray evaluation failed 11 of 14 recorded cases](../build/dynamic-voxel-m8-current-20260925/quality-compact-32/results.json). Lower counts are supported startup controls, not accepted general quality presets. Cache format is also startup-only.

Cold compact/decoded captures, animated-light and compute/geometry-motion profiles, 128-cubed and cone profiles, and traversal samples exist in the artifact root, but most predate parallel gathering. Deterministic receiver sorting was subsequently added to traversal capture and has not been recaptured. Cold hardware-event traces overflowed their event buffer; API-timestamp captures are available, but changing/absent pass ranges in exports can repeat or accumulate and must not be summed as initialization cost. Final cold readiness, matrix, memory and traversal reporting remain open.

The authoritative [unfinished-work checklist in the plan](DynamicVoxelGIImplementationPlan.md#m8-unfinished-work-at-the-stop-point-2026-09-25) covers the static regression, final profiling matrix, unrun workgroup/scratch/tiled-filter experiments, preset decisions, final validation and automatic-selection decision. A tiled-filter candidate exists only in experiment artifacts; active-receiver scratch compaction is unimplemented. Keep `auto` on cone until the documented promotion decision; hardware queries remain deferred to H0-H2.

## Capture conditions and reproduction

Measurements use Nsight Graphics **2026.3.1 GPU Trace Profiler**, RTX 5080, driver 616.92, Windows 11, and the MSVC RelWithDebInfo build. Nsight locks GPU clocks to base and disables VSync. Each steady-state capture contains 60 frames after 40 warmup frames. These are GPU frame times exported by Nsight, not the demo's CPU submission times.

The measured scenes use compute voxelization, a 64³ grid, averaged reflectance, 128 rays per face, five fixed point lights with shadows, environment and emission enabled, and a fixed camera/geometry. Full filtering means elapsed-time temporal filtering plus spatial filtering. Sponza has a 6144 MiB GI budget; the small fixture has 3072 MiB. Geometry motion and animated-light costs are not represented by these steady-state measurements. Output is 1920×1080; G-buffer extent is listed separately.

Build with the release preset and isolate its executable from the validation build:

```powershell
cmake --preset x64-windows-msvc-release -DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO=E:/Dev/ZenEngine/build/dynamic-voxel-m8/release-bin
cmake --build build/x64-windows-msvc-release --target scene_renderer_demo -j 6
python tools/profile_dynamic_voxel_gi.py --config build/dynamic-voxel-m7/scenes/sponza-comp-filtered.cfg --output build/dynamic-voxel-m8/new-sponza-run
```

Run from the repository root in a configured MSVC developer shell. The runner defaults to the normal 2048² G-buffer, saves the exact configuration/command and executable/SPIR-V hashes, launches from `bin`, and restores `Data/engine.cfg` byte for byte. Use a new output directory each time. Do not run another config-owning test or GPU workload concurrently. `--gbuffer 256` reproduces the diagnostic preset; `--set voxel_gi_method=cone` selects the comparison method. `--thread` and `--async-compute` control submission modes; `--cold` starts tracing at the first queue submission.

The demo's new `--gpu-markers` flag records RDG pass names through RHI command lists to Vulkan debug labels. Markers include each pass's prologue barriers and are off by default. `--disable-validation` is used only for performance captures; ordinary startup still enables validation. The runner also disables RT. No timestamp-query framework or RT support was added. Native correctness tests enable the labels with synchronization validation active.

Each run retains its `.ngfx-gputrace`, `BASE/FRAME.xls`, `BASE/D3DPERF_EVENTS.xls`, reproduction information, launch log and `summary.json`. The `.xls` exports are TSV. Repeated pass names represent individual faces; the summarizer adds those rows per frame before computing pass statistics. Pass ranges can overlap with async compute and must not be added to infer total frame time. See NVIDIA's [GPU Trace documentation](https://docs.nvidia.com/nsight-graphics/UserGuide/gpu-trace-overview.html).

## Steady-state results

Times are milliseconds. Each row links to the exported measurements under `build/dynamic-voxel-m8`.

| Scene / method | G-buffer | Before median / p95 | Current median / p95 |
| --- | --- | --- | --- |
| Sponza, directional, full filtering | 256² | [9.189 / 9.665](../build/dynamic-voxel-m8/baseline-sponza-full/summary.json) | [6.124 / 6.382](../build/dynamic-voxel-m8/selection-sponza-full/summary.json) |
| Sponza, directional, filters off | 256² | [6.899 / 7.536](../build/dynamic-voxel-m8/baseline-sponza-raw/summary.json) | [5.416 / 5.678](../build/dynamic-voxel-m8/selection-sponza-raw/summary.json) |
| Fixture, directional, full filtering | 256² | [4.437 / 4.705](../build/dynamic-voxel-m8/baseline-fixture-full/summary.json) | [4.429 / 4.725](../build/dynamic-voxel-m8/selection-fixture-full/summary.json) |
| Sponza, cone | 256² | [2.368 / 2.633](../build/dynamic-voxel-m8/baseline-sponza-cone/summary.json) | Unaffected by receiver selection |
| Fixture, cone | 256² | [0.687 / 1.134](../build/dynamic-voxel-m8/baseline-fixture-cone/summary.json) | Unaffected by receiver selection |
| Sponza, directional, full filtering | 2048² | Not captured | [10.115 / 10.671](../build/dynamic-voxel-m8/normal-sponza-full/summary.json) |
| Sponza, cone | 2048² | Not captured | [2.939 / 3.236](../build/dynamic-voxel-m8/normal-sponza-cone/summary.json) |

The first optimization removes repeated receiver-selection work when output pixels reuse the same G-buffer texel. Each axis selects `min(viewport, source)` samples, preserving the exact original texel set for downscaling, upscaling, mixed axes and noninteger ratios. All later receiver, query, energy and filtering rules are unchanged.

For filtered Sponza at 256², median frame time falls **33.4%**. Mean `GISelectStaticReceivers` time falls from **3.413 to 0.410 ms**. The small fixture has no material total-frame improvement. At 2048² both G-buffer axes exceed the output axes, so this optimization removes no texel duplication; the normal-size results are measurements of the current implementation, not a claimed before/after improvement.

At 2048² the largest mean pass ranges are receiver selection **3.227 ms**, six static gathers **2.936 ms**, offscreen rendering **0.814 ms**, six padding passes **0.793 ms**, and scene lighting **0.742 ms**. Static gather remains a measured optimization candidate. This identifies expensive ranges; it does not establish a memory-bandwidth or occupancy bottleneck without further metric analysis.

The normal-size submission comparison isolates async compute with the same threaded CPU mode:

| RHI thread | Async compute | Median / p95 ms |
| --- | --- | --- |
| Off | Off | 10.115 / 10.671 |
| On | Off | [10.189 / 10.576](../build/dynamic-voxel-m8/normal-sponza-thread/summary.json) |
| On | On | [10.240 / 10.666](../build/dynamic-voxel-m8/normal-sponza-async/summary.json) |

This pair provides no evidence of a useful async-compute improvement for this fixed workload. It does not establish queue overlap or performance for moving geometry.

## Startup capture limitation

The [startup attempt](../build/dynamic-voxel-m8/cold-sponza-full/summary.json) requests `--start-after-submits 0` and retains a trace with 60 exported frames and cache-batch ranges. However, initial voxelization/cache-clear ranges are absent from the per-frame export. After cache batches finish, that export repeats their first timing values instead of representing their absence. Consequently, its pass averages cannot establish total initialization cost. Inspect the raw trace's pre-frame interval or use a targeted capture trigger before accepting cold-start data. **Cold initialization remains unverified**; the attempted trace is retained as diagnostic evidence, not an accepted cold-start benchmark.

## Memory and verification

Nsight reports **7885 MiB committed / 7871 MiB requested** for normal-size Sponza directional GI, versus **1223 MiB committed** for cone. At 256², directional Sponza reports **6978 MiB committed**, and the fixture **3394 MiB**. These are capture information readings, **not a verified lifetime peak**. They include resources outside the GI budget. The cache layout is unchanged: six 96-byte decoded-hit buffers, or 72 KiB per reserved receiver at 128 rays per face.

The affected debug/release targets build. **573 CPU tests pass**, with seven existing disabled tests, and **104 native GI tests pass** with RT disabled, GPU labels enabled and synchronization validation active across all four submission modes. The 64³/128³ grid and Sponza suites pass **26 scene captures**, including two 256³ rejection/cone cases. In the 24 directional cases, selected receiver sets and all raw/final static irradiance values match the M7 captures exactly; no tolerance was changed. The existing **35 static-scene captures** also pass, covering 2x/1.5x/odd extents, normal maps, mirrored geometry, fallback, energy and direct shadows. A separate 144-case axis-mapping check covers scale ratios and mixed-axis combinations. All 29 directional compute SPIR-V modules pass validation for Vulkan 1.1. Evidence: [native results](../build/dynamic-voxel-m8/native-selection.json), [scene comparison](../build/dynamic-voxel-m8/selection-scene-comparison.json), [static-scene results](../build/dynamic-voxel-m8/selection-static-scenes/results.json), [mapping check](../build/dynamic-voxel-m8/selection-mapping.json).

## Remaining M8 gates

These historical results do not close M8. Use the [current stop-point evidence](#current-work-at-the-stop-point-2026-09-25) and [unfinished-work checklist](DynamicVoxelGIImplementationPlan.md#m8-unfinished-work-at-the-stop-point-2026-09-25) for current status: compact-cache checks, VMA peaks and dynamic/traversal captures now exist, while the latest static regression, remaining experiments, final validation/reporting and promotion decision are still open. Hardware-query work remains deferred to H0-H2.
