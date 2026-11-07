# ZenApp: New Sponza fullscreen 4K Nsight profile

Rerun on **2026-11-07** to measure compute frustum culling and indirect GBuffer draws at revision `2c70d3d2`. NVIDIA Nsight Graphics launched the actual Release **ZenApp.exe**. New Sponza, the stationary courtyard-center camera, **3840 × 2160 fullscreen framebuffer and render size**, and **async compute enabled and available** match the previous experiment.

The controlled comparison uses **the same executable with `--gpu-culling=0` and `--gpu-culling=1`**. There are two 30-frame captures per Compute GI setting and three per HWRT setting. The second pair reverses the first pair's order; an additional HWRT pair checks higher variability in its second pair. Every capture uses a 60-second startup delay and GPU clocks locked to base. All runs are retained.

## Result

**No GPU FPS improvement was observed from enabling culling and indirect drawing.** Every paired capture was slower with the feature enabled. The pooled GPU-equivalent FPS changes are -1.01% for Compute GI and -1.72% for HWRT. These results apply to this camera and workload.

| Backend | Direct draws, median ms | Culling + indirect, median ms | Direct GPU FPS | Culling + indirect GPU FPS | FPS change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Compute GI | 26.523 | 26.793 | 37.70 | 37.32 | -1.01% |
| HWRT | 17.065 | 17.364 | 58.60 | 57.59 | -1.72% |

**GPU FPS** means `1000 / median GPU frame interval` from Nsight, using all 60 Compute GI or 90 HWRT frame samples per condition. These are instrumented, base-clock GPU measurements, not uninstrumented application FPS or CPU submission timings. The two toggled features are measured together; this experiment does not isolate culling from indirect drawing.

Individual capture medians, in milliseconds:

| Backend / pair | Direct draws | Culling + indirect | GPU FPS change |
| --- | ---: | ---: | ---: |
| Compute GI / 1 | 26.527 | 26.834 | -1.14% |
| Compute GI / 2 | 26.508 | 26.659 | -0.57% |
| HWRT / 1 | 17.065 | 17.364 | -1.72% |
| HWRT / 2 | 17.412 | 18.548 | -6.13% |
| HWRT / 3 | 16.616 | 16.816 | -1.19% |

Capture order: Compute on/off, HWRT off/on, Compute off/on, HWRT on/off, HWRT off/on. The short adjacent frame samples are not independent trials; the per-capture results above expose repeat variability.

The second HWRT pair has wider whole-frame variation, including higher lighting and reconstruction timings. The exported graphics/system/memory clock metrics remain effectively unchanged, and Nsight reports no VRAM demotion; the source of the variation is not established. It limits the precision of the whole-frame percentage. The additional third pair also shows lower GPU FPS with the feature enabled. GBuffer duration rises in every pair.

## Pass timings

Median marker durations from the official Nsight export, pooled over all samples per condition, in milliseconds:

| Marker | Compute direct | Compute culling + indirect | HWRT direct | HWRT culling + indirect |
| --- | ---: | ---: | ---: | ---: |
| `GBufferFrustumCull` | — | 0.0015 | — | 0.0015 |
| `OffScreen` | 1.5436 | 1.7176 | 1.5608 | 1.7019 |
| `ProbeTrace` | 6.9913 | 7.0500 | 1.1986 | 1.2071 |
| `ProbeSpecularTrace` | 4.5118 | 4.5488 | 0.7208 | 0.7241 |
| `ProbeReconstruct` | 4.0700 | 4.0682 | 4.0753 | 4.0727 |
| `SceneLighting` | 4.8298 | 4.8244 | 5.0801 | 5.1255 |
| `ForwardTranslucent` | 2.3455 | 2.3451 | 2.1569 | 2.1616 |
| `ScenePresentation` | 0.0403 | 0.0404 | 0.0404 | 0.0402 |

The culling dispatch itself is about 0.0015 ms. `OffScreen` is the GBuffer pass affected by the new indirect draw path. Its pooled median increases by 0.174 ms (11.3%) for Compute GI and 0.141 ms (9.0%) for HWRT. The toggle also selects indirect shader variants, and the exported counters below show the cost comes from them. GI tracing, reconstruction, lighting, and translucent rendering still account for much larger GPU intervals. Pass intervals may overlap and must not be summed to infer frame time.

