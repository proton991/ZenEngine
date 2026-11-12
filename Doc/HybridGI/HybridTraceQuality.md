# Matched 4K Hybrid GI quality — 2026-11-12

Fresh quality measurements for the quarter/half presets in
[HybridTraceProfile.md](HybridTraceProfile.md).
Half GI lowers pooled five-view image RMS by 15.2% and mean camera-motion error by 34.1%.
The corresponding earlier fullscreen frame medians were 19.33–20.48 ms for quarter GI and
32.82–33.15 ms for half GI. These quality captures do not supply new performance timings.

These numbers describe the pre-stage-4 default. Stage 4 (2026-11-12) changed the per-pixel ReSTIR
trace; its matched results against this reference (pooled relative RMS 5.261% / 4.503%, motion
0.3564 / 0.2422 /255) are in the stage 4 section of [HybridTraceProfile.md](HybridTraceProfile.md).

The accuracy reference is a high-sample, unfiltered native-resolution **ZenApp renderer reference**.
It shares the engine's material shading and lighting contract; it is not an independent Mitsuba
or multi-bounce path-tracer reference. Accuracy is measured on a stratified sample of native 4K
pixels. Motion and spark measurements cover the full 4K image.

## Configuration and evidence

- RTX 5080; standalone ZenApp; render and framebuffer both 3840x2160, fullscreen.
- BC New Sponza, normal maps on, all six GI features on HW RT, pixel gather, four diffuse rays,
  ReSTIR GI/spatial/rough-reflection reuse, environment lighting off, fixed timestep, no animation.
- Threaded RHI, async compute available, GPU indirect culling, validation enabled for captures.
- Original setups: `build/hybridtrace-optimization/verify-20261112/{quarter,half}.zenrender`.
  They differ only in `pixelResolutionDivisor`: 4 (960x540 GI) and 2 (1920x1080 GI).
- Center is the profiled camera. C2–C5 use the eye/target positions from `newsponza/v3`, retaining
  the profiled projection, materials and lighting. No old quality images are used as references.
- Evidence: `E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2/`.
  `report-refined.json` is the final static/motion report; `sparks/report.json` contains spark counts.
  `identity.json`, per-job `capture.json`, setups and image sidecars retain executable/input/shader
  hashes, exact startup arguments and actual rendering state. Lossless PNGs retain the captured pixels.
- The isolated reference module, sampling mask, source snapshot, compiler arguments and hashes are
  in `E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-reference-module/`.

## Static accuracy

RGB values are captured display-space values in 0–255, including direct lighting and materials.
RMS is the root mean squared RGB error. Relative RMS divides it by the reference RGB RMS;
PSNR uses a peak of 255. These are **image metrics**, not the raw linear diffuse-term errors in
the older 960x540 accuracy database. Residual reference noise has not been subtracted.

Candidates are frame 256. The reference is the mean of frames 240–256, extended to 496–512
for C2 and C3. The companion 17-frame candidate averages are retained in the JSON report.

| View | Quarter RMS /255 | Half RMS /255 | Quarter relative RMS | Half relative RMS | Quarter PSNR | Half PSNR |
|---|---:|---:|---:|---:|---:|---:|
| Profiled center | 0.696 | 0.589 | 3.41% | 2.89% | 51.28 dB | 52.72 dB |
| C2 | 0.707 | 0.562 | 5.68% | 4.51% | 51.15 dB | 53.14 dB |
| C3 | 0.419 | 0.354 | 6.53% | 5.52% | 55.69 dB | 57.15 dB |
| C4 | 0.930 | 0.808 | 9.71% | 8.43% | 48.76 dB | 49.98 dB |
| C5 | 0.285 | 0.272 | 53.27% | 50.79% | 59.03 dB | 59.45 dB |
| **Pooled five views** | **0.649** | **0.551** | **5.47%** | **4.64%** | **51.89 dB** | **53.32 dB** |

Pooled metrics combine squared errors and reference signal energy across views, rather than
averaging their percentages or decibels. C5 is nearly black with environment lighting disabled:
its reference RGB RMS is only about 0.535/255. Its large relative percentage accompanies a small
absolute error and is sensitive to display quantization.

### Reference validation and convergence

One complete 8x8 trace tile is selected within each 64x64-pixel stratum: 130,560 native pixels,
1.574% of the image. Both candidates use the same mask. RMS, signal and mean errors use inverse
selection-probability weights, including the shorter bottom strata. This samples image error;
it does not exhaustively find rare artifacts, which are measured separately over the full frame.

An isolated module skips unmeasured trace tiles and leaves selected pixels' integration unchanged.
It never replaces shipped SPIR-V files. At 64 samples, a full-dispatch versus sampled-dispatch
control was captured for every view: observed RMS differences were 0.0028–0.0187/255, with a
maximum difference of 2/255. These are small numerical differences, not byte identity.

