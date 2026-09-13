# RenderCore / RHI threading implementation

The main thread remains RenderCore and owns window events, scene updates, RDG compilation,
pass callbacks, and RHI command recording. One dedicated `RHIThread` owns native command
translation, submission, presentation, and runtime backend operations. This follows the
RenderCore-to-RHI command-list boundary in the UE5 checkout cited by the
[original plan](RenderCoreRHIThreadingPlan.md).

## Execution and ownership

- `RHICommandListExecutor` wraps `DynamicRHI`. Its bounded FIFO holds up to 64 tasks and sleeps
  on condition variables. Synchronous backend requests use `Invoke`; nested requests from the
  RHI thread execute immediately. Stop drains pending tasks and joins the worker.
- Collections use project containers: `Queue` for worker tasks, `HeapVector` for retained
  batches, resource references, frame storage, and extractions, and `FlatHashMap` for resource
  deduplication.
- A viewport frame detaches its command arena into an owned `RHICommandBatch`. The reusable
  producer retains a shared context owner, whose native state is used on the RHI thread.
  Rendering layouts are copied, and commands retain their referenced resources, including
  texture-view owners. Rollback releases references acquired after its checkpoint.
- RDG pool reuse preserves native resource identity: it reuses the same allocation and
  descriptor contents. Logical RDG versions and content tracking remain separate. Descriptor
  keys use stable resource IDs while RHI translates commands for an older frame.
- Bindless recording captures a protected lifetime token without collecting native resources
  on RenderCore. Final resource/context destruction is routed to the RHI thread. Native
  creation, mapped-memory operations, texture views, uploads, and cache misses can use
  synchronous requests. Backend bootstrap occurs before the worker starts.
- RenderCore can record frame N+1 while RHI executes N. The next frame handoff waits for N's
  submission result, limiting the viewport path to one unconfirmed frame. GPU frame slots
  have separate serial-based reuse gates.
- `GRenderFrameState` selects RenderCore frame storage. A pending ticket retains a pointer to
  that stable frame so delayed results cannot update the current slot by mistake. The worker
  initializes and uses the independent `GRHIFrameState`; backend `EndFrame()` advances it.
  Batches and results contain no duplicate frame-number or slot fields, and neither thread
  reads the other's mutable timeline during execution.
- Tickets carry submission status, queue serials, presentation status, and recreation requests.
  RenderCore owns scheduled and confirmed resource states; extraction results become visible
  only after successful native submission is consumed.
- Asynchronous rejection or partial failure blocks dependent frames. Started batches remain
  retained until device teardown after an idle wait. Resize, viewport destruction, and
  shutdown drain submission tickets and GPU work before releasing native owners. Swapchain
  recreation is requested by RHI and coordinated by RenderCore.

The first implementation conservatively retains both command arenas and resource references
until GPU retirement. CPU translation completion and GPU completion are separate milestones;
recycling arenas immediately after translation is a future optimization.

## API and startup options

`RenderConfig::rhiExecutionMode` defaults to `RHIExecutionMode::eThreaded`. The RenderDevice
constructor also accepts an explicit mode. Select the mode before constructing the device;
runtime switching is not supported.

In threaded mode, `ExecuteRenderGraph(viewport)` returning true means **queued successfully**.
Use `PollFrameSubmissions()` to consume ready results, or `FlushRHIThread()` to wait for RHI
execution and consume all results. Neither operation by itself establishes GPU completion.
`WaitForPreviousFrames()` additionally waits for the GPU. `AreSubmissionsBlocked()` reports
late failures. The offscreen `ExecuteRenderGraph(RenderGraph&)` and upload paths preserve
their synchronous submission contract.

`CollectCompletedResources()` requests a GPU-progress sweep on RHI, then consumes the latest
published serials. Keep calling it while rendering is paused so later GPU completion can retire
both batches and RenderCore's deferred owners. `RHICommandListExecutor::PollGPUProgress()`
coalesces requests; a full FIFO defers the request until a later poll rather than waiting for
capacity. Completed-serial getters remain cached snapshots. `FlushRHIThread()` also refreshes
progress after fencing queued CPU work, but never waits for unfinished GPU submissions.
Progress-query failure blocks submissions and retirement even without a pending frame ticket.