All enabled runs report `gpu_indirect` without fallback, and `GBufferFrustumCull` has 30 samples in each. Disabled runs report `direct_disabled` and contain no culling marker. Every exported marker is recorded in every frame. No field-construction pass occurs in the recorded intervals.

### Why `OffScreen` slowed down

`GPUTRACE_REGIMES.xls` shows culling removing work that was already cheap, while the shader change adds work. These are pooled Compute GI medians for the `OffScreen` marker; HWRT matches within a few percent.

| Counter | Direct | Culling + indirect | Change |
| --- | ---: | ---: | ---: |
| `pda__input_prims_realtime.sum` | 3.73M | 1.31M | -64.7% |
| `tpc__warps_launched_shader_vtg_realtime.sum` | 136,030 | 50,034 | -63.2% |
| `tpc__warps_launched_shader_ps.sum` | 438,890 | 425,580 | -3.0% |
| `sm__inst_executed_pipe_uniform_realtime` (% of peak) | 5.93 | 0.0013 | -100% |
| `tpc__warps_issue_stalled_branch_resolving` (per cycle) | 0.19 | 1.67 | +778% |
| `tpc__average_warp_latency_shader_ps` (cycles) | 8,331 | 9,137 | +9.7% |

Vertex work was about 5% of active shader work in this pass, so rejecting 65% of the triangles could save little. Revision `2c70d3d2` also moved the material index from a push constant to a flat varying. The uniform pipe then went unused and branch stalls rose about 8x, which accounts for the increase. That link is inferred from these counters, not isolated by a separate capture.

**These results describe revision `2c70d3d2`.** Later work restores a per-batch material push constant. It has not been measured here, so these numbers do not describe that code.

The culling pass intentionally stays on the graphics queue because the following GBuffer pass consumes its indirect arguments; see [AddGBufferCullPass](../../ZenCore/Source/Graphics/RenderCore/V2/DeferredLightingRenderer.cpp). Global async availability does not imply that this pass overlaps graphics.

## Comparison with the previous ZenApp runs

| Backend | Nov 6 first run GPU FPS | Nov 6 rerun GPU FPS | Nov 7 culling + indirect GPU FPS | Change from Nov 6 rerun |
| --- | ---: | ---: | ---: | ---: |
| Compute GI | 35.08 | 37.13 | 37.32 | +0.52% |
| HWRT | 53.97 | 57.70 | 57.59 | -0.18% |

The setup, scene package and Nsight executable hashes match November 6. The ZenApp executable and available shader set differ. **RTSS was not running for any November 7 capture**, whereas the November 6 captures may include its overlay. Small historical differences cannot be attributed to culling; use the same-build on/off comparison above to evaluate the feature. The [previous report](../../build/nsight-zenapp-4k-culling-20261107/previous-report.md), [November 6 verification](../../build/nsight-zenapp-4k-center-20261106-rerun/verification.json), and [older demo report](NewSponza4KNsightProfile.md) retain their original evidence. Demo captures are not ZenApp results.

## Conditions and verification

- RTX 5080, driver 621.01; Nsight Graphics 2026.3.1 GPU Trace; Vulkan Release build; clocks locked to base.
- Threaded RHI, `--async-compute=1`, `--vsync=0`, `--validation=0`, fixed timestep, UI disabled, GPU markers enabled. Hardware events and multipass metrics disabled; real-time shader profiling enabled by Nsight's default configuration.
- Executable: `E:/Dev/ZenEngine/build/x64-windows-msvc-release/bin/ZenApp.exe`, SHA-256 `3d84176677e7de02ad247280c2beba68f0aaabe7948c7b0f435bea4947a11afb`. The ZenApp build target was checked before profiling; [build log](../../build/nsight-culling-build-20261107.log).
- [Setup](../../build/nsight-zenapp-4k-culling-20261107/sponza.zenrender): `E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf`, prepared package `45805307d6863205.zenasset`, 3,747,018 triangles, imported lights, animation disabled. Setup SHA-256 `782134f41956be916fe5595a1cfe308bc05665e1140cd4cfd3b340c30a43eb47`.
- Normalized camera eye `(-0.051807632, -0.199212402, -0.068445134)`, target `(0.912103148, -0.004898672, 0.113538636)`, vertical FOV 58.5119764 degrees.
- Voxel resolution 128, field resolution 64, probe spacing 16 pixels, adaptive probe placement disabled. All 24 field generations completed before capture in every run; source-log evidence is retained in verification.json.
- Every log confirms `fullscreen=true, framebuffer=3840x2160` and `ZenApp render: 3840x2160, async-compute=available, RHI=threaded`.
- Compute explicitly uses `--disable-rt` and has no ray-query or acceleration-structure extensions enabled. HWRT enables both extensions and reports the hardware ray provider. Backend selection includes direct shadows.
- All ten captures use identical executable, setup, package, Nsight and shader hashes. Each has 30 positive frame intervals, complete marker samples, no application errors, and 0 MiB VRAM demotion reported by Nsight.

