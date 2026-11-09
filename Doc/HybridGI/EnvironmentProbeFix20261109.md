# Environment lighting in screen probes — 9 November 2026

The HW RT investigation found two large errors in bright-sky scenes: generated environment cubemaps clipped HDR sun texels to half-float range, and uniformly sampled screen probes rarely observed those texels. Importance sampling removed most of the temporal variation, but interpolating the sun's visibility between probes still blurred its shadows. The implemented fix preserves HDR energy, importance-samples the broader sky in probes, and traces compact bright sources at each receiver.

The implementation changes are:

- Generated prefiltered cubemaps use RGBA32F. The low-sun capture now retains a peak of 133,697.5 instead of 65,504. Authored cubemaps retain their authored formats.
- Each 64-ray probe batch uses 32 residual-environment samples and 32 uniform-hemisphere samples, with their common mixture density. Uniform samples preserve support for bounce, visibility and grazing SH directions. Environment samples below the hemisphere are completed zero contributions; unavailable traversal samples remain distinct.
- The existing environment source classification separates up to four compact bright tiles. `ProbeSkySources` traces two samples per source per pixel, using the selected sky provider and shading/geometric hemispheres. Source RGB is excluded from probe integration, so it is counted once. Source visibility is resolved at the receiver and is not multiplied by the probes' approximate contact visibility.
- Source lighting has an independent history, capped at 64 frames and by the configured history limit; responsive lighting caps it at two. It reuses the existing surface, position, normal and motion rejection. Missing samples retain valid history without advancing its length. Probe/contact and bounce pixel histories retain their short limits.
- The source history uses RGBA32F. An intermediate RGBA16F history produced about 1% mean darkening despite identical raw samples: small EMA updates stopped advancing stored values. Regression coverage checks updates smaller than a half-float ULP. The public sky texture still contains the combined signal; reconstruction removes the previous source contribution before separately accumulating the probe residual.
- Review follow-up (not yet re-measured): the sky pixel history and the raw sky texture also use RGBA32F, because the residual is recovered by subtracting a source that is often much brighter, and a half-float sum keeps only about 0.1% of that sum's precision. A missing source sample no longer marks the probe sample missing; only the source history skips it. Reprojected source history averages only neighbours that have source history.

The new source pass has HW RT, distance-field and mixed variants. All variants compile; the scene accuracy and timing results below qualify **HW RT only**. No automatic switch to dense per-pixel GI is introduced. Probe spacing remains 16 and the existing probe ray budget remains 64 per batch; the source rays are additional work.

Verification used the new Sponza model, the immutable `newsponza/v2` dataset, 960×540 rendering, normal maps disabled, all feature providers set to hardware, and a 128³ radiance cache. Twenty native configurations completed with verified setup, executable, shader, source and capture identities. The source-only reference evaluates the original HDR and glTF, not engine-generated lighting. For the three views below, it was refined to 65,536 samples at 32,768 matched receiver pixels. RMS subtracts the independently estimated reference variance before normalization.

| View | Before sky RMS | Final sky RMS | Before mean bias | Final mean bias |
|---|---:|---:|---:|---:|
| C1_L1, high-sun atrium | 47.04% | 3.53% | −2.68% | −0.30% |
| C3_L2, low-sun gallery | 79.81% | 0.64% estimate | −42.25% | +0.37% |
| C4_L2, dim interior | 8.04% | 9.30% | −0.35% | −0.35% |

The final C3_L2 error is approximately the reference noise, so 0.64% is an estimate, not a tightly qualified error bound. C1_L1 meets the workflow's reference-noise-below-one-quarter-error criterion. C4_L2 remains slightly worse in this captured frame because of rare source samples: absolute RMS changes from 0.000338 to 0.000391. Giving sources their own history reduced the initial two-ray/four-frame version's 21.04% RMS to 9.30%; it does not eliminate sampling variance.

