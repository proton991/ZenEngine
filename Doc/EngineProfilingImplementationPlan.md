# Portable engine profiling implementation plan

## Objective and scope

Add an opt-in, vendor-independent profiling path for M8 and subsequent renderer work.
Reuse RDGMetrics, the recorded RHI command stream, and Vulkan completion tracking.
The first implementation provides CPU/GPU pass measurements and reproducible files;
vendor tools remain responsible for occupancy, shader analysis, and hardware counters.
No custom timeline UI or cross-queue clock calibration is required for this milestone.

## Implementation sequence

1. **RHI timing contract and Vulkan backend.** Record optional begin/end GPU timing
   commands with shared result ownership. Unsupported backends publish an explicit
   status. Vulkan checks the actual queue family's timestamp support, converts ticks
   with timestampPeriod, and handles timestampValidBits wraparound. Allocate bounded
   timestamp storage lazily; exhaustion drops samples without blocking rendering.
   Native command buffer completion resolves results before query reuse. Discard,
   command rollback, replay, and shutdown must preserve correct ownership. Readback
   must not use WAIT_BIT, device idle, or introduce new per-frame GPU waits.
2. **RDG integration.** Add opt-in GPU timing to existing sampled node metrics.
   Scopes enclose each node's prologue barriers and execution. Preserve immediate CPU
   reporting and provide a separate deferred GPU-complete sink with owned snapshots.
   Bound pending snapshots and node details, record dropped/unavailable states, and
   retain execution, application frame, node, submission-group, and queue identity.
   Each graph execution creates fresh timing results, including graph replay.
3. **Demo capture and exports.** Add a command-line profiling output option, enable
   the required RDG detail collection, and export CPU frame data plus resolved pass
   measurements. Include schema version, device/driver/build and effective settings,
   scene identity, warmup policy, raw samples, and median/p95 summaries. Separate
   startup/warmup from steady state. Drain completed measurements at ordinary shutdown.
   Expose VSync control for reproducible throughput measurements. Keep file I/O outside
   timed rendering work where practical, and bound capture storage explicitly.
4. **Verification and documentation.** Add meaningful CPU tests for result state,
   timestamp conversion, deferred ownership/replay, and dropped/unsupported samples.
   Exercise native Vulkan timing with validation, including completion, discard, and
   supported queue types. Build and run a short profiled scene_renderer_demo capture,
   validate its exports, and run affected existing suites. Document usage and limits.

## Measurement contract

- GPU values are elapsed pass intervals, including prologue barriers and any pipeline
  overlap within the timestamp endpoints; they are not exclusive shader occupancy.
- CPU recording, CPU submission/backpressure, application frame wall time, and GPU
  elapsed time are separate quantities. Never substitute CPU time for unsupported GPU
  time or encode missing samples as measured zero.
- Preserve physical queue equivalence. Do not sum overlapping queue/pass durations
  into a GPU frame time or claim an aligned cross-queue timeline without calibration.
- Existing allocator/pool byte counters describe their documented accounting scope,
  not complete hardware residency. Runtime GPU work counts requiring new readbacks
  are outside this implementation.
- Timing support and granularity must be verified on each platform. Local validation
  covers the available AMD device; NVIDIA and macOS/MoltenVK remain platform followups.
- Dedicated transfer-only queues report unsupported in this first version: command
  buffer query reset requires a graphics/compute-capable queue in this engine. Timing
  transfer-only queues would require an enabled host-query-reset feature or additional
  cross-queue synchronization. Transfer work on graphics/compute families remains timed.
- Profiling defaults off. Enabling it has instrumentation overhead; benchmark reports
  must identify instrumented runs and use matched settings when comparing captures.

## Acceptance criteria

- Existing execution and tests retain their behavior when profiling is disabled.
- A captured supported Vulkan pass eventually has a finite, nonnegative duration,
  and an unsupported/discarded/overflowed pass has a named status.
- Native query resources cannot be recycled while submitted work still references them.
- Recording or submission failures cannot leave unbounded retained snapshots.
- Captures are inspectable without Nsight, identify their workload/configuration, and
  report meaningful per-pass distributions without inventing a GPU frame total.
- Validation reports no new query, synchronization, or lifetime errors.

## Progress

- [x] Plan recorded before implementation.
- [x] RHI timing results/commands and bounded Vulkan query pools with completion,
  discard, error, unsupported-queue, and counter-wrap handling.
- [x] Opt-in RDG instrumentation, frame identity, deferred snapshots/sink, and bounded
  collection that preserves existing CPU reporting and recording rollback.
- [x] Demo profiling/VSync options, bounded CSV/JSON capture, configuration snapshots,
  device/build/input identity, phase separation, and median/p95 summaries.
