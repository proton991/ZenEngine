# Engine profiling

The portable profiler measures CPU frame/recording work, Vulkan GPU elapsed time
for RDG passes, and a graphics/compute GPU frame interval. It defaults off and does not require a vendor profiler or debug-marker
extension. The implementation plan is in
[EngineProfilingImplementationPlan.md](EngineProfilingImplementationPlan.md), and the
collector API is documented in [RDGMetrics.md](RDGMetrics.md#portable-gpu-pass-timing).

## Capture a benchmark

Use an optimized build for performance conclusions. From the repository root:

```powershell
.\build\x64-windows-msvc-release\bin\scene_renderer_demo.exe --mode=3 --frames=180 --warmup=30 --fixed-step --vsync=0 --disable-validation --profile=build/profiles/cone
```

`--profile=PREFIX` requires a finite, nonzero frame count (or the finite smoke-test
mode). `--warmup=N` separates warmup from measured frames. The first rendered frame
has its own cold phase; without warmup it is excluded from the measured distribution.
`--fixed-step` fixes light/motion animation steps; elapsed GI temporal filtering still
uses its renderer clock. `--vsync=0|1` controls the viewport presentation request. GPU and
presentation behavior still depend on the platform and selected present mode; turning
off VSync does not guarantee immediate presentation on every implementation.

For a correctness run, keep validation enabled by omitting `--disable-validation`.
The existing `--rhi-thread=0|1`, `--async-compute=0|1`, `--width`, `--height`, and
`--gbuffer-size` controls can be combined with profiling. Select the workload and GI
settings through the ordinary engine configuration. Match those settings, build type,
validation state, scene, camera, and capture options between comparisons.

The output files are:

- `PREFIX.frames.csv`: application frame CPU measurements, GPU frame status/duration,
  included/excluded native interval counts, and phase identity.
- `PREFIX.passes.csv`: resolved per-pass GPU status/duration, CPU metrics, graph,
  frame, node, submission, and queue identity.
- `PREFIX.profile.json`: capture metadata, settings, accounting limits, and summaries
  including median/p95 distributions. Configuration and executable/shader fingerprints
  identify the captured inputs; see the manifest for their exact scope and algorithm.
- `PREFIX.config.cfg`: the configuration bytes saved at capture startup.

Check a capture's file consistency, frame/graph identity, availability semantics, and
recomputed statistics with the dependency-free verifier:

```powershell
python tools/validate_engine_profile.py build/profiles/cone --require-gpu
```

Add `--require-frame-gpu` to require available whole-frame GPU measurements. Schema 2
adds `gpu_status`, `gpu_frame_ms`, `gpu_intervals`, and `gpu_excluded_intervals` to
frame rows, with per-phase `gpu_frame_ms` statistics and `gpu_frame_unavailable` counts
in JSON. Unavailable durations are empty CSV fields, never measured zeros. The verifier
can still read schema-1 captures, but they cannot satisfy `--require-frame-gpu`.

Omit `--require-gpu` when checking a capture from a backend without GPU timestamp
support. The verifier rejects truncated node details, missing/duplicate pass rows, and
dropped/discarded/error timings; explicitly unsupported timings remain valid records.
The input fingerprints use FNV-1a 64-bit as a change detector, not a security
signature. Scene hashing covers the main scene document; external buffers and textures
are outside that fingerprint. The manifest records this limitation explicitly.

File output occurs after capture rather than inside each timed pass. Finite storage
limits and any omitted/dropped samples are part of the report. A capture with missing
samples is diagnostic evidence, not a complete timing aggregate. Keep all outputs from
one run together; never merge partial captures from different binaries or settings.

## Interpretation

CPU frame wall time includes the work and backpressure measured by the demo loop.
CPU pass recording and submission time are separate quantities. GPU pass time spans
the node's prologue barriers and execution, from a top-of-pipe timestamp to a
bottom-of-pipe timestamp. It can include pipeline overlap and waits; it is not exclusive
shader execution time. Startup graphs, warmup frames, and measured frames must be
considered separately. The first rendered frame is also useful as a cold-frame sample,
but does not represent all device/asset initialization.

Only `available` GPU samples have valid durations. `unsupported` means the queue or
backend cannot provide timing; `dropped` means a capture/query capacity was exceeded
or a pending capture was abandoned. `discarded` denotes unsubmitted/incomplete work,
and `error` denotes an invalid scope or failed native measurement. Missing timings are
not zero-cost passes. Very short valid intervals can be zero at the device's resolution.

Native timestamps resolve after normal command-buffer completion without a profiler
GPU wait. Storage is capped at 512 scopes per command buffer and 256 lazily allocated
native pools per device. Completed command buffers return their pools to a device-owned
cache, so retired startup recordings cannot starve later frames. Pool reuse still resets
queries in the next recording; device shutdown destroys the cached pools. Deferred RDG
snapshots also have an independent bound.
The application drains work at normal shutdown before exporting the final results.

Pass intervals can overlap, including pipeline overlap on one queue. Do not sum them
into GPU frame time. Physical queue IDs identify shared native queues; individual pass
duration alone does not establish an aligned timeline or async overlap.

The GPU frame measurement is the interval from the earliest top-of-pipe timestamp to
the latest bottom-of-pipe timestamp across native command buffers begun during the
application's render-workload scope. It includes participating graphics/compute work
across graphs and submissions, including the graphics-queue swapchain copy. It can
include queue waits and CPU submission gaps; it is neither summed pass time nor GPU
active time. Overlap between consecutive frames is possible.

Dedicated transfer-only command buffers are excluded and counted. Their dependency
waits can still contribute to the elapsed graphics/compute interval. Asset initialization
outside the frame, image acquisition itself, presentation/compositor/display latency,
and post-run screenshot readback are outside the metric. The first frame is not a
measurement of complete application startup or GI initialization-to-ready.

Frame timing requires enabled `VK_KHR_calibrated_timestamps` or its EXT fallback to
guarantee comparable device timestamps across submissions and queues. No CPU-clock
calibration calls are needed. Without that capability, frame timing is `unsupported`
while supported per-pass timing remains available. See the
[Vulkan timestamp guarantees](https://docs.vulkan.org/spec/latest/chapters/queries.html)
and [extension rationale](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html).

Native frame intervals share the existing bounded query pools and resolve after normal
completion, without added submissions, semaphore waits, or GPU-idle calls. A frame owns
at most 256 intervals; the demo retains at most 32 pending frames. Overflow, incomplete
scope boundaries, failed submissions, discarded buffers, or query errors invalidate the
frame measurement. Raw integer ticks preserve precision and handle wraparound; the frame
must fit within half of the narrowest participating timestamp counter's range. Timings
are collected into immutable frame records before export.

Reported RDG resource bytes describe logical/pool payload accounting. They exclude
allocator alignment, external resources, driver allocations, and swapchain memory.
The existing `--gpu-memory-stats` VMA diagnostics remain useful for allocator commitment
peaks; neither measurement establishes complete device memory residency.

## Cone-only profile schema (2026-09-30)

Schema 4 retains CPU/GPU frame timings, pass timings, frame wall intervals, resource accounting and input identities. `gi_method` is `cone` or `none`, according to the rendered mode. Directional cache/readiness telemetry, its readback pool and the specialized benchmark runner were removed with Dynamic Voxel GI.

Use `python tools/validate_engine_profile.py PREFIX --require-gpu --require-frame-gpu` to require completed timings. The verifier checks monotonic frame wall intervals for schema 4. It accepts older timing schemas for historical analysis but no longer verifies their retired directional-GI readiness fields. The former `--require-ready-gi` option is removed.

## Platform and tool boundaries

The Vulkan backend checks the actual queue family's timestamp bit count and device
timestamp period. Counter wraparound is handled for scopes shorter than one wrap.
This implementation resets queries with `vkCmdResetQueryPool`, so it times graphics
and compute-capable queue families. Dedicated transfer-only queues report unsupported;
transfer passes routed to a graphics/compute family can still be timed. Vulkan does
not permit [command-buffer query reset on transfer-only queues](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdResetQueryPool.html).
Supporting those queues later requires an enabled host-query-reset feature or a
separate synchronized reset path.
macOS uses the same capability checks through MoltenVK; timing fidelity still needs
verification on the target machine. Unsupported backends retain CPU reporting.

Instrumentation has CPU/GPU overhead, especially in Debug or with validation. Compare
matched instrumented runs for pass analysis, and use uninstrumented runs to confirm
final application throughput. Use Nsight Graphics, Radeon GPU Profiler, or platform
tools when shader occupancy, bandwidth, cache behavior, or hardware counters are needed.

The [M8 final verification](DynamicVoxelGIM8FinalVerification.md) and [AMD measurements](DynamicVoxelGIM8Measurements.md) archive schema 3 measurements on the retired directional candidate, including readiness, fallback, pool reuse, 210 unique profile runs and profiling-on/off image equivalence. NVIDIA and MoltenVK timing validation remain open.
