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

## Windows message waits

Windows swapchain operations can synchronously send messages to the window's owning
thread. Waiting for RHI through a future, a full FIFO, or a thread join without processing
those messages can deadlock that thread with RHI. This applies to creation, resize,
presentation and shutdown; see the
[Vulkan Win32 surface requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateWin32SurfaceKHR.html).

`RHIThreadEvent` now uses a manual-reset Windows event and
`MsgWaitForMultipleObjectsEx` with `QS_SENDMESSAGE`. `PeekMessageW` services sent messages
without removing posted messages, input or `WM_QUIT`. Synchronous calls and frame tickets
signal their own completion events; full-queue waits use a capacity event, and shutdown
waits for the native thread handle before joining. Queue locks are released before a
wait can enter a window procedure. Other platforms use condition-variable event waits.

Sent `WM_SIZE` messages update the GLFW window's dimensions immediately. Application
resize callbacks are coalesced and run from `GlfwWindowImpl::Update()` after event polling,
so viewport recreation cannot re-enter a pending RHI wait. A new resize received while
that callback runs remains pending for the following update.

Four Windows RenderCore regressions cover synchronous invocation, frame-ticket waits,
full-queue backpressure, and draining active and queued work during shutdown. They also
verify that posted messages and quit requests remain available to the application. A GLFW
regression sends resize messages from RHI, verifies callback deferral and coalescing, and
uses a callback that makes a synchronous RHI call. The original bounded message-delivery
reproductions pass unchanged. The deadlock tests use `SendMessageTimeoutW` to model the
documented WSI behavior without hanging a failing test process.

Validation date: 2026-09-13, MSVC Debug, with the native validation setup above.

| Check | Result | Local evidence |
| --- | --- | --- |
| Four Debug targets | Build passed | `build/rhi-deadlock-fix/build-final.log` |
| RenderCore | 228 passed, 7 existing disabled tests | `build/rhi-deadlock-fix/rendercore.log`, `.xml` |
| Vulkan RHI unit tests | 28 passed | `build/rhi-deadlock-fix/vulkan-unit.log`, `.xml` |
| Native Vulkan integration | 206 passed with synchronization validation; no validation errors | `build/rhi-phase4/rhi-deadlock-integration.log`, `build/rhi-deadlock-fix/vulkan-integration.xml` |
| Original message-wait probes and control | All three passed unchanged | `build/rhi-deadlock-fix/original-probes.log` |
| Threaded and inline scene smoke | 64 frames each, exit 0, no validation errors | `build/rhi-deadlock-fix/smoke-1.log`, `smoke-0.log` |

All test and smoke processes reported no tracked memory leaks. `git diff --check` passed.

## Native GPU progress failures

Timeline and fence helpers preserve `VkResult` through queue polling and waits. The queue
treats `VK_NOT_READY` and `VK_TIMEOUT` as incomplete work; other unsuccessful results block
the backend. This distinction follows the Vulkan return codes for
[fence status](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetFenceStatus.html),
[timeline counters](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetSemaphoreCounterValue.html)
and [timeline waits](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitSemaphores.html).
A failed status query cannot fall through to another wait, publish a returned counter value,
or admit another native submission. Previously completed serials remain valid, and ownership
of unfinished workloads remains with the queue until teardown. A successful timeline wait
proves completion of its requested serial without requiring a second counter query.

`DynamicRHI::AreSubmissionsBlocked()` exposes the terminal backend state. The executor reads
it on RHI and permanently publishes it through its atomic blocked flag. Polling, synchronous
waits, CPU flushes, explicit retirement collection and inline completion queries all propagate
the failure. A failure detected during frame execution or final progress publication also
produces a fatal submission ticket. RenderCore and batch retirement retain affected owners
until device shutdown.

