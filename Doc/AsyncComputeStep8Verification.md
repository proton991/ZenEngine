# Async compute Step 8 verification

Implemented and verified on 2026-09-20. Step 8 completes the scheduled-frame failure and publication review. Stopped before Step 9 for user verification.

## Changes

`RHICommandListExecutor` now classifies presentation-command rejection as fatal when scheduled work was already accepted, or when the failed presentation submission itself advanced graphics progress. It stops presentation and retains the owned batch through safe teardown. Recoverable viewport recreation continues through the existing viewport protocol and does not invalidate accepted compute outputs.

Progress reporting captures all logical queues' submitted serials before querying completion. A completion-query exception therefore cannot hide accepted compute work merely because the graphics completion query ran first. Failed tickets still include the submitted snapshot, while uncertain completion blocks further submissions and retirement.

`RenderDevice` tracks every valid handoff ticket, including immediate inline failure and a failed history commit after handoff. Those tickets always reach frame completion processing, which extends retirement with accepted serials. A successful native result also requires valid scheduled history before publishing extractions or confirmed resource state. Failure clears speculative history and blocks further frames.

`RendererServer` retains its existing `RequestVoxelization()` call on immediate graph failure. The code now explains why it restores a request consumed during recording, and why deferred native failure requires device recovery before another update. No graph is automatically replayed after uncertain or partial execution.

## Failure matrix evidence

| Boundary | Verified behavior |
| --- | --- |
| Compile, allocation, recording, or validation failure | Existing transaction tests restore command checkpoints/private state and publish no extraction; later-group recording failure submits no groups. |
| Declined handoff | Prior accepted resource history and defined contents survive; no new batch or extraction is published; a rebuilt graph can retry. Existing executor preflight tests also verify commands are not detached on rejection. |
| First native rejection without accepted work | An accepted frame blocks later submissions and discards remaining groups. Existing standalone tests retain retryability without advancing the native frame. |
| Accepted or uncertain work followed by failure | Remaining groups and presentation stop, accepted compute serials survive in retirement, resources stay retained through device drain until teardown, and later submissions are blocked. |
| Deferred failed producer | A following graph can finish CPU recording, but handoff rejects it after producer failure; neither frame publishes extraction or speculative resource history. |
| Presentation-command rejection | Accepted compute work makes the frame fatal; presentation is not called and the accepted compute lifetime gate remains. |
| Recoverable presentation/recreation | Native acceptance permits extraction publication before GPU completion; resize drains accepted work, including compute, and retains the output. |
| Invalid confirmation | Even a successful native ticket cannot publish extraction when its scheduled producer history has failed. |
| Immediate voxel request failure | The consumed request can be restored with the renderer's existing retry call, schedules one successful reset on retry, and remains cached afterward. |

The 26 new CPU cases reuse the existing owned-schedule and RenderDevice fixtures. Executor cases cover inline/threaded execution and distinct/aliased compute-transfer queues. Two new native cases inject a Vulkan timeline-counter failure after compute submission, preserve the accepted compute serial, and verify that the dependent graphics group never submits. Existing native dispatch/readback, barrier, queue-topology, indirect-command, and lifetime coverage also passed.

## Verification

All five MSVC x64 Debug targets built successfully: `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 445 enabled tests passed; 7 existing disabled benchmarks |
| VulkanRHIIntegrationTest | 271 passed; no skipped tests |
| Total | **758 enabled tests passed** |
| Scene smoke matrix | **8 runs × 64 frames passed** |

The smoke matrix covers voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`, including the existing mode-switch and resize/minimize sequence. Vulkan synchronization validation reported zero VUIDs and zero `SYNC-HAZARD` messages. Tests and smoke runs reported no leaks. `Data/engine.cfg` was restored byte-for-byte, with matching SHA-256 `754289c7835e75d304d0eb1ecb33c247fbb61bd1cec4748755525ec929b922b7`.

Project formatting and whitespace checks passed. Added C++ code uses explicit types, final returns, short callbacks, and engine containers; it introduces no `std::array`, `std::vector`, or exception-based error handling. The RDG/backend boundary audit found no direct backend or submission-progress queries in RenderGraph, RDGResourceManager, or RenderSubmissionHistory.

Evidence is under `build/async-compute-step8-*`: build logs, suite logs/XML, focused failure logs/XML, scene logs, `smoke.json`, and `verification.json`. The native launcher kept validation enabled and left persistent overlay settings unchanged.

## Next boundary

Step 9 subsequently added the ComputeVoxelizer queue annotations, reset preference, first-use diagnostics, and repeated-update smoke coverage; see [Step 9 verification](AsyncComputeStep9Verification.md). A subsequent [Nsight Graphics verification](AsyncComputeNsightVerification.md) captured repeated-update GPU timings after counter access was enabled, confirming dedicated compute and correct dependencies but no overlap in the measured workload. First-load timing and full performance acceptance remain open. The Step 8 smoke results above predate renderer opt-in and do not establish a production performance benefit.