- [x] Unit/native tests, real demo captures, export verification, and usage documentation.

## Verification (2026-09-29)

Windows/MSVC Debug, AMD Radeon RX 7900 XT. These are correctness captures, not optimized
M8 performance acceptance measurements. Vulkan synchronization validation was enabled;
the existing local RTSS exclusion was used with an identical temporary executable.
No persistent overlay settings or engine configuration were changed.

| Check | Result |
| --- | --- |
| Build: scene_renderer_demo, RenderCoreTest, VulkanRHITest, VulkanRHIIntegrationTest | Passed |
| RenderCoreTest | 492 passed, 7 existing disabled |
| VulkanRHITest | 39 passed |
| VulkanRHIIntegrationTest | 275 passed, 6 capability-dependent skips; all 5 new timing tests passed |
| Export verifier regression tests | 6 passed |
| Threaded/async 48-frame smoke (mode changes, resize, revoxelization) | 1,351 available GPU samples, 4 unsupported transfer samples; exports verified |
| Inline, async disabled; 3 warmup + 12 requested frames | 794 available GPU samples, 4 unsupported transfer samples; exports verified |
| Matched 3-frame PBR capture with profiling off/on | Byte-identical images; profiled export verified |
| Unwritable output path / profiling without finite frames | Both correctly return nonzero |
| Validation and lifetime checks | No VUID/synchronization errors or reported engine memory leaks |

Build/test logs and JSON reports are under `build/engine-profile-*`; exported examples
are under `build/engine-profile-validation/`. The implementation includes
`tools/validate_engine_profile.py` to independently recompute summary statistics and
reject mixed captures, truncated node details, missing/duplicate pass rows, invalid
timings, and dropped/error measurements.

Usage and interpretation are in [EngineProfiling.md](EngineProfiling.md). Validation on
NVIDIA and macOS/MoltenVK remains a platform followup. Dedicated transfer-queue timing,
calibrated multi-queue timelines, and an interactive viewer remain future extensions.
At this foundation checkpoint, the final M8 workload/performance matrix and promotion
decision were still open. The subsequent [final M8 verification](DynamicVoxelGIM8FinalVerification.md)
closes them for the stated AMD scope with non-promotion; NVIDIA hardware gates remain open.

## M8.1 followup: whole-frame GPU timing (2026-09-29)

The first milestone above remains the per-pass foundation. M8.1 now adds a separately
scoped frame interval across native graphics/compute command buffers and submissions,
including the graphics swapchain copy. Optional KHR/EXT calibrated-timestamp support
provides the common device time domain; unsupported devices retain per-pass reporting.
This adds no CPU-clock calibration, GPU waits, or submission dependencies.

Schema 2 exports raw frame durations/statuses and per-phase median/p95. Native recording
boundaries, wraparound, mixed counter widths, bounded ownership, and asynchronous
submission failure are checked explicitly. See [EngineProfiling.md](EngineProfiling.md)
for scope and [M8.1 verification](DynamicVoxelGIM81Verification.md) for current evidence.
At this M8.1 checkpoint, cache readiness and the reproducible benchmark runner were still
pending as M8.2 and M8.3. Both are now complete in the [final M8 verification](DynamicVoxelGIM8FinalVerification.md),
including schema-3 readiness telemetry and input identity validation.

## Review followup (2026-09-30)

The benchmark runner now compares captured executable, configuration, scene-document
and complete shader-set paths, sizes and FNV-1a fingerprints with inputs frozen before
launch. The separate SHA-256 manifest and external asset checks remain in place.
Mismatched scenes or shader directories, unreadable files, incomplete enumeration,
duplicate entries and changed content are rejected before a trial can be accepted.
New C++ profiler code and tests use explicit types, and historical plan checkpoints
are distinguished from the completed AMD M8 scope.

Verification used the rebuilt MSVC performance targets on RX 7900 XT:

- All five affected targets built: demo, RenderCore, RHI, Vulkan integration and native GI.
- RenderCore: 519 passed, 7 existing disabled; RHI: 39 passed.
- Vulkan integration: 285 passed, 6 capability skips, with synchronization validation
  and the byte-identical executable alias using the existing RTSS exclusion.
- Cone visibility: all four submission modes passed 196,680 scalar/vector query comparisons.
- Python: 19 export-verifier tests and 15 runner tests passed.
- A real cone trial passed cold/profile/throughput capture and input verification.
  Separate real runs rejected an overridden scene and a different shader directory
  containing identical shader bytes. All runs restored the original configuration.
- Formatting and Git whitespace checks passed; generated Python caches are ignored.

Logs, test reports and capture manifests are under `build/review-fixes/`. These checks
validate the review fixes; the dated M8 and environment-visibility performance reports
retain their original frozen binary identities and measurement scope.
