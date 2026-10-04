# RHI stall watchdog implementation plan

Status: proposed, 2026-10-01. Based on revision `c9fdebfdba2bc8873066650cc10ff81a9126fe3e` plus the uncommitted message-wait fix described in [Window hang detection](RenderCoreRHIThreadingVerification.md#window-hang-detection). This document defines future work; it does not implement it.

Every wait on the RHI thread is currently unbounded. If an RHI task never returns, for example a Vulkan wait on a fence that is never signaled, the waiting thread blocks forever with no diagnostic. The window stays responsive to Windows, so neither the OS nor the engine reports anything. Add a watchdog to those waits. When a wait exceeds a threshold, it reports what is blocked, how long it has waited, and what the RHI thread is doing. An optional fatal threshold lets automated runs fail instead of hanging.

Scope: `Graphics/RHI` waits (`RHIThreadEvent`, `RHIThread`, submission tickets), `RHIOptions`, the demo's command line, and RenderCore tests. Out of scope: GPU hang detection (device loss already blocks submissions), stalls on threads that are not waiting on RHI, and bounding the RHI thread's own Vulkan waits.

## 1. Current waits

| Wait | Code | Current behavior |
| --- | --- | --- |
| Synchronous call | `RHIThread::InvokeQueued` → `completion->Wait()` ([RHIThread.h:89](../ZenCore/Include/Graphics/RHI/RHIThread.h)) | Unbounded |
| Queue backpressure | `RHIThread::Dispatch` → `m_space.Wait()` ([RHIThread.cpp:174](../ZenCore/Source/Graphics/RHI/RHIThread.cpp)) | Unbounded |
| Frame submission | `RHISubmissionTicket::Wait` → `m_completion->Wait()` ([RHICommandListExecutor.cpp:159](../ZenCore/Source/Graphics/RHI/RHICommandListExecutor.cpp)); also reached through `ResourceRetirement` and `RenderDevice::PollFrameSubmissions` | Unbounded |
| Shutdown | `RHIThread::Stop` → `WaitForRHIObject(thread handle)` then `join` ([RHIThread.cpp:153](../ZenCore/Source/Graphics/RHI/RHIThread.cpp)); non-Windows joins directly | Unbounded |
| Inside RHI | `VulkanSwapchain` acquire/present fence and queue waits use `UINT64_MAX` ([VulkanSwapchain.cpp](../ZenCore/Source/Graphics/VulkanRHI/VulkanSwapchain.cpp) lines 321, 447, 481, 503) | Unbounded; observable only through the waits above |

All Windows waits funnel into `WaitForRHIObject` (`MsgWaitForMultipleObjectsEx(QS_ALLINPUT)` with an unfiltered `PM_NOREMOVE` peek). Other platforms wait on a condition variable in `RHIThreadEvent::Wait`. The watchdog therefore needs only two wait loops, plus a way for each caller to identify the RHI thread it waits on.

Reference: Unreal Engine's `GameThreadWaitForTask` waits for the render thread in slices of at most 33 ms. After `GTimeoutForBlockOnRenderFence` it calls `HandleRenderTaskHang`. The check is skipped under a debugger or while timeouts are suspended (`Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp` in the Unreal Engine source, around line 1160). UE treats the hang as fatal; this plan reports by default and makes fatal behavior opt-in.

## 2. Required contracts

1. The watchdog never changes wait semantics. A wait still ends only when its object signals. Sent messages are still dispatched immediately; posted messages, input and `WM_QUIT` still stay queued.
2. Slicing the Windows wait must keep the validated message behavior: one unfiltered `PM_NOREMOVE` peek before waiting, a `QS_ALLINPUT` wake mask, and no `MWMO_INPUTAVAILABLE`. `RHIThreadTest.WindowsLongWaitDoesNotMarkWindowThreadHung` and `WindowsWaitDoesNotSpinOnQueuedPostedMessages` must keep passing.
3. Waits that finish within a slice cost nothing extra; frame waits are normally milliseconds.
4. Reports are rate-limited: one at the threshold, then at doubling intervals, plus one recovery line if the wait later completes.
5. Reporting never runs on the RHI thread and never blocks on RHI. It reads only atomics published by the worker.
6. The fatal path is disabled by default. When enabled, it writes the report, flushes the logger and aborts. It never throws from inside a wait, because RenderCore state would unwind while RHI may still be executing a task that references it.
7. Both thresholds are suppressed while a debugger is attached (Windows: `IsDebuggerPresent`), so breakpoints on the RHI thread do not trigger reports.

## 3. Phase 1: stall reports

Estimated size: about half a day; roughly 100–150 lines plus tests.

### Published RHI activity

Add `RHIThreadActivity` to `RHIThread.h`, owned by `RHIThread` and written only by the worker in `RHIThread::Run`:

| Field | Meaning |
| --- | --- |
| `std::atomic<uint64_t> completedTasks` | Incremented after each task returns. |
| `std::atomic<int64_t> taskStartedTicks` | Steady-clock ticks when the current task started; 0 while idle. |
| `std::atomic<uint32_t> queuedTasks` | Queue depth, updated under the existing queue mutex in `Dispatch`, `TryDispatch` and `Run`. |

Expose it with `const RHIThreadActivity& RHIThread::GetActivity() const`.

### Watched waits

Add `struct RHIWaitWatch { const RHIThreadActivity* activity; const char* operation; }`. `operation` is a static string naming the wait. Change `RHIThreadEvent::Wait` to take a watch, and pass one at every call site:

| Caller | Operation label |
| --- | --- |
| `RHIThread::InvokeQueued` | `"synchronous RHI call"` |
| `RHIThread::Dispatch` (full queue) | `"RHI queue space"` |
| `RHISubmissionTicket::Wait` | `"frame submission"`, using `GetRHIThread().GetActivity()`; the executor always uses the global thread |
| `RHIThread::Stop` | `"RHI shutdown"` |

On non-Windows platforms, `Stop` waits on a new `RHIThreadEvent` that `Run` signals on exit, then joins. That makes shutdown observable on every platform.

### Monitor

Add a source-local `RHIStallMonitor` class in `RHIThread.cpp`, created per wait. It records the wait start time and `completedTasks` at the start.

- **Windows wait loop:** replace `INFINITE` with a 250 ms slice. `WAIT_TIMEOUT` calls `monitor.Check()`; message wakes keep calling `ServiceSentMessages()`.
- **Other platforms:** replace the condition-variable `wait` with `wait_for(slice)` in a loop, with the same check.
- **`Check()` behavior:** compares elapsed time with the report deadline (threshold, then 2×, 4×, … capped at 60 s between reports) and the fatal threshold.

Report format, one `LOGE` line:

```
RHI stall: <operation> waited 12.0 s; RHI <state>; tasks completed during wait: N; queued: M
```

`<state>` is one of:
- `running one task for 11.8 s` (no completions since the wait began): the RHI task is not returning;
- `progressing (N tasks completed)`: backlog or slow work;
- `idle`: the waiter is blocked on work that never ran, which indicates an ordering or lost-wakeup bug.

If any report was issued, a `LOGW` recovery line records the total wait when it completes.

### Configuration

Add to `RHIOptions`:

| Option | Default | Notes |
| --- | --- | --- |
| `SetStallReportThreshold(std::chrono::milliseconds)` | 10 s | 0 disables reports. Tests use sub-second values. |
| `SetStallFatalThreshold(std::chrono::milliseconds)` | 0 (disabled) | For automated runs. |

Demo flags: `--rhi-stall-report-seconds=N` and `--rhi-stall-fatal-seconds=N`, parsed next to `--rhi-counters` in [SceneRendererDemo.cpp](../ZenSamples/VulkanRHIDemo/SceneRenderer/SceneRendererDemo.cpp) and added to its usage line.

The fatal path uses `spdlog::critical`, flushes the default logger and calls `std::abort()`.

### Tests (RenderCoreTest, `RHIThreadingTests.inl`)

Capture logs with the existing `ostream_sink_mt` pattern in `RenderCoreTests.cpp`. Restore options and the default logger in each test.

1. **Stuck task:** threshold 200 ms; an `Invoke` task sleeps 700 ms. Expect one report naming `synchronous RHI call` and `running one task`, followed by one recovery line.
2. **Below threshold:** threshold 500 ms; task sleeps 100 ms. Expect no report.
3. **Backlog versus stuck:** capacity 1; dispatch short tasks whose total exceeds the threshold. Expect `progressing`, not `running one task`.
4. **Disabled:** threshold 0 and a long task. Expect no report.
5. **Fatal:** a death test (`threadsafe` style), fatal threshold 200 ms, task sleeps 2 s. Expect abort with the report text.
6. **Existing Windows message tests unchanged**, including both hang-flag guards. Slicing must not re-enable `MWMO_INPUTAVAILABLE` or a filtered peek.

Completion gate: all RenderCore tests pass. Threaded and inline demo smoke runs at the default threshold produce no stall reports.

## 4. Phase 2: RHI call stack in the first report (Windows)

Estimated size: about one day; roughly 150–200 lines.

A report that says "running one task for 11.8 s" still leaves the question of which call. The window-ghosting investigation answered that only by attaching an external sampler. Capture the RHI thread's stack once per stalled wait, on the first report.

Keep platform code out of `RHIThread.cpp`. Add `Platform/ThreadStackCapture.h` with:

- `size_t CaptureThreadStack(void* nativeThread, uintptr_t* frames, size_t capacity)`
- `void FormatThreadStack(const uintptr_t* frames, size_t count, std::string& out)`

Provide a Windows implementation and a no-op elsewhere. `RHIThreadActivity` also publishes the worker's native handle.

Capture in two steps, so the waiter cannot deadlock on a lock the suspended RHI thread holds (loader, heap, logger, dbghelp):

1. **Suspended (no heap, no locks, no logging):** `SuspendThread`, `GetThreadContext`, then unwind with `RtlLookupFunctionEntry` and `RtlVirtualUnwind` into a fixed array of up to 64 return addresses, then `ResumeThread`. Resume is unconditional. Stop the walk on an invalid frame.
2. **After resuming:** symbolize with dbghelp (`SymInitializeW` lazily, once; `SymFromAddr`; `SymGetLineFromAddr64`), serialized by a mutex because dbghelp is single-threaded. When no PDB is available (performance and release presets), fall back to `module+offset` via `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)`.

Link `dbghelp` in the Windows ZenCore build. Add an `RHIOptions` switch to disable capture if a driver or overlay misbehaves when suspended.

Tests:
- An RHI task blocks inside a uniquely named `__declspec(noinline)` function on an event. In Debug, the report contains that function name. The task then completes normally, which proves the thread resumed.
- A capture against an idle RHI thread succeeds and shows `RHIThread::Run`.

## 5. Deferred

| Item | Reason |
| --- | --- |
| Task labels on dispatched work | `Invoke` is variadic, so a defaulted `std::source_location` argument does not fit without touching about 40 call sites. Phase 2 stacks give more precise information. |
| A separate heartbeat thread for all engine threads | Main-thread CPU stalls are already visible through Windows hang detection, now that waits no longer cause false positives. GPU hangs surface as device loss. Revisit only if a stall occurs while no thread waits on RHI. |
| Timeouts on RHI-internal Vulkan waits | Belongs to typed wait results in the [RHI error handling plan](RHIErrorHandlingPlan.md). The watchdog reports these stalls but does not bound them. |

## 6. Risks

| Risk | Mitigation |
| --- | --- |
| False reports from legitimate long RHI work: Debug pipeline creation, `WaitDeviceIdle` during resize or shutdown, swapchain recreation | Generous 10 s default, report-only by default, recovery line, doubling intervals |
| Debugger breakpoints on the RHI thread | Suppress while a debugger is attached |
| Slicing regresses the Windows message fix | Contract 2 and its existing regression tests |
| Phase 2 deadlock while the RHI thread is suspended | Two-step capture; no allocation or locks while suspended; option to disable |
| Fatal mode aborting legitimate long work | Off by default; intended for automated runs with a known workload |

## 7. Validation

| Check | Expectation |
| --- | --- |
| RenderCoreTest | All tests pass, including new stall tests and the existing Windows message tests |
| VulkanRHIIntegrationTest, from an unhooked `7zFM.exe` copy | Unchanged pass/skip counts; no validation errors |
| Demo smoke runs, threaded and inline, default threshold | No stall reports |
| Demo with `--rhi-stall-report-seconds=1 --background-test-seconds=6` | Any reports occur only around swapchain recreation and are followed by recovery lines |
| Phase 2, Debug | Report includes the blocked RHI function and source line |

Follow the ZenEngine C++ rules for all code: one return per function, no `auto`, blank lines between statements in `.cpp` files, engine containers where suitable, and whole-file clang-format 19.1.5 checks on changed files.
