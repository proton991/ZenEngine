# Async compute Step 9 verification

Historical report: the test results below describe the original step. Later synchronization representations and APIs are superseded where noted in [SynchronizationSimplificationVerification.md](SynchronizationSimplificationVerification.md); the original measurements are preserved.

Renderer integration and correctness checks implemented on 2026-09-20. Paused for user verification. **Nsight Graphics capture now succeeds: dedicated compute execution and dependencies are verified, with zero overlap in nine captured update frames and longer measured async update spans. First-load timing and full performance acceptance remain open.**

## Renderer behavior

`ComputeVoxelizer` uses one shared descriptor helper to request async compute for `ResetComputeIndirectComp`, every `VoxelizationComp`, `VoxelizationLargeTriangleComp`, `ResetDrawIndirectComp`, and `VoxelPreDrawComp`. `BeginVoxelization()` accepts an optional queue preference and passes it to `ResetVoxelVolumes`; the scheduler checks compute clear support and resource eligibility. GeometryVoxelizer retains the default preference.

`VoxelDraw2`, skybox rendering, and presentation stay on graphics. The existing produced-element and indirect-buffer declarations, one persistent set of voxel outputs, and immediate retry behavior remain in use. Earlier graphics readers constrain a later compute overwrite through submission history. Cached-output frames do not schedule compute updates. Async policy remains opt-in; `Data/engine.cfg` is unchanged.

To exercise the feature, select `voxelizer=comp` and use `--async-compute=1`. `--async-compute=0` selects the graphics baseline. `voxelizer=auto` selects geometry voxelization on the tested GPU. Both inline (`--rhi-thread=0`) and threaded (`--rhi-thread=1`) modes support GPU compute scheduling.

## Diagnostics

RenderDevice logs first use only after the native batch succeeds and its scheduled history is confirmed. The tested Sponza update reports:

```text
Async compute in use: graph=frame_rdg; passes=6; compute serial=1
```

This reports the actual accepted graph, compute pass count, and logical compute serial. It appears once per device and never appears for geometry-only, async-disabled, or failed-frame tests. The owned pending-frame data retains the label/count without worker access to a live graph.

RDG captures now include bounded submission details: logical queue, native queue equivalence ID, producer group/external reference, producer logical queue, synchronization kind, and wait-stage mask. They use the prepared schedule, including cross-frame dependencies. Nodes retain preference and eligibility/fallback information. `maxSubmissionDetails` and `maxDependencyDetails` cap output without changing execution; omitted counts are reported. External reference IDs are scoped to one capture, not native timeline serials.

`submission_cpu_us` separately reports CPU handoff/backpressure. These CPU numbers do not establish GPU duration, graphics waiting, or overlap. See [RDG metrics](RDGMetrics.md).

## Correctness verification

All five MSVC x64 Debug targets built successfully.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 455 enabled tests passed; 7 existing disabled benchmarks |
| VulkanRHIIntegrationTest | 271 passed; no skipped tests |
| Total | **768 enabled tests passed** |
| Repeated-update smoke matrix | **8 runs × 64 frames passed** |

Ten new CPU cases cover reset preference/default behavior, unsupported-clear fallback, inline/threaded execution, accepted first-use reporting, suppression after failure, exact cross-frame capture dependencies, producer queues, and bounded diagnostics. Existing native deterministic dispatch/readback, image/indirect synchronization, queue-topology, lifetime, and failure tests passed. They establish synthetic output correctness; no bitwise equivalence or performance claim is made for the full voxel visualization.

The smoke matrix covers voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`. Smoke updates occur at frames **0, 2, 3, 8, 11, 12, 21, and 28**: consecutive requests, return from PBR, and requests around resize/minimize/restore. Transfer details are enabled for these captures so volume reset placement is visible.

| Configuration | Compute submissions across 64 frames | First-use messages |
| --- | --- | --- |
| `comp`, async on, inline or threaded | 8 | 1 |
| `comp`, async off, inline or threaded | 0 | 0 |
| `auto`/geometry, either policy, inline or threaded | 0 | 0 |

Each async-on capture places the volume reset and all five compute pass types on compute and `VoxelDraw2` on graphics. Repeated updates show graphics-reader dependencies before compute overwrite. Exactly eight accepted compute serials for eight requests also verifies that the intervening cached/PBR frames add no empty compute submissions.

Final validation logs contain zero VUIDs, zero `SYNC-HAZARD` reports, and no reported leaks. The smoke runner restored configuration byte-for-byte. Formatting, whitespace, added-code preferences, and the RDG/backend boundary audit passed. New code uses engine containers, explicit types, final returns, and no new exception-based error handling.

Evidence is under `build/async-compute-step9-*`: suite logs/XML, scene logs, `smoke.json`, `queue-evidence.json`, and `verification.json`.

## Nsight Graphics verification and remaining performance gate

After the user enabled performance-counter access, six Nsight Graphics 2026.3.1 traces and matching metric exports succeeded. Recorded GPU timestamps confirm the clear/compute chain on the dedicated compute queue, voxel drawing and presentation on graphics, consumer waits on the exact compute signal, and no current-compute wait on the independent skybox submission.

Across nine captured async update frames, measured graphics/compute overlap is zero. For the warmed frame-28 update, the GPU work span is **1.403 ms off / 2.334 ms on**. The independent skybox command buffer lasts only 7.52 µs and finishes before compute starts. These instrumented Debug measurements show no update-frame performance benefit; they do not justify changing the opt-in policy.

Two fresh standalone controls passed with zero VUIDs, zero synchronization hazards and no reported leaks. The Nsight-injected runs have a repeated HOST_WRITE/ALL_COMMANDS barrier diagnostic in both policies, absent from the controls and from captured application image barriers. They are not validation-clean; exact attribution remains a profiler-interaction investigation. See the report for diagnostics, timestamp methodology, timing tables, limitations, and reproduction artifacts.

First-load frame 0 is absent from these captures; pure semaphore-wait time is not isolated, and a repeatable low-overhead performance comparison is still needed. **The full performance gate remains open, but no longer because of counter permissions.** The earlier failed Nsight Systems registration and Nsight Graphics counter-access attempts remain recorded in the original `profile-*` / `gputrace-*` logs.

See [Nsight verification](AsyncComputeNsightVerification.md) and its [warmed-update timeline](assets/async-compute-step9-nsight-warm.svg). Paused for user verification; no engine source changes were made during this profiling follow-up.