Run the scene demo from `bin`, where the configured model paths resolve correctly:

```powershell
Set-Location E:\Dev\ZenEngine\bin
.\scene_renderer_demo.exe --rhi-thread=1 --frames=64 --smoke-test
.\scene_renderer_demo.exe --rhi-thread=0 --frames=64 --smoke-test
```

The smoke sequence switches PBR/voxel rendering, resizes the window, minimizes/restores it,
and drains work on shutdown. `--frames=N` also enables bounded runs without the smoke actions.
Logs report RenderCore loop duration, RHI frame execution duration, queue delay, completed
batches, and peak pending batches. Durations are elapsed wall time, including waits, rather
than operating-system CPU utilization. RHI batch counters apply to the asynchronous frame
path; they are zero in the inline demo.

## Initial verification

Validation date: 2026-09-13. Configuration: MSVC Debug, Windows, NVIDIA RTX 5080.

| Check | Result | Local evidence |
| --- | --- | --- |
| Four Debug targets | Build passed | `build/rhi-thread-build.log` |
| RenderCore | 214 passed, 7 existing disabled tests | `build/rhi-thread-rendercore.log`, `.xml` |
| Vulkan RHI unit tests | 28 passed | `build/rhi-thread-rhi.log`, `.xml` |
| Native Vulkan integration | 205 passed with synchronization validation enabled | `build/rhi-thread-vulkan-integration-clean.log`, `.xml` |
| Threaded scene smoke | 64 frames, exit 0, no validation errors | `build/rhi-thread-smoke-threaded-clean.log`, `.err` |
| Inline scene smoke | 64 frames, exit 0, no validation errors | `build/rhi-thread-smoke-inline-clean.log`, `.err` |

All test and smoke processes reported no tracked memory leaks. The eleven new threading tests
are part of RenderCore's total. `git diff --check` passed. Added C++ uses explicit types,
single function exits, and short callbacks where required; changed ranges follow the
repository's clang-format configuration.

For the final 64-frame smoke run, threaded RenderCore loop time totaled 348.23 ms, RHI frame
execution totaled 58.39 ms, queue delay totaled 0.92 ms, and peak unconfirmed batches was 1.
The inline loop totaled 340.82 ms. These include smoke actions and synchronization validation;
the slightly slower threaded result is not a performance benchmark or a speedup claim.

The threading tests cover FIFO ordering and draining, nested calls and exceptions, inline
thread identity, delayed layout/resource consumption, CPU versus GPU retirement, rollback,
frame-slot reuse, queued cancellation, partial native failure, and delayed extraction.
A gated test releases the blocked RHI submission only from the next frame's command-recording
callback, proving actual recording overlap without relying on a timing speedup.
Another gated test gives RHI two slots and RenderCore three, offsets their frame numbers,
and advances RenderCore before consuming a result. Reusing RenderCore's original slot must
wait for that result's GPU serial, independent of the current slot on either timeline.

Build from an x64 MSVC developer shell:

```powershell
cmake --build build/x64-windows-msvc-debug --target RenderCoreTest VulkanRHITest VulkanRHIIntegrationTest scene_renderer_demo --parallel 8
.\bin\RenderCoreTest.exe --gtest_color=no
.\bin\VulkanRHITest.exe --gtest_color=no
$env:VK_LAYER_VALIDATE_SYNC = '1'
$env:VK_LOADER_LAYERS_DISABLE = '~implicit~'
$env:DISABLE_RTSS_LAYER = '1'
.\bin\VulkanRHIIntegrationTest.exe --gtest_color=no
```

This machine has a separate RTSS injection hook in addition to its implicit Vulkan layer.
As documented in the earlier local Phase 4 verification, native WSI tests require an identical
temporary test-executable copy named `bin/7zFM.exe`, whose installed RTSS template disables
hooking. The existing local `build/rhi-phase4/run_validation.py` performs that isolation with
the three environment variables above and removes the copy afterward. No installed executable
or overlay settings are changed. Validation errors remain enabled. Scene smoke runs use the
process-local layer exclusions above.

