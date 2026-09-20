# Async compute Step 7 verification

Implemented and verified on 2026-09-20. This step connects the scheduler, group recorder, resource history, and owned RHI submission path. Stopped before Step 8 for user verification.

## Owned submission and execution

`RHICommandBatch` owns a `HeapVector` of recorded groups. Every group owns its detached command list, the resources retained by that list, and exact predecessor references. The returned batch result includes each group's status and logical queue/serial, together with the retirement completion set and presentation result. Recording inputs are borrowed only until handoff; the worker receives no RenderGraph pointer or pass callback.

`SubmitFrame()` accepts a complete schedule and retains its single-list convenience overload. Preflight validates every list, context, producer point, and predecessor direction before detaching any commands. Duplicate lists, mismatched contexts, and forward/self group references reject the entire handoff without consuming its recording. Empty schedules can still process presentation and the native frame boundary without manufacturing a compute group.

The worker processes groups in topological order. It resolves each exact predecessor, validates the accepted serial, attaches dependencies, and finalizes/submits only that ready group. It records the actual producer serial before advancing to a consumer. It never relies on the backend's graphics/compute/transfer flush enumeration to order the schedule. Native aliases retain producer order while the backend omits unnecessary semaphore waits; the group recorder supplies their ordinary barriers.

Presentation uses the existing graphics viewport protocol after the scheduled groups have been accepted. One frame ticket covers the entire batch, and successful frame processing ends the native frame once. Completion requirements retain each logical timeline, including unrelated compute work with no graphics join. Recycling keeps separate queues of idle command storage, releases retired contexts on RHI, and restores the recording producer's context during detach.

## RenderDevice integration and transaction boundary

With async compute enabled and supported, both frame and standalone graph execution prepare resource history, acquire queue-specific lists, and record the schedule transactionally. `SetQueuePreference(ePreferAsyncCompute)` now causes eligible passes to execute on compute in inline and threaded modes. Independent graphics prefixes retain their own submissions and waits. Frames without eligible work create no empty compute submission.

The frame handoff applies the existing pending-frame backpressure once, after recording. Both grouped frames and the legacy single-list fallback share history validation, extraction staging, and queued publication. Prior frames can remain on the worker while the next graph records; a delayed-worker test resets and rebuilds the graph while its first batch owns the recorded work. A ticket confirms native CPU submission processing, not GPU completion.

Standalone schedules use the same owned worker processing through `SubmitGroups()`, with synchronous CPU confirmation and no native `EndFrame`. A rejected first native submission remains retryable if no GPU work was accepted. A later failure after accepted work is fatal: remaining groups and presentation stop, speculative points fail, resources remain owned for safe teardown, and further submissions are blocked. Frame rejection after asynchronous acceptance keeps the existing blocking policy. Step 8 remains the separate review and completion gate for the full failure/publication matrix and renderer recovery behavior.

Disabled or unsupported async policy preserves the established single-list graphics/transfer fallback, including its conservative reader chaining and timeline-disabled transfer behavior. No voxelizer passes were annotated in this step; those changes remain in Step 9. Startup logs now distinguish an available scheduler for opted-in passes from graphics fallback, without claiming that the current voxelizer used compute.

## Verification

All five MSVC x64 Debug targets built: `ConfigLoaderTest`, `VulkanRHITest`, `RenderCoreTest`, `VulkanRHIIntegrationTest`, and `scene_renderer_demo`.

| Coverage | Result |
| --- | --- |
| ConfigLoaderTest | 7 passed |
| VulkanRHITest | 35 passed |
| RenderCoreTest | 419 enabled tests passed; 7 existing disabled benchmarks |
| VulkanRHIIntegrationTest | 269 passed; no skipped tests |
| Total | **730 enabled tests passed** |
| Scene smoke matrix | **8 runs × 64 frames passed** |

Final logs contain zero VUIDs, zero `SYNC-HAZARD` reports, and no reported leaks. The scene matrix covers voxelizer `auto`/`comp`, RHI thread `0`/`1`, and async policy `0`/`1`, with the existing mode switches and resize/minimize sequence. Configuration restoration was byte-for-byte. Formatting, whitespace, and direct RDG backend/progress-query audits passed. Added C++ code uses engine containers, explicit types, final returns, and no new exception-based error handling; it introduces no `std::array` uses.

The 25 new CPU cases cover inline/threaded execution and distinct/aliased compute-transfer queues: exact group serials, producer-before-consumer preflight, independent prefixes, one presentation/frame boundary, malformed schedule rejection before detach, accepted-prefix failure, retryable standalone rejection, compute retirement without a graphics join, recycling across context types, production frame queue selection, recording rollback, extraction publication, an ordinary following frame, and graph rebuilding during delayed RHI execution. Earlier planning tests now assert actual compute submissions; metric checks identify nodes by ID because independent work can be grouped into a different execution order.

Four new native cases run in inline/threaded and timeline/fence modes. Each submits both graphics → compute → graphics and transfer → compute → graphics schedules through one owned frame batch, with an independent graphics prefix. The producer uploads a storage buffer and an `R32UInt` image; the compute shader modifies both; graphics copies its output for CPU readback. The expected values are `0x100d0a0c` and `0x100d0a0e`. Native wait capture verifies exact producer serials and zero host timeline waits before the explicit readback wait. The test uses queue equivalence rather than a family-only prerequisite and explicitly skips if no distinct compute queue is available. Existing native tests continue to cover shared native queues, separate indices within one family, indirect commands, progress failures, and lifetime gates.

Evidence is under `build/async-compute-step7-*`: build logs; `config`, `rhi`, `rendercore`, and `integration` logs/XML; native focused logs/XML; scene logs and `smoke.json`; and the final verification summary. The isolated native launcher retains synchronization validation and leaves persistent overlay settings unchanged. The smoke runner restores the exact original configuration bytes.

## Next boundary

Step 7 is the owned submission and production scheduler connection. The subsequent failure/publication review is now complete; see [Step 8 verification](AsyncComputeStep8Verification.md). Execution is paused before Step 9, which will annotate the ComputeVoxelizer chain and complete renderer controls, repeated-update coverage, diagnostics, and performance measurement. These tests establish submission and synchronization correctness. They do not establish production voxelizer overlap or a performance benefit.
