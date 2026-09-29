# M8.1: GPU frame timing verification

2026-09-29. **Complete on AMD Radeon RX 7900 XT.** This is the timing-infrastructure
phase of [M8](DynamicVoxelGIImplementationPlan.md#m81-whole-frame-gpu-timing), not final
performance or automatic-promotion acceptance. Windows/MSVC Debug captures below use
Vulkan synchronization validation. NVIDIA and macOS/MoltenVK remain platform followups.

## Measurement and ownership

The GPU frame interval runs from the earliest TOP timestamp to the latest BOTTOM
timestamp across participating native graphics/compute command buffers begun within
the application's render-workload scope. It covers preparation graphs, frame graph
submissions, and the graphics swapchain copy. Dedicated transfer-only buffers are
excluded and counted. Asset startup outside the frame and screenshot readback are
excluded. Host acquire/present calls and compositor/display latency are not measured
directly; dependency waits and CPU submission gaps can contribute to the interval.
This is elapsed time, not summed pass duration, GPU active time, or occupancy.

Vulkan optionally enables `VK_KHR_calibrated_timestamps`, preferring it over EXT when
both are available. This supplies the common device timestamp domain without host-clock
calibration calls. Without either extension, frame timing is explicitly unsupported
and supported per-pass timing remains available. The
[Vulkan query specification](https://docs.vulkan.org/spec/latest/chapters/queries.html)
and [extension rationale](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_calibrated_timestamps.html)
describe the cross-submission timestamp guarantee.

Native intervals reuse the existing query pools and resolve only after normal retirement,
without `WAIT_BIT`, extra GPU waits, or new submission dependencies. A frame retains at
most 256 intervals; the demo retains at most 32 unresolved frames. Recordings remain
tracked until actual native submission or discard, so open and closed-but-unsubmitted
buffers cannot cross accepted frame boundaries. Failed/rejected submissions, invalid
boundaries, query failures, and overflow produce unavailable results rather than partial
durations. Fresh ownership separates replay and command-buffer reuse.

Integer tick arithmetic handles rollover and mixed counter widths, retaining the widest
counter's known bits. Captures must span less than half the narrowest participating
counter range; known ambiguous spans are rejected. A backend must guarantee the intervals
share a clock. The collector never constructs frame time by summing pass durations.

## Verification

| Check | Result |
| --- | --- |
| Build: scene_renderer_demo, RenderCoreTest, VulkanRHITest, VulkanRHIIntegrationTest | Passed |
| RenderCoreTest | 519 passed; 7 existing disabled |
| New aggregate/executor coverage | 21 aggregate + 6 executor tests passed |
| VulkanRHITest | 39 passed |
| VulkanRHIIntegrationTest | 284 passed; 6 capability-dependent skips |
| New native coverage | 6 frame-boundary/queue/lifetime tests, 2 capability tests, and query-pool allocation-failure recovery passed |
| Export verifier | 14 tests passed; existing schema-1 capture remains readable |
| Inline, async off, mode 3 | 15/15 frame timings available; exports verified |
| Inline, async on, mode 3 | 10/10 frame timings available; exports verified |
| Threaded, async off, mode 3 | 10/10 frame timings available; exports verified |
| Threaded, async on, smoke mode changes/resize/revoxelization | 48/48 frame timings available; exports verified |
| Threaded PBR image capture | 3/3 frame timings available; exports verified |
| Profiling off/on PBR images | Byte-identical SHA-256: `B09C910346A84A6A5A55B79BCC16BA94D61CE943D0513E84E71A61E31524934A` |
| Frame/pass consistency cross-check | All 3,084 matching graphics/compute pass durations fit within their frame intervals |
| Validation/lifetimes | No VUID/synchronization errors or reported engine memory leaks |
| C++ conventions | Requested zenengine-cpp-review rules reviewed; clang-format applied and checked |

The tests cover overlap without summation, out-of-order starts, timestamp wraparound,
mixed widths, ambiguous ranges, missing raw ticks, delayed completion, capacity,
unsupported clocks, inline/threaded ordering, rejected/partial submissions, fresh replay
captures, native buffer splits/reuse/discard, and recording boundaries. The native
query observer checks that readback never requests `VK_QUERY_RESULT_WAIT_BIT`.

Artifacts are under `build/m8-1-validation/`; logs and test JSON are `build/m8-1-*.log`
and `build/m8-1-*.json`. Captures retain executable/shader/config fingerprints and their
scope. `envelope-check.json` records the independent pass/frame consistency check.
An identical temporary executable used the machine's existing RTSS exclusion; no
persistent overlay settings changed. `Data/engine.cfg` was unchanged.

## Use and next phase

Existing `--profile=PREFIX` captures now export schema 2. Frame CSV rows include
`gpu_status`, `gpu_frame_ms`, `gpu_intervals`, and `gpu_excluded_intervals`; JSON includes
per-phase median/p95 and unavailable counts. Validate with:

```powershell
python tools/validate_engine_profile.py build/m8-1-validation/inline --require-gpu --require-frame-gpu
```

`--require-frame-gpu` requires valid GPU timing for every requested application frame;
without it, unsupported frame timing remains a valid explicit limitation. See
[EngineProfiling.md](EngineProfiling.md) for the full interpretation contract.

Next is **M8.2: cache readiness and workload diagnostics**. Cache-batch counts still do
not prove GPU cache readiness. These Debug captures use the current cone/PBR/smoke
settings and do not establish DDA initialization-to-ready, optimized performance,
NVIDIA regression resolution, final image-quality acceptance, or automatic promotion.