The regression first reproduced the unblocked executor in both execution modes. Integration
tests inject device-loss return codes into real Vulkan-backed executor calls; they do not
cause actual hardware device loss. Queue tests exercise both timeline and fence paths,
including timeouts, memory errors, invalid returned counters, preserved completion serials,
and rejection of native work after a polling failure. A RenderCore regression verifies that
batch and deferred owners survive subsequent successful queries and release during shutdown.

Validation date: 2026-09-13, MSVC Debug, with the native validation setup above.

| Check | Result | Local evidence |
| --- | --- | --- |
| Original polling regression before the fix | Failed in both execution modes | `build/rhi-phase4/rhi-progress-before.log`, `build/rhi-progress-fix/before.xml` |
| Four Debug targets | Build passed | `build/rhi-progress-fix/build-final.log` |
| Targeted executor and queue regressions | 28 passed | `build/rhi-phase4/rhi-progress-targeted.log`, `build/rhi-progress-fix/targeted.xml` |
| RenderCore | 229 passed, 7 existing disabled tests | `build/rhi-progress-fix/rendercore.log`, `.xml` |
| Vulkan RHI unit tests | 28 passed | `build/rhi-progress-fix/vulkan-unit.log`, `.xml` |
| Native Vulkan integration | 222 passed with synchronization validation; no validation errors | `build/rhi-phase4/rhi-progress-integration.log`, `build/rhi-progress-fix/vulkan-integration.xml` |
| Threaded and inline scene smoke | 64 frames each, exit 0, no validation errors | `build/rhi-progress-fix/smoke-1.log`, `smoke-0.log` |

All test and smoke processes reported no tracked memory leaks. `git diff --check` passed.


## Window surface ownership on macOS

The Windows message-wait fix does not address macOS view ownership. Threaded viewport
creation still called `glfwCreateWindowSurface` from RHI. The bundled Cocoa implementation
attaches a `CAMetalLayer` with `NSView::setLayer:` and `setWantsLayer:` there. On Apple M3 Pro /
MoltenVK, the scene submitted frames without Vulkan validation errors while the window
remained white; inline execution displayed the scene.

Viewport creation and resize now use the same window-thread orchestration on every platform:

- `CreateViewport` prepares the platform surface on the window-owning thread, then invokes
  native viewport allocation, swapchain creation and backbuffer initialization on RHI.
- `VulkanSwapchain` consumes a prepared surface and no longer calls window APIs.
- Resize tears down native resources on RHI and preserves the surface for ordinary resize.
  Surface loss returns to the window thread to create its replacement before RHI rebuilds.
- `GlfwWindowImpl` asserts surface-operation thread ownership. The worker never needs to
  synchronously dispatch back to a blocked main thread. RHI threading remains enabled.
- Surface handoff uses automatic cleanup instead of an added catch/rethrow block. A blocked
  resize logs and returns without replacing resources or throwing a new exception.

Validation on macOS, 2026-09-13:

- All four Debug targets build. RenderCore: 225 passed (four Windows-only cases are excluded);
  Vulkan unit tests: 28 passed.
- Eight new native cases cover inline/threaded creation, native swapchain thread identity,
  ordinary resize, injected surface loss, zero-extent startup/minimize/restore, and blocked
  resize preserving its viewport without throwing.
- The targeted surface/swapchain suite passes 36 cases and skips four presentation-fence
  cases because this MoltenVK device lacks swapchain maintenance. Those existing tests now
  report missing device support as a skip instead of asserting the extension is present.
- The threaded demo's scene was visually verified through an identical temporary app-bundle
  copy. Threaded and inline 64-frame smoke runs cover renderer switching, resize,
  minimize/restore and shutdown with synchronization validation requested.