The short validation runs exercise the covered behaviors and observable overlap. They do not establish
a performance improvement: synchronous uploads, resource creation, cache misses, and slot waits
can still serialize work. Parallel RDG recording, multiple RHI translation workers, asynchronous
resource creation, and early command-arena recycling remain outside this initial implementation.

## Review corrections and revalidation

The 2026-09-13 review found two gaps in the initial tests: RDG pool reuse changed native
generations while RHI could still read them, and idle retirement never refreshed cached GPU
completion. Pool reuse now preserves native resource identity. Idle collection queues one progress
sweep, retries a full FIFO on a later poll, and keeps ownership retained after query failures.
CPU flush samples progress without turning into a GPU wait.

Six regression tests cover overlapping buffer/texture reuse, idle batch and deferred-owner
retirement, CPU-flush semantics, coalescing, queue saturation/retry, and query failure without
a pending frame. The original two isolated review reproductions also pass unchanged.
MSVC uses `/bigobj` for the shared RenderCore test translation unit, which exceeded the
standard COFF section limit after adding these tests.

| Check | Result | Local evidence |
| --- | --- | --- |
| Four Debug targets | Build passed | `build/rhi-fix/build-final.log` |
| RenderCore | 220 passed, 7 existing disabled tests; 17 threading tests included | `build/rhi-fix/rendercore.log`, `.xml` |
| Vulkan RHI unit tests | 28 passed | `build/rhi-fix/vulkan-unit.log`, `.xml` |
| Native Vulkan integration | 205 passed with synchronization validation; no validation errors | `build/rhi-fix/vulkan-integration.log`, `.xml` |
| Original review reproductions | Both passed | `build/rhi-fix/original-probes.log` |
| Threaded and inline scene smoke | 64 frames each, exit 0, no validation errors | `build/rhi-fix/smoke-1.log`, `smoke-0.log`, corresponding `.err` files |

All runs reported no tracked memory leaks. `git diff --check` passed. Native integration used
the same temporary executable isolation described above. No speedup is claimed from these
short validation runs.

## Final resource history cleanup

The subsequent review found that a batch could own the last buffer or texture reference after
RenderCore released its deferred owners. GPU retirement then destroyed the resource on RHI,
leaving its stable ID in RenderCore's persistent state and content history.

Final buffer/texture destruction now publishes the stable ID to a mutex-protected mailbox in
`RHICommandListExecutor`. RenderCore drains this mailbox during submission polling, resource
collection, and shutdown, and removes the ID from persistent, confirmed, and pending frame
states and validation history. Draining is deferred while RDG records and submits a graph so
private tracker or metrics copies cannot restore removed entries on commit or rollback. This
cleanup introduces no native queue or GPU waits and does not change the error-handling policy.

Four regressions cover final RHI release of both resource types during frame overlap, retirement
during recording with a pending snapshot, inline ownership retention, and rollback. The original
isolated reproduction also passes unchanged.

Validation date: 2026-09-13, MSVC Debug, with the native validation setup described above.

| Check | Result | Local evidence |
| --- | --- | --- |
| Four Debug targets | Build passed | `build/rhi-history-fix/build-all.log` |
| RenderCore | 224 passed, 7 existing disabled tests | `build/rhi-history-fix/rendercore.log`, `.xml` |
| Vulkan RHI unit tests | 28 passed | `build/rhi-history-fix/vulkan-unit.log`, `.xml` |
| Native Vulkan integration | 205 passed with synchronization validation; no validation errors | `build/rhi-history-fix/vulkan-integration.log`, `.xml` |
| Original final-release reproduction | Passed | `build/rhi-history-fix/original-probe.log` |
| Threaded and inline scene smoke | 64 frames each, exit 0, no validation errors | `build/rhi-history-fix/smoke-threaded.log`, `smoke-inline.log` |

All test and smoke processes reported no tracked memory leaks. `git diff --check` passed.