Stationary sky RMS change over 16 frames fell from 0.02826 to 0.000472 in C1_L1 and from 0.01271 to 0.000297 in C3_L2, about 60× and 43× respectively. Final captures passed the stability gate at 160 settled frames; the two baseline views required 224. This measures those capture pairs, not a universal convergence time or a camera-motion qualification. The native tests cover reprojection, foreign surfaces, history resets, missing observations, source precision and fused/separate reconstruction; a production camera-motion sequence was not captured.

The soft-sky C1_L3 result is essentially unchanged (approximately 18.6% sky RMS). Bounce-cache errors also remain: the independent 4,096-sample checks still show approximately −14% mean bounce bias in C1_L1 and −18% in C3_L2 after restoring HDR energy. This change does not replace the radiance cache or solve the remaining spatial reconstruction errors.

Performance was captured through standalone Release **ZenApp.exe** using `tools/profile_hybrid_gi.py`: RTX 5080, driver 621.14, windowed 960×540 framebuffer and render extent, base GPU clocks, threaded RHI, async compute available, GPU culling on, VSync/UI/validation/RTSS off. Each capture contains 30 frames after a 30-second startup delay. The before/after configurations are hash-identical and each timing build matches its corresponding quality run. The values are instrumented GPU frame intervals, not application FPS.

| View | Before frame median | Final frame median | Before ProbeTrace | Final ProbeTrace | New ProbeSkySources | Before/after ProbeReconstruct |
|---|---:|---:|---:|---:|---:|---:|
| C1_L1 | 4.370 ms | 4.670 ms | 0.225 ms | 0.289 ms | 0.215 ms | 0.304 / 0.316 ms |
| C3_L2 | 4.543 ms | 5.710 ms | 0.109 ms | 0.152 ms | 0.245 ms | 0.320 / 0.333 ms |

Pass durations must not be summed into full-frame time. A repeated final C3_L2 capture measured **3.736 ms** median and **4.110 ms** p95, versus 5.710 / 6.723 ms in the first. ProbeTrace stayed at 0.152 ms, ProbeSkySources at 0.245 ms, and ProbeReconstruct at 0.333–0.334 ms. The frame intervals therefore show substantial variability beyond the measured GI passes. Both captures are retained; no full-frame speedup or fixed 1.17 ms regression is inferred from these short samples. The source pass cost itself is consistently 0.215–0.245 ms in these views.

Logical allocation increases are approximately 16 MiB per generated 512² environment mip chain plus 2 MiB for the shared preprocessing target. Per view, the source pass adds one RGBA16F transient and two RGBA32F history textures: approximately 19.8 MiB at 960×540. The follow-up's RGBA32F sky history adds another 7.9 MiB at 960×540; the RGBA32F raw sky is a transient that normal rendering culls. These are allocation sizes, not a measurement of device peak memory.

Release ZenApp, all affected shaders, and ConeVoxelGIIntegrationTest build successfully. All **52 ScreenProbeGatherIntegrationTest cases** pass across the four submission modes with no Vulkan validation errors or reported leaks. Formatting and whitespace checks pass. Existing unrelated working-tree changes were preserved.

Evidence and reproduction:

- [Final native run](E:/Dev/ZenGroundTruth/runs/20261109-env-probes-v4/run.json), [full 20-view report](E:/Dev/ZenGroundTruth/runs/20261109-env-probes-v4/report.json).
- [Combined measurements](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-fix-results.json), [analysis script](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env_probe_fix_analyze.py), [refined reference provenance](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/refined-sky-reference-provenance.json).
- [Sky-only visual comparison](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-sky-comparison.png). Images show the original full-frame 256-sample reference; the table above uses the refined reference points.
- [Native test results](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-tests-v4.xml), [C1_L1 trace summary](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-performance-final/C1_L1/summary.json), [C3_L2 trace summary](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-performance-final/C3_L2/summary.json), [C3_L2 repeat](E:/Dev/ZenGroundTruth/investigations/20261109-hwrt/env-probe-performance-final/C3_L2_repeat/summary.json).

The measurement directory retains intermediate captures that isolate importance sampling, receiver source visibility, and source-history precision. They are diagnostic ablations; `20261109-env-probes-v4` is the final quality run. No baseline acceptance threshold was changed.