Evidence: `/tmp/zen-macos-surface-final-build.log`, `/tmp/zen-macos-surface-rendercore.log`,
`/tmp/zen-macos-surface-vulkan-unit.log`, `/tmp/zen-macos-surface-no-throw-tests.log`,
`/tmp/zen-macos-surface-visual.log`, and `/tmp/zen-macos-surface-smoke-{threaded,inline}.log`.
The exception-policy follow-up rebuilt the demo/integration targets and passed another
threaded 64-frame smoke run with no Vulkan validation errors or tracked leaks; see
`/tmp/zen-macos-surface-no-throw-build.log` and `/tmp/zen-macos-surface-no-throw-smoke.log`.
Windows runtime validation of this surface change remains for a Windows machine.

## Command list arena reuse

The threaded frame path previously allocated a fresh 64 KiB command arena while detaching
the recorded list and another for presentation. Completed batches destroyed both lists.
This added about 128 KiB of cumulative allocation traffic per viewport frame even though
live memory stayed bounded.

The executor now recycles command lists only after CPU execution and all recorded GPU
serials complete. Each cache retains at most `RHIFrameState::kMaxFramesInFlight` lists.
Detached lists reset their command objects and resource references, then release their
shared producer context on RHI before returning empty storage to RenderCore through a
mutex-protected cache. Presentation lists retain their dedicated graphics contexts in a
separate cache owned by RHI. Both caches are cleared before backend destruction. Failed
batches keep the existing shutdown-only retirement rule.

`PoolAllocator::Alloc()` also reuses retained overflow blocks after `Reset()`. Previously it
allocated another block whenever the first block filled, leaving existing later blocks
unused. This correction lets command lists and RDG arenas preserve their grown capacity.

Regressions verify payload contents and command destruction across repeated detach/reset
cycles without new tracked allocation events, aligned overflow-block reuse, delayed GPU
completion, the cache limit, and context destruction on RHI before backend teardown.

Windows MSVC Debug measurements, 2026-09-13, using `scene_renderer_demo --smoke-test` with
synchronization validation. Values below use MiB (the allocator report labels them MB).

| Threaded run | Total allocated before | Total allocated after | Peak live before | Peak live after | Arena allocation calls before / after |
| --- | --- | --- | --- | --- | --- |
| 64 frames | 31.42 MiB | 23.55 MiB | 20.88 MiB | 20.93 MiB | 133 / 11 |
| 1,024 frames | 162.62 MiB | 32.28 MiB | 20.88 MiB | 20.93 MiB | 2,053 / 11 |
| 8,192 frames | Not measured | 97.48 MiB | Not measured | 20.93 MiB | Not measured / 11 |

At 1,024 frames cumulative tracked allocation fell by about 80%. Arena allocation stayed
at 11 calls and 768 KiB across all three optimized runs. Other temporary container allocations
still contribute to the lifetime total. The cache retains arena capacity, accounting for the
small increase in peak live memory. These are allocation measurements, not an FPS benchmark.
The 1,024-frame inline control reported 31.70 MiB total and 20.78 MiB peak after the change
(31.69 MiB total and 20.78 MiB peak before).

Validation:

- Four Debug targets built successfully: `build/rhi-arena-reuse/build.log`.
- RenderCore: 233 passed, 7 existing disabled tests; Vulkan unit tests: 28 passed.
  Logs and XML: `build/rhi-arena-reuse/rendercore.*`, `vulkan-unit.*`.
- Current native integration suite: 230 passed with no validation errors, including the
  eight window-surface cases added since the preceding Windows validation.
  Evidence: `build/rhi-phase4/rhi-arena-integration.log`,
  `build/rhi-arena-reuse/vulkan-integration.xml`.
- Threaded 64/1,024/8,192-frame and inline 1,024-frame smoke runs all exited 0, with no
  validation errors or tracked leaks. After-run logs: `build/rhi-arena-reuse/smoke-*.log`.
  Before-run evidence: `build/rhi-progress-fix/smoke-1.log` and
  `build/rhi-memory-review/smoke-1024-{1,0}.log`.

All smoke reports ended at 0 bytes current tracked usage. `git diff --check` passed.