`Data/engine.cfg` remained byte-identical, SHA-256 `c79ce520a31c3dfbf7b5d5408c10d45dc4bc41a9463b568520a5c9007213d39f`. The display arrangement was left unchanged. No rendering code was modified for this experiment.

Validation is disabled for performance captures. The November 6 strict synchronization finding is retained as historical evidence in the [previous report](../../build/nsight-zenapp-4k-culling-20261107/previous-report.md) and [migration verification](../ZenAppMigration.md#verification); this performance experiment does not retest it.

## Capture artifacts

| Run | Raw trace | Summary | Exact application command |
| --- | --- | --- | --- |
| compute-on-0 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/compute-on-0/ZenApp_2026_11_07_08_09_19.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/compute-on-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/compute-on-0/application-command.json) |
| compute-off-0 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/compute-off-0/ZenApp_2026_11_07_08_12_48.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/compute-off-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/compute-off-0/application-command.json) |
| hardware-off-0 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-0/ZenApp_2026_11_07_08_15_26.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-0/application-command.json) |
| hardware-on-0 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-0/ZenApp_2026_11_07_08_17_20.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-0/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-0/application-command.json) |
| compute-off-1 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/compute-off-1/ZenApp_2026_11_07_08_19_11.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/compute-off-1/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/compute-off-1/application-command.json) |
| compute-on-1 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/compute-on-1/ZenApp_2026_11_07_08_21_14.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/compute-on-1/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/compute-on-1/application-command.json) |
| hardware-on-1 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-1/ZenApp_2026_11_07_08_23_09.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-1/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-1/application-command.json) |
| hardware-off-1 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-1/ZenApp_2026_11_07_08_24_52.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-1/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-1/application-command.json) |
| hardware-off-2 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-2/ZenApp_2026_11_07_08_27_07.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-2/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-off-2/application-command.json) |
| hardware-on-2 | [GPU Trace](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-2/ZenApp_2026_11_07_08_29_02.ngfx-gputrace) | [JSON](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-2/summary.json) | [Arguments](../../build/nsight-zenapp-4k-culling-20261107/hardware-on-2/application-command.json) |

[Verification and comparisons](../../build/nsight-zenapp-4k-culling-20261107/verification.json) contain actual extents, async and culling states, hashes, log line references, run medians and pooled results. [Analysis script](../../build/nsight-zenapp-4k-culling-20261107/compare.py) reproduces those calculations. Each capture directory retains `launch.log`, `command.json`, `inputs.json`, `setup.zenrender`, the raw trace, and official exports under `BASE/`.

`FRAME.xls` supplies GPU frame intervals; `D3DPERF_EVENTS.xls` supplies marker durations; `REPRO_INFO.xls` supplies capture configuration and executable identity. The helper computes p95 using sorted sample index `floor(0.95 × (N − 1))`. These GPU Trace exports do not provide frame-debugger event IDs.

## Reproduce

Use the same native 4K display and fresh output directories. Run sequentially, with RTSS stopped or ZenApp excluded. Explicitly toggle `--gpu-culling=0` and `--gpu-culling=1`; repeat in reversed order:

```powershell
python tools/profile_hybrid_gi.py --setup=build/nsight-zenapp-4k-culling-20261107/sponza.zenrender --backend=compute --gpu-culling=1 --output=build/nsight-zenapp-compute-on-next --fullscreen --width=3840 --height=2160 --async-compute=1 --start-after-ms=60000 --frames=30 --clocks=base
python tools/profile_hybrid_gi.py --setup=build/nsight-zenapp-4k-culling-20261107/sponza.zenrender --backend=hardware --gpu-culling=1 --output=build/nsight-zenapp-hardware-on-next --fullscreen --width=3840 --height=2160 --async-compute=1 --start-after-ms=60000 --frames=30 --clocks=base
```

ZenApp startup arguments and the sidebar's **Start fullscreen** control are documented in [ZenEditor](../../ZenEditor/README.md).
