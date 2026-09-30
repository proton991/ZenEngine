> Historical archive: implementation retired on 2026-09-30. Dynamic Voxel code, shaders, configuration and tools referenced below have been removed. Cone tracing is the supported GI path. See [retirement evaluation](DynamicVoxelGIM8Measurements.md#retirement-evaluation-2026-09-30).

# M8 final verification and promotion decision

2026-09-29. **M8.0-M8.9 complete for the stated AMD scope; DDA is not promoted.**
The acceptance scope is Stage A on AMD Radeon RX 7900 XT. The [final acceptance manifest](../build/dynamic-voxel-m8-final/acceptance.json) verifies the candidate, all required matrix cells, unique profile identities, quality limits, source snapshot and restored configuration.
The NVIDIA regression and RTX 5080 target remain open hardware/artifact gates. Hardware
triangle visibility (H0-H2) remains deferred.

## Candidate and implementation

The review's provider-boundary issue is fixed. The provider selects query-dependent
stage shaders, binds mesh/shadow inputs, advertises compact-cache support, and supplies
static visibility cache keys. The directional renderer retains the shared cache,
lighting, gathering and filtering sequence. A forwarding-provider test deliberately
uses a different backend identity while delegating DDA operations; four native modes
verify that the renderer follows the provider contract rather than its enum value.

Schema 3 adds bounded asynchronous GPU status/work snapshots with application-frame
identity, cache epochs, visibility revisions, occupied/selected counts, completed cache
work/readiness and effective frame-level fallback. Readback waits for normal resource
retirement; no additional in-frame GPU wait controls receiver selection. A native
four-mode assertion verifies epoch changes after rejected publication and stable provider
revision reporting. Per-pixel validity can still invoke composition fallback.

Profiling exposed timestamp-pool starvation after startup command buffers consumed the
old process-global budget. Pools now belong to the device and return to its bounded cache
only after completion or discarded recording. The limit is 256 pools of 512 scopes.
The long moving-fixture capture and native pool capacity/reuse checks pass.

`benchmark_dynamic_voxel_gi.py` owns configuration serially, restores exact bytes, records
binary/SPIR-V/config/glTF/external-resource/environment SHA-256 identities, and requires
complete fresh exports. Workgroup size is read from SPIR-V independently of ray count.
Cold initialization, instrumented pass/frame samples, uninstrumented CPU pacing and
intrusive traversal captures run separately. An external configuration edit is preserved.

The final application is [candidate-v2/scene_renderer_demo.exe](../build/dynamic-voxel-m8-final/candidate-v2/scene_renderer_demo.exe),
SHA-256 `af88c193b4249b806cded51ccfb9d2737702166cd02f90fe33452646721a87a8`.
Its [identity](../build/dynamic-voxel-m8-final/candidate-v2/identity.json) pins all 117 SPIR-V
modules. The previous frozen candidate differs only in the opt-in material-contract
harness: submitted-batch equality became cache-epoch equality. No renderer, provider,
profiler, RHI or shader changed. Native test binaries and completed M4-M7 runtime results
therefore exercise the same engine implementation. The final image and performance
runs use the exact final application hash.

## Measurement scope

GPU frame duration is the envelope of participating graphics/compute submissions,
including swapchain copy, and is never the sum of pass durations. Dedicated transfer-only
work is explicitly unsupported/excluded. CPU frame wall time includes frame-slot
backpressure and host pacing; it is not GPU activity or occupancy. All reported steady
DDA frames must have completed readiness data and zero fallback flags.

Initialization-to-ready is application wall time from the first DDA frame through the
end of the first frame whose GPU snapshot proves readiness. It excludes preceding
application startup, is not an exact GPU completion timestamp, and does not establish
temporal convergence. The delayed-start cold capture makes this boundary observable.

VMA lifetime peaks include retained allocator blocks and pooled resources, excluding
private driver/swapchain allocations; they do not measure physical residency. Logical
directional reserves are reported separately. The configured method cap also budgets
class/cone transition resources, and cannot be equated to VMA commitment.

## Baseline diagnosis (M8.4)

The [matched AMD baselines](../build/dynamic-voxel-m8-final/baselines-v2/) use 64 cubed,
128 rays, compact cache, compute voxelization, owner reflectance, radius 2, elapsed
history/spatial filtering, 320x180 presentation, 256-square G-buffer, 1024 shadows and a
6144 MiB cap. Each has three trials, 48 warmup and 80 measured frames, paired throughput
and separate 32-frame cold captures. Sponza is static at camera `(0.55, -0.15, 0)`; the lifecycle fixture moves geometry at camera `(0, 0, 2)`.
Values are medians of trial medians, GPU / uninstrumented CPU milliseconds.

| Workload | Method | Thread / async / VSync | GPU / CPU ms |
| --- | --- | --- | --- |
| Static | Cone | 1 / 1 / 0 | 0.347 / 1.028 |
| Static | DDA | 1 / 1 / 0 | 2.494 / 3.970 |
| Static | DDA | 0 / 1 / 0 | 2.200 / 3.651 |
| Static | DDA | 1 / 0 / 0 | 1.786 / 3.721 |
| Static | DDA | 1 / 1 / 1 | 7.582 / 6.043 |
| Motion | Cone | 1 / 1 / 0 | 1.688 / 3.724 |
| Motion | DDA | 1 / 1 / 0 | 12.157 / 11.469 |
| Motion | DDA | 0 / 1 / 0 | 12.122 / 11.429 |
| Motion | DDA | 1 / 0 / 0 | 11.273 / 11.618 |
| Motion | DDA | 1 / 1 / 1 | 12.195 / 11.540 |

Static work is sensitive to presentation and submission overhead; disabling async compute
reduces its GPU envelope without a proportional CPU improvement. Motion is dominated by
sender-environment and gather work: major same-pass occurrence sums are 5.799 ms dynamic
sender environment, 3.207 ms dynamic gather, 3.148 ms static gather and 3.144 ms static
sender environment. Those overlapping pass costs are not a frame total. The equivalent
static leaders are gather 0.572 ms, padding 0.438 ms, spatial filtering 0.143 ms and clear
0.129 ms. This supports a workload-specific diagnosis on AMD, not a universal queue switch.

Historical NVIDIA 168.8 -> 153.2 FPS and 89.2% -> 63.9% activity cannot be reproduced:
matched historical binaries/artifacts and the RTX 5080 are unavailable. The original
[historical report](DynamicVoxelGIM8Profiling.md) is preserved. AMD data neither closes
that regression nor satisfies its 1920x1080/16.7 ms target.

## Optimization experiments (M8.5)

All experiments held rays at 128. Each was built, checked with native GI tests and measured
in three static/moving trials before restoration. No experiment was adopted; the final
implementation retains 64-thread gathering, full scratch clear and the original filter.
[Decisions and raw artifacts](../build/dynamic-voxel-m8-final/experiments/decisions.json)
include rejected candidates and the active-clear prototype's initial recording failure.

| Candidate | Static GPU / CPU ms | Motion GPU / CPU ms | Decision |
| --- | --- | --- | --- |
| Gather 32 threads | 2.402 / 4.017 | 12.604 / 11.997 | Reject: moving regression, no static throughput gain |
| Gather 128 threads | 2.454 / 4.031 | 12.066 / 11.344 | Reject: small moving gain, slower static throughput |
| Tiled spatial filter | 2.424 / 3.970 | 12.085 / 11.446 | Reject: no useful throughput gain for added path |
| Active scratch clear | 2.534 / 4.181 | 12.058 / 11.477 | Reject: static regression, moving throughput unchanged |

The tiled candidate used a 4x4x4 group with a bounded 216-cell halo and a linear fallback
for nonaligned chunks. Active clearing used previous receiver lists before reset, with
full clears on first use/reset/rejected publication and no additional GPU allocation.
Boundary/filter/padding tests passed. Active clearing also passed empty/changing sets and
mocked indirect-dispatch limits. Experiment suites excluded only the separately accepted
planar case (144 passes, or 148 with active-clear-specific tests); final acceptance retains
that case and its raw failure.

## Sample presets (M8.6)

Only 128 rays is accepted as the general reference. Compact/decoded selection and ray
count remain startup controls. Changing them is not a supported live reconfiguration.

| Rays per face | Locked image profiles | Stationary summaries | Preset decision |
| --- | --- | --- | --- |
| 128 | 56/56 passed on the final candidate | 4/4 passed | Reference/default |
| 64 | 38/56 passed; 18 failures, all at 128 cubed | 4/4 passed | Reject general preset |
| 32 | 14/56 passed; 42 failures | 4/4 passed | Reject general preset |

Both compute/geometry producers, 64/128 grids and raw/spatial variants were evaluated.
The locked limits SHA-256 remains
`1fdd961b515736b1a3c7984a27a5514b8c9cbeb375813454e836c1b4c57f01d5`.
Quality captures use averaged reflectance and explicit 6144/12288 MiB caps at 64/128;
benchmark profiles use owner reflectance and 6144 MiB. These distinct configurations are
retained in their manifests. Lower-ray failures remain in [preset-results.json](../build/dynamic-voxel-m8-final/quality/preset-results.json);
they are not converted into accepted presets by faster timings.

## Final performance and correctness (M8.7-M8.8)

The complete 24-cell matrix passed: 64/128 cubed, compute/geometry producers, static
Sponza, moving geometry and changing lights, each with matched cone tracing. All runs
use the final application/shader identity at 1920x1080 with a 1024-square G-buffer.
Three 120-frame measured trials per cell follow 96 warmup frames; cold and traversal
captures remain separate. There are 8,640 accepted steady GPU-frame samples, plus paired
uninstrumented CPU samples. Every DDA measured frame is ready with zero fallback flags.

DDA medians are 3.34-3.36 ms static, 3.89-3.90 ms light updates and 12.39-12.62 ms motion
at 64 cubed. At 128 cubed they are 9.12-9.14, 10.21-10.23 and 66.49-66.71 ms respectively;
motion p95 reaches 76.27 ms. These results support retaining explicit selection.
VMA total/device-local peaks reach 2.625/2.500 GiB at 64 and 6.942/6.817 GiB at 128;
logical directional reserves are 1.689 and 6.000 GiB under the same 6144 MiB method cap.

[Complete timing, pass, memory, initialization and traversal tables](DynamicVoxelGIM8Measurements.md)
retain both medians and tails. [Raw summary](../build/dynamic-voxel-m8-final/matrix/summary.json)
includes all passes and trial variation. The first 64-cubed static cold trial takes
909.92 ms versus a 175.68 ms three-trial median; driver caches are not purged. Cold ranges
remain visible rather than being discarded as outliers. All 36 separate traversal
captures report zero unknowns and zero fallback bits; sampled query counts are not
production-frame ray totals.

The final [image matrix](../build/dynamic-voxel-m8-final/quality/reference-final/results.json)
passes 56/56 profiles plus four stationary summaries (68 captures including stationary
sequences). Both stationary RMS and error to the converged spatial result are zero for
room/Sponza with both producers. This acceptance is against the declared approximate-DDA
limits, not triangle-exact rendering: Sponza at 128 cubed has raw/spatial NRMSE of
45.90%/39.84% against the triangle image reference.

[Compact/decoded equivalence](../build/dynamic-voxel-m8-final/quality/decoded-reference/equivalence.json)
passes all 16 room/Sponza profiles, covering both resolutions/producers and raw/spatial
filters. Raw/final static faces and final lighting are byte-identical.

[V0/V1 regression validation](../build/dynamic-voxel-m8-final/prerequisites/index.json)
passes 332 raw-volume/class-lifecycle checks across 12 case groups. Both producers and
all submission modes preserve owner/averaged occupancy, material attributes, contributions,
rebuild/remove/restore/in-flight behavior and class queries. Mixture order, duplication
and tessellation checks also pass, including 128/256 grids.

The 13 [supplemental cases](../build/dynamic-voxel-m8-final/supplementary/summary.json)
cover lower-ray presets, inline/shared submission, decoded layout and selection/fallback.
At 64 cubed, 32/64 rays reduce moving GPU medians to 5.85/7.96 ms, but neither is an
accepted general quality preset. The 64-ray uninstrumented CPU median remains 12.36 ms;
a GPU interval reduction does not guarantee a throughput improvement.

Decoded static Sponza uses 4.19/10.57 ms GPU and 6.936/12.942 GiB total VMA commitment at
64/128 cubed, compared with compact's 3.36/9.14 ms and 2.625/6.942 GiB. The layouts use
6144 MiB at 64; the 128 decoded reference needs the separately declared 12288 MiB cap.
Reserved receiver capacities differ, so the 12x record-size reduction is not a 12x total
memory claim. These results and byte-equivalence support retaining compact.

Shared-queue 64-cubed static/motion GPU medians are 2.57/11.37 ms; inline gives 3.24/12.30 ms.
The measured subset can guide explicit workload choices, but does not justify a universal
queue-policy change across producers, grids and hardware. `auto` selects cone in all
three trials; 16 MiB preflight selects cone, and 1 MiB selects PBR (`none` in the GI CSV).

[Profiling equivalence](../build/dynamic-voxel-m8-final/profile-equivalence/results.json)
passes both inline/shared and threaded/dedicated modes with synchronization validation:
lighting, surface data and raw/final static faces are byte-identical with profiling on/off.
All 210 cold/profile run identities across the matrix and supplements are unique.
The exact original `Data/engine.cfg` bytes are restored and no configuration lock remains.

| Validation | Result | Evidence |
| --- | --- | --- |
| Full native GI | 144 passed; 4 raw failures, accepted isolated planar case | [Native acceptance](../build/dynamic-voxel-m8-final/native-acceptance.json) |
| Cache epoch/rejected-publication assertions | 4/4 passed | [Telemetry tests](../build/dynamic-voxel-m8-final/correctness-v2/telemetry/native.json) |
| RenderCore | 519 passed, 7 disabled | [Unit results](../build/dynamic-voxel-m8-final/rendercore-final.json) |
| Full Vulkan integration | 291/291 passed | [Native results](../build/dynamic-voxel-m8-final/correctness-v2/vulkan/native.json) |
| SPIR-V validation, Vulkan 1.1 target | 117 modules, zero failures | [Validation](../build/dynamic-voxel-m8-final/spirv-validation.json) |
| Dynamic lifecycle, both producers/all four submission modes | 72 captures passed | [M4](../build/dynamic-voxel-m8-final/correctness-v3/m4/results.json) |
| Raw/temporal/full filtering with extended lighting | 54 captures passed | [M5](../build/dynamic-voxel-m8-final/correctness-v3/m5/results.json) |
| Lighting, emission, material, texture/UV and normal-transform contracts | 54 captures passed | [M6](../build/dynamic-voxel-m8-final/correctness-v3/m6/results.json) |
| Grids, switches, triangle fixtures, Sponza, budget fallback | 111 captures passed | [M7](../build/dynamic-voxel-m8-final/correctness-v3/m7/all-results.json) |
| Material/opacity, fixed bounds, explicit rebuild and cone/PBR contracts | 184 captures passed | [Contracts](../build/dynamic-voxel-m8-final/correctness-v4/contracts/results.json) |
| Profiler/runner Python tests | 19 + 7 passed | `tools/test_engine_profile.py`, `tools/test_dynamic_voxel_benchmark.py` |
| Owned C++ formatting | 20 files pass clang-format 19 dry-run | [Formatting record](../build/dynamic-voxel-m8-final/formatting.json) |

The four planar failures are exactly `0.0021588802337646484` against the unchanged
`0.002f` bound, in the four submission modes. All 336 assertion messages were checked;
no other failure is accepted. The user explicitly accepts this isolated AMD difference.
Neither this native bound nor the image/leakage/temporal limits were loosened.

The initial full Vulkan run encountered overlay-related swapchain failures. The same
byte-identical test binary passed affected tests and then all 291 tests when run through
the existing RTSS-excluded `7zFM.exe` filename. No global overlay configuration or engine
behavior was changed. The original failing logs are retained in `correctness/vulkan/`;
accepted results are in `correctness-v2/vulkan/`, with synchronization validation enabled.

Legacy lifecycle validators incorrectly inferred cache rebuilds from submitted batch
counts. Compact caches can keep submitting empty ranges after all occupied receivers are
ready. Python checks now use completed GPU work/readiness and visibility/grid revisions;
the in-demo material harness checks cache epochs. Rejected pre-correction runs remain
under `correctness-v2/m4/` and `correctness-v3/contracts/`.

## Promotion decision (M8.9)

Keep `auto` on cone tracing and `dynamic_voxel` explicit. For the measured experimental
profile, start at 64 cubed; 128-cubed moving geometry is substantially more expensive.
Retain 128 rays, compact cache,
64-thread gathering, radius 2 and the existing filtering behavior. No automatic backend,
ray-count, grid-resolution or queue-policy change is justified by this AMD-only evidence.
An explicit method memory cap remains required; measured fixture caps are not a universal
shipping default. Unsupported/over-budget DDA requests retain cone/PBR fallback.

The method remains single-bounce diffuse transport with approximate occupied-cell
visibility, not triangle visibility, multi-bounce lighting or glossy GI. Hardware RT,
NVIDIA/MoltenVK profiling, native allocation-failure recovery and a unified live graphics
settings service remain outside this Stage A completion scope.


## Reproduction and artifact ownership

Run from the repository root, with no other configuration-mutating capture or GPU
benchmark active. The benchmark takes an explicit executable and asset; paths below
show this checkout's retained final inputs.

```powershell
python tools/benchmark_dynamic_voxel_gi.py `
  --executable build/dynamic-voxel-m8-final/candidate-v2/scene_renderer_demo.exe `
  --asset D:/Dev/glTF-Sample-Models/2.0/Sponza/glTF/Sponza.gltf `
  --output build/my-fresh-m8-static-run --resolution 64 --voxelizer comp `
  --rays 128 --cache compact --budget-mb 6144 --workload static `
  --width 1920 --height 1080 --gbuffer 1024 --thread 1 --async-compute 1 `
  --vsync 0 --require-async --trials 3 --warmup 96 --frames 120 `
  --cold-frames 64 --diagnostics
```

Each fresh output directory contains exact configuration bytes, commands, per-trial logs,
raw frames/pass CSVs, profile JSON, implementation/input identities, traversal data and
its acceptance manifest. `matrix-index.json` records every required cell's command;
`supplementary-index.json` records lower-ray, submission and fallback checks. Intrusive
capture commands never enter the timed aggregates. `build/m8-summarize.py` consolidates
accepted manifests and raw measured samples; its JSON preserves every pass and trial.

[Artifact root](../build/dynamic-voxel-m8-final/) retains intermediate/rejected evidence
separately from accepted sets. Local build artifacts are not a substitute for committing
or distributing them if another machine must reproduce the exact binaries. No source
commit, pull request or external publication is performed by this work.