The accuracy runs use 4,096 diffuse and 4,096 reflection samples per receiver per frame, native
GI resolution, no ReSTIR and no spatial filter. Reference temporal history caps at 256 frames;
after reaching that cap it is an exponential mean. Forward layers retain their ordinary shading
and reuse behavior; the final 17-frame average also reduces their remaining noise.

| View | Reference final frame | RMS drift between checkpoint averages /255 |
|---|---:|---:|
| Center | 256 | 0.1260 |
| C2 | 512 | 0.1225 |
| C3 | 512 | 0.0841 |
| C4 | 256 | 0.1524 |
| C5 | 256 | 0.0564 |

Checkpoint averages are 128–144 versus 240–256, or 384–400 versus 496–512. All ten final
candidate comparisons have reference drift below one quarter of their measured image RMS.
This is an observed convergence check, not a proof of zero bias or an independent noise estimate.
C2/C3 were extended because their initial half-GI comparisons missed this threshold; their
relative RMS results changed by less than 0.02 percentage points after extension.

The initial full-image 64-sample reference was rejected as insufficiently converged: center
checkpoint drift was 0.680/255. Its evidence remains in `matched-4k-20261112/`; those accuracy
numbers are superseded by the table above.

## Camera motion and sparks

The camera holds for 160 frames, then follows eight poses covering a 0.5-degree orbit and a
0.5% dolly. Each moving image is compared with its own preset at the same pose, settled for
256 frames. These measure temporal response relative to a settled image, not a moving
ground-truth path-tracer reference. All metrics below cover all 8,294,400 output pixels.

| Motion metric, mean over eight poses | Quarter | Half |
|---|---:|---:|
| Mean maximum-channel error /255 | 0.3769 | 0.2484 |
| RGB RMS error /255 | 0.7548 | 0.5944 |
| Mean per-pose PSNR | 50.58 dB | 52.65 dB |
| Mean per-pose 99th-percentile maximum-channel error /255 | 3.0 | 2.0 |
| Pixels with maximum-channel error >4/255 | 0.495% | 0.262% |
| Static reference's own lag-16 mean maximum-channel variation /255 | 0.0448 | 0.0595 |

Comparing against each pose's 17-frame static average gives almost the same mean errors:
0.3770/255 and 0.2483/255. Static residual variation is reported separately rather than
subtracted from the motion measurements.

The separate spark protocol warms up for 128 frames, captures 16 frames at a static camera or
alternating 0.5-degree poses, and scores the middle 12 frames. A spark rises by more than 48/255
relative to both matching poses two frames earlier/later. Counts use 8-connected components.

| Sparks per frame | Quarter | Half |
|---|---:|---:|
| One pixel, moving | 10.50 | 7.42 |
| One pixel, static | 7.42 | 5.25 |
| Four or more pixels, moving | 1.25 | 0.50 |
| Four or more pixels, static | 0.00 | 0.00 |

These fresh fullscreen counts reproduce the earlier spark measurements. The remaining outliers
are still present; lower global RMS does not establish their absence.

## Verification and reproduction

All 431 shipped shader hashes match the performance-capture shaders. The executable was rebuilt
after cleanup and adds diagnostic support for 64 reference samples and explicit buffered
reference-module selection. Candidate frame-256 images before/after that selection support are
pixel-identical at both GI resolutions. RenderCoreTest: 781 passed, 9 disabled; its 21 settings
tests also passed independently. Reference modules pass SPIR-V validation. Capture jobs check
actual framebuffer/GI extents, provider, fullscreen, async state and Vulkan validation output.
Analytic checks verified RGB RMS, PSNR and the sampling weights.

A final fresh frame-128 capture at each resolution is also pixel-identical to the retained
release captures associated with the performance measurements: zero differing pixels at both
resolutions (`baseline-check.json`).

The setup's copied default environment was restored after cleanup from `Data/Textures/papermill.ktx`;
its SHA-256 exactly matches the retained original input hash. `Data/engine.cfg` was not edited.

Reproduction tools (use `build/venv/Scripts/python.exe` with the pinned quality requirements):

1. `tools/build_hybrid_quality_reference.py --output=<new-reference-directory>` builds isolated
   modules and the sampling mask.
2. `tools/gi_matched_quality.py --setup=<quarter.zenrender> --half-setup=<half.zenrender>
   --reference-module=<reference.omm.spv> --reference-mask=<mask.png> --output=<new-dataset>`
   captures the five views and motion sequence. Select the appropriate OMM module for the GPU.
3. `tools/refine_gi_reference.py --dataset=<dataset>` extends views failing the convergence check
   and writes `report-refined.json`, preserving the initial report.
4. `tools/gi_matched_sparks.py --setup=<quarter.zenrender> --output=<new-spark-directory>` repeats
   the fullscreen spark comparison.

The scope is this scene, lighting setup, five static views and two short camera-motion tests.
Independent normal-mapped reference shading, environment-on quality and long motion sequences
are not certified by these measurements.
