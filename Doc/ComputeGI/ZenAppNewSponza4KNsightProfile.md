# ZenApp: New Sponza fullscreen 4K Nsight profile

Rerun on **2026-11-06**, after removing the legacy scene demo. NVIDIA Nsight Graphics launched the actual Release **ZenApp.exe** with explicit startup arguments. Both backends used New Sponza with a stationary camera at the courtyard center, a **3840 × 2160 fullscreen framebuffer and render size**, and **async compute enabled and available**.

One 30-frame capture per backend, after a 60-second startup delay, with GPU clocks locked to base:

| GPU frame timing | Compute GI | HWRT |
| --- | ---: | ---: |
| Median | 26.932 ms | 17.332 ms |
| p95 | 27.034 ms | 17.394 ms |
| Mean | 26.917 ms | 17.248 ms |

HWRT's median GPU frame interval is **35.6% lower**, corresponding to **1.55×** reciprocal frame throughput under these capture conditions. This compares complete backend configurations: `--backend` selects all feature providers, including direct shadows.

## Pass timings

Verified marker timings from Nsight's official export, in milliseconds:

| Marker median | Compute GI | HWRT |
| --- | ---: | ---: |
| `ProbeTrace` | 6.987 | 1.397 |
| `ProbeSpecularTrace` | 4.732 | 0.722 |
| `ProbeReconstruct` | 4.070 | 4.053 |
| `SceneLighting` | 4.819 | 5.072 |
| `ForwardTranslucent` | 2.327 | 2.158 |
| `OffScreen` | 1.777 | 1.782 |
| `ScenePresentation` | 0.041 | 0.040 |

The largest measured pass differences are in `ProbeTrace` and `ProbeSpecularTrace`. Pass intervals can overlap and must not be summed to infer frame time. Every exported marker has 30 recorded samples; no field-construction pass appears in either recorded interval.

## Comparison with the previous ZenApp run

| Backend | Previous median | Rerun median | Change |
| --- | ---: | ---: | ---: |
| [Compute GI](../../build/nsight-zenapp-4k-center-20261106/compute-0/summary.json) | 28.509 ms | 26.932 ms | -5.5% |
| [HWRT](../../build/nsight-zenapp-4k-center-20261106/hardware-0/summary.json) | 18.529 ms | 17.332 ms | -6.5% |

The executable, scene package, setup, shader set, and Nsight executable hashes match the previous run. Each result is a single 30-frame capture; the lower rerun timings are not evidence that removing the demo optimized ZenApp. The [older demo report](NewSponza4KNsightProfile.md) records a different executable and is retained as historical evidence.

## Conditions and verification

- RTX 5080, driver 621.01; Nsight Graphics 2026.3.1 GPU Trace; Vulkan Release build; clocks locked to base.
- Threaded RHI, `--async-compute=1`, `--vsync=0`, `--validation=0`, fixed timestep, UI disabled, GPU markers enabled. Hardware events and multipass metrics disabled; real-time shader profiling enabled by Nsight's default configuration.
- Executable: `E:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/ZenApp.exe`, SHA-256 `76066afb8fb00ab6893881183e528d7935745d02010a6251abb18412e6c4db67`. No renamed executable was used.
- [Setup](../../build/nsight-zenapp-4k-center-20261106-rerun/sponza.zenrender): `E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf`, prepared package `45805307d6863205.zenasset`, 3,747,018 triangles, imported lights, animation disabled.
- RTSS runs on this machine and hooks `ZenApp.exe` unless excluded, so these captures may include its overlay work. Repeat them with `ZenApp.exe` excluded before reading small differences as engine cost.
- Normalized camera eye `(-0.051807632, -0.199212402, -0.068445134)`, target `(0.912103148, -0.004898672, 0.113538636)`, vertical FOV 58.5119764 degrees. The setup is byte-identical to the previous run.
- Voxel resolution 128, field resolution 64, probe spacing 16 pixels, adaptive probe placement disabled. All 24 field generations completed before capture: 18:49:45.890 for Compute GI and 18:52:54.699 for HWRT, local time.
- Logs confirm `ZenApp presentation: fullscreen=true, framebuffer=3840x2160` and `ZenApp render: 3840x2160, async-compute=available, RHI=threaded`. This establishes the enabled/available policy; it does not establish that every compute pass overlaps graphics.
- Compute explicitly uses `--disable-rt`; its log has neither ray-query nor acceleration-structure extensions enabled. HWRT enables both extensions and reports the hardware ray provider.
- Capture commands exited successfully, all 30 GPU frame samples are positive, all expected markers are present, and neither capture log contains application errors.

`Data/engine.cfg` remained byte-identical, SHA-256 `c79ce520a31c3dfbf7b5d5408c10d45dc4bc41a9463b568520a5c9007213d39f`. The display arrangement was left unchanged as requested.

Validation is disabled for these performance captures. The separate strict synchronization check still reports `SYNC-HAZARD-PRESENT-AFTER-WRITE` in the unchanged ZenApp binary; see the [migration verification](../ZenAppMigration.md#verification). These timings do not resolve that issue. Earlier fullscreen UI and launch tests are retained in [checks.json](../../build/zenapp-fullscreen-check/checks.json).

## Capture artifacts

| Backend | Raw trace | Summary | Exact application command | Launch log |
| --- | --- | --- | --- | --- |
| Compute GI | [GPU Trace](../../build/nsight-zenapp-4k-center-20261106-rerun/compute-0/ZenApp_2026_11_06_18_50_58.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-center-20261106-rerun/compute-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-center-20261106-rerun/compute-0/application-command.json) | [Log](../../build/nsight-zenapp-4k-center-20261106-rerun/compute-0/launch.log) |
| HWRT | [GPU Trace](../../build/nsight-zenapp-4k-center-20261106-rerun/hardware-0/ZenApp_2026_11_06_18_54_04.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-center-20261106-rerun/hardware-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-center-20261106-rerun/hardware-0/application-command.json) | [Log](../../build/nsight-zenapp-4k-center-20261106-rerun/hardware-0/launch.log) |

[Verification and comparison data](../../build/nsight-zenapp-4k-center-20261106-rerun/verification.json) records the actual extents, async state, source-log lines, and hash comparisons. Each capture directory also retains `command.json`, `inputs.json`, its copied setup, and official exports under `BASE/`.

`FRAME.xls` supplies GPU frame intervals; `D3DPERF_EVENTS.xls` supplies marker durations; `REPRO_INFO.xls` supplies capture configuration and executable identity. The helper computes p95 using sorted sample index `floor(0.95 × (N − 1))`. These GPU Trace exports do not provide frame-debugger event IDs.

## Reproduce

Use a native 4K display and fresh output directories:

```powershell
python tools/profile_hybrid_gi.py --setup=build/nsight-zenapp-4k-center-20261106-rerun/sponza.zenrender --backend=compute --output=build/nsight-zenapp-compute-next --fullscreen --width=3840 --height=2160 --async-compute=1 --start-after-ms=60000 --frames=30 --clocks=base
python tools/profile_hybrid_gi.py --setup=build/nsight-zenapp-4k-center-20261106-rerun/sponza.zenrender --backend=hardware --output=build/nsight-zenapp-hardware-next --fullscreen --width=3840 --height=2160 --async-compute=1 --start-after-ms=60000 --frames=30 --clocks=base
```

ZenApp startup arguments and the sidebar's **Start fullscreen** control are documented in [ZenEditor](../../ZenEditor/README.md).
