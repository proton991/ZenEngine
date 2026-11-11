# Per-pixel ReSTIR GI implementation and verification

2026-11-10. The core Phase 1–4 code paths are implemented and this verification round is complete.
The plan is not complete: scene acceptance gates fail, and the planned radiance-refresh comparison
remains undone. `restirGI` remains off by default. Phase 5 remains the optional, out-of-scope
Compute extension described in [the plan](ReSTIRGIPlan.md).

## Settings and scope

In a `.zenrender` setup, `rendering.gi.cone` accepts:

```json
{
  "restirGI": true,
  "restirSpatial": true,
  "restirSpecular": true,
  "samples": 4
}
```

These are optional fields, so older setups retain their previous behavior. The corresponding
legacy configuration keys are `voxel_gi_restir`, `voxel_gi_restir_spatial`, and
`voxel_gi_restir_specular`. The editor exposes the three switches under per-pixel gather.
`samples` already supports 1, 2 and 4; adaptive 4/6 sampling remains available at 4.

Reuse applies independently to the HW bounce and reflection feature bits. Compute features keep
their existing gather. Screen probes, reference accumulation and ray-count diagnostics retain
their existing estimators. Sky escapes and primary direct lighting retain their existing
estimators. Reflections below roughness 0.3 retain the original fresh-ray estimator.

## Estimator and storage

`restir_reservoir.glsl` defines the six scalar words of a 24-byte record: secondary position,
12+12-bit octahedral normal, seven-bit candidate count and validity flag, RGB9E5 radiance, and
FP32 contribution weight. The CPU layout has a matching size assertion in `Graphics/Shared/ReSTIRGI.h`.
RGB9E5 quantization and the count cap are tested on the GPU.

HybridTrace streams its existing diffuse proposals using their existing mixture-PDF weights.
Completed misses and zero-contribution proposals count toward M; unavailable traces do not.
It stores one selected hit and its normalization. There is no extra initial diffuse traversal.
The reflected hit signal uses a separate record. Its target is normalized GGX reflected radiance:
the deferred pass still owns the split-sum material factor. A 128×128 integral table, built once,
normalizes the existing Smith/VNDF lobe. An independent hemisphere quadrature checks this table
and the analytic roughness-one, normal-incidence integral `1 − log(2)`.

Temporal reuse reprojects through receiver motion and checks receiver identity, plane distance,
shading and geometric normals, and roughness for reflections. History M is limited to four fresh
frames for diffuse, twenty for rough reflections, and the seven-bit packed range. The 20-frame
diffuse candidate reduced lag-16 change but increased reference RMS. Reconnection converts solid-angle measures with the
secondary-normal/distance Jacobian. Back-side reconnections have zero weight. Visibility uses
finite HW segments with offsets at both endpoints.

Spatial reuse chooses four neighbors within 32 pixels at 1080 lines, scaling with render height.
It uses the same surface tests plus relative roughness agreement for reflections. An initial
1/M experiment lost about 22% of the center-view bounce mean at 960×540; it was rejected.
The implementation uses a pairwise balance heuristic instead. Each neighbor is paired with the
center, the center's confidence is divided between its pairs, and the pair weights are averaged.
Both target densities are expressed in the center's
solid-angle measure. Reverse visibility is checked when evaluating the center sample at a
neighbor. The selected shifted sample is checked before resolve. The weight sum is normalized
by the selected target; M remains a bounded reuse-confidence count.

The partition follows the generalized pairwise MIS family in the
[GRIS supplementary derivation, section S.1.2](https://graphics.cs.utah.edu/research/projects/gris/GRIS_supplemental.pdf).
The earlier equal-pair variant retained half the center weight for equal reservoirs. The current
uniform variant gives five equal reservoirs one-fifth each; the GPU fixture checks this with
different center and neighbor radiance.

Temporal runs in place because each invocation reads/writes only its own current record.
Spatial uses two dispatches: each writes one checkerboard parity and reads only the opposite
parity. Render-graph barriers separate those dispatches. This avoids a third full reservoir set.
Resolve is folded into spatial; disabling spatial still runs a resolve dispatch.

Two persistent diffuse records cost 398,131,200 bytes at 3840×2160; rough reflections add the same
amount. Disabled components use one-record placeholders. HW-unused previous-cache bounce and
reflection images use 1×1 placeholders and their shaders skip those stores. The full-size current
bounce/filter images are preserved. Allocations validate storage-buffer limits and alignment;
failure declines the view instead of using partial state. Resizing or changing the reservoir
layout invalidates history, including a declined toggle. The previous/current side and LUT
initialization are published only after a successful submission.

## Lighting changes and filtering

Geometry, surface, lighting and provider revisions reset the appropriate histories. Lighting
resets restart fresh reservoirs instead of retaining stale secondary radiance. The implementation
uses this reset policy rather than adding another hit-shading evaluation for every selected
sample. A refresh would also need to recover the secondary material/UV data, which is not stored
in the 24-byte record. The dynamic-light measurement below evaluates the resulting recovery;
the planned refresh-versus-reset implementation and empirical cost comparison remain undone.
This is a plan deviation, not a measured finding that resetting is better than refreshing.

The reconstruction variance previously assumed independent frames and neighboring taps. Reused
paths violate both assumptions. For reused components, preparation divides history length by
the corresponding lobe's reuse cap before estimating variance of the mean. Spatial propagation
blends the independent variance with the full-correlation bound
`(sum(w * sqrt(v)) / sum(w))²`: 0.025 of the bound after the first pass and 0.2 after later passes.
The full bound increased mean reference RMS to 10.63%; the mixture reduced it to 7.21% with the
same four-frame diffuse cap. Sampled horizontal raw-error correlations in three views were
below 0.022. Overlapping unweighted B-spline footprints imply mixtures of 0.140–0.189 in later
passes. The script, input hashes and calculations are retained in
`E:/Dev/ZenGroundTruth/restir/uniform-m4-cov-20261110/reproduce/covariance-evidence.{py,json}`.
Those three views and stationary footprints motivate an approximation; they do not establish
the covariance of every recursive reservoir. Smooth reflections and non-reused terms retain
their original variance formulas. All five à-trous passes remain.

ZenApp's batch diagnostic output buffers now use host-visible memory, as their readback buffers
already did. The two storage outputs otherwise added 3.052 GB of device-local memory at 4K and
two measurement attempts lost the device. The existing transfer and CPU-read ordering is
unchanged. This affects infrequent batch captures; ordinary rendering and Nsight timing runs
do not allocate these buffers. The subsequent 4K capture completed successfully.

## Deterministic light sequence

ZenApp now accepts `--light-path=<keys.json>` with `--fixed-step`. For example:

```json
{
  "lightId": 11,
  "keys": [
    { "frame": 1, "position": [0, 1, 0] },
    { "frame": 161, "position": [0.04, 1, 0] }
  ]
}
```

The authored ID must select a point or spot light. Keys use strictly increasing positive frame
numbers and finite positions. A key is held until the next key; a smooth sampled path supplies a
key for every moving frame. Changes use the existing rendering-settings/light-revision path.
Malformed paths do not publish partially parsed state.

`tools/gi_light_stability.py` produces eight moving frames followed by eight recovery frames by
default, after 160 warmup frames. It compares every moved pose with a 256-frame static render at
the identical light position and records each static reference's lag-16 residual. It retains
commands, setup/input/executable/shader hashes, the light path hash and actual capture state.
Use a fresh output directory:

```powershell
build/venv/Scripts/python.exe tools/gi_light_stability.py `
  --setup=<scene.zenrender> --light-id=11 --output=<new-output-directory>
```

This reports display-space error, not a linear-radiance oracle. A static reference has its own
sampling noise, and direct lighting can dominate a whole-image metric. The tool also records
the change between the first and last static light poses. At the last pose and during recovery,
it reports error in pixels whose static endpoints differ by more than four 8-bit levels, and
saves this region as `changed-region.png`. This identifies a sequence that merely resets history
without appreciably changing visible illumination. Cross-configuration recovery comparisons
use the off-control region for every configuration, rather than different per-configuration masks.

The initial lamp-1 run is retained under `off/light/` as a reset-control experiment. Moving it
changed only 562 pixels between the first and last static poses, and only one pixel by more
than four levels. It is not used to establish lighting-recovery acceptance. The visible-light
fixture moves lamp 11 by the same 0.04 world units along X. Its preflight endpoint comparison
changes 240,547 pixels, including 71,804 by more than four levels, with mean maximum-channel
change 2.114/255. The setups and commands are in `light-fixture-check/`; the actual sequences
are recorded under each configuration's `light-visible/` directory.

## Verification records

The frozen comparison baseline remains [Phase 0](ReSTIRGIPhase0.md). Current evidence is under
`E:/Dev/ZenGroundTruth/restir/verification-20261110/`; `frozen-build.json` identifies the executable and
every shader. Each configuration derives an immutable copy of the same reference dataset by
changing only the ReSTIR switches and sample budget. Reference arrays remain unchanged.
The executable SHA-256 is `5a0142a01007977786aff6f64b6e8e0744088d02f208c41d4084c81dc57df389`.
The executable and all 300 shader binaries are archived under `frozen-binaries/` in that evidence
directory; `frozen-build.json` records their original paths and individual hashes.

The configuration names mean:

| Configuration | Temporal | Spatial | Rough specular | Diffuse budget |
|---|---|---|---|---|
| phase1 | yes | no | no | 4 |
| phase2 | yes | yes | no | 4 |
| phase3 | yes | yes | yes | 4 |
| phase4-2 | yes | yes | yes | 2 |
| phase4-1 | yes | yes | yes | 1 |

All use the same correlation-aware reconstruction and frozen build. Adaptive sampling is off
in the frozen Phase 0 setup and these comparisons, so the first three cases use four diffuse
rays. The engine also retains its existing adaptive 4/6 option. The earlier directory named
`final-20261110` contains a rejected tuning round, not this verification build.

Release build succeeds. RuntimeLaunchTest has 21 passing tests, including optional-setting
round trips and light-path parsing/held keys. RenderCoreTest has 781 passing tests with no
allocator leaks; nine tests disabled by that suite were not run. Together with eight math and
four reuse GPU cases, 814 tests pass with no failures. The added lifecycle fixture covers lighting/geometry revisions and a declined
disable/re-enable sequence. Its visibility submission callback and frame advancement match
the renderer lifecycle.

The reservoir/math/analytic emissive-disk and GGX-integral GPU tests pass in all four submission
modes. The ray-query fixture passes in all four modes and covers separate diffuse/specular count caps,
foreign surfaces, invalid motion, invalid receivers, reset, two in-place spatial parities and
occlusion rejection. These runs enable Vulkan validation. An 80-frame standalone ZenApp run
also passes validation while changing a point light at frame 40.

Current logs and XML results are in `verification-20261110/tests/`. Full ZenApp validation also
passes in all four submission modes; their 40-frame captured images are identical. Probe and
reference-mode exclusion runs pass. Mixed-provider runs pass both during distance-field
preparation in New Sponza and after publication in the small curved-surface fixture. The latter
record `fieldTraversal=true`, with hardware masks 47 and 55 for HW bounce/Compute reflection
and Compute bounce/HW reflection respectively. Finite tests establish those
contracts; they do not establish the scene quality/performance gates.

## Measurement results

The static matrix, same-build off control, camera and moving-light sequences are complete.
The current build uses four-frame diffuse and twenty-frame specular
history caps throughout this matrix:

| Configuration | Mean bounce RMS | Max bias change | 4K bounce change | 4K specular change |
|---|---:|---:|---:|---:|
| Phase 0 baseline | 7.795% | — | 2.922% | 30.190% |
| Temporal diffuse, 4 rays | 8.044% | 0.806 points | 2.114% | 30.190% |
| Spatial diffuse, 4 rays | 7.215% | 1.545 points | 1.444% | 30.190% |
| Diffuse + rough reflection, 4 rays | 7.224% | 1.545 points | 1.444% | 5.615% |
| Diffuse + rough reflection, 2 rays | 8.552% | 1.573 points | 1.956% | 5.583% |
| Diffuse + rough reflection, 1 ray | 10.359% | 2.322 points | 2.471% | 5.590% |

| Configuration | Median frame | HybridTrace | Steady allocation | Peak allocation |
|---|---:|---:|---:|---:|
| Phase 0 baseline | 94.512 ms | 37.255 ms | 13.401 GB | 15.276 GB |
| Same-build off control | 97.102 ms | 36.163 ms | 13.401 GB | 15.276 GB |
| Temporal diffuse, 4 rays | 102.325 ms | 38.322 ms | 13.741 GB | 13.741 GB |
| Spatial diffuse, 4 rays | 106.146 ms | 38.217 ms | 13.741 GB | 13.741 GB |
| Diffuse + rough reflection, 4 rays | 121.619 ms | 38.400 ms | 14.139 GB | 14.139 GB |
| Diffuse + rough reflection, 2 rays | 104.630 ms | 24.138 ms | 14.139 GB | 14.139 GB |
| Diffuse + rough reflection, 1 ray | 96.248 ms | 17.850 ms | 14.139 GB | 14.139 GB |

Timing uses 30-frame Nsight Graphics GPU Traces after a 60-second capture delay, with 3840×2160
render and framebuffer extents, windowed presentation, threaded RHI, HW RT and async compute
available. Memory is measured in separate uncaptured 160-frame runs, rendering at 3840×2160
into a 1920×1080 window, matching Phase 0. GB means decimal bytes. Captured quality runs use
one frame slot to keep readback pressure bounded. The off control passes 4K batch validation
without leaks, and all ten lighting-term statistics match the frozen baseline exactly. Its
eight camera-motion error and static-residual measurements also match exactly (mean 0.768/255).
See `off/control-comparison.json`. The off-control timing is reported separately to expose
the difference from the historical timing run; feature overhead can also be compared within
the current build.

Rough reflection reuse passes the specular-noise limit. Spatial diffuse improves mean RMS at
four rays, but exceeds the one-point bias-change limit in C5_L3. No case reaches 0.6% bounce
change. Two and one rays meet the 25 ms HybridTrace target and reduce static noise relative
to baseline, but both worsen mean RMS. Full-frame time also remains above baseline. Therefore
the measurements do not justify enabling ReSTIR or lowering the ray budget by default.

The imported-lamp comparison has 496 matched receivers. Measured bounce bias is −8.83% for
temporal-only and −5.92% for spatial reuse at four rays, versus −9.69% at baseline. Its reference
RMS noise is about 97.65% of the mean, so these aggregate biases are inside that noise scale;
the zero noise-corrected RMS values are unresolved measurements, not zero physical error.

### Camera and moving-light results

Both sequences render at 960×540 with fixed simulation steps. Camera motion uses eight poses,
0.5-degree orbit steps and 0.005 dolly fraction after 160 warmup frames. Each pose is compared
with its own 256-frame static image. The light sequence moves lamp 11 across eight frames and
then holds it for eight recovery frames. All table entries are mean maximum-channel absolute
errors in 8-bit levels (out of 255); static residuals compare static frames 256 and 240.

| Configuration | Camera error | Camera static residual | Light error after 8 frames | Light error in common region | Static residual in common region |
|---|---:|---:|---:|---:|---:|
| Same-build off control | 0.768 | 0.508 | 0.803 | 0.922 | 0.409 |
| Temporal diffuse, 4 rays | 0.745 | 0.489 | 0.774 | 0.831 | 0.374 |
| Spatial diffuse, 4 rays | 0.755 | 0.489 | 0.778 | 0.848 | 0.372 |
| Diffuse + rough reflection, 4 rays | 0.482 | 0.242 | 0.681 | 0.759 | 0.252 |
| Diffuse + rough reflection, 2 rays | 0.518 | 0.258 | 0.689 | 0.716 | 0.277 |
| Diffuse + rough reflection, 1 ray | 0.559 | 0.290 | 0.760 | 0.810 | 0.315 |

The common light region contains the same 71,804 off-control pixels in every row. Frame-eight
99th-percentile errors in that region are 7, 5, 5, 3, 3 and 4 levels respectively. The complete
recovery curves are in `light-recovery.png`; all per-frame results and capture hashes are in
`motion-light-comparison.json`. The off camera result exactly reproduces Phase 0.

Every configuration improves these aggregate camera and final recovery errors relative to the
off control. However, recovery errors remain above the static lag-16 residual after eight frames,
and the inspected four-ray error image retains localized patches, including the left floor.
`light-recovery-inspection.png` shows the off and combined four-ray frame, static reference and
error image. These measurements do not separate history lag from correlated noise or residual
bias; the static residual is itself a finite, correlated noise proxy. They therefore do not
establish full recovery to a stationary result within eight frames or certify absence of ghosting.
The reset path is exercised, but the stricter dynamic-light acceptance remains unproven.

### Rejected tuning and capture failures

Rejected tuning rounds are retained separately:

| Round | Mean bounce RMS | Max bias change | 4K bounce change | 4K specular change |
|---|---:|---:|---:|---:|
| Phase 0 | 7.795% | baseline | 2.922% | 30.190% |
| Four-frame cap, full covariance (`uniform-m4-20261110`) | 10.629% | 1.55 points | capture failed | capture failed |
| Four-frame cap, covariance mixture (`uniform-m4-cov-20261110`) | 7.211% | 1.55 points | 1.44% | 11.62% |
| Twenty-frame cap, covariance mixture (`uniform-m20-cov-20261110`) | 8.205% | 1.55 points | 0.93% | 5.59% |

The largest bias change is C5_L3, from −10.55% to approximately −9.00%. It moves toward the
reference but still exceeds the plan's one-point difference limit. All 20 reference views
remain noise-limited under the quarter-error test; the measured differences are not a
certificate of unbiased transport.

The first 4K phase1 noise attempt (`final-20261110/phase1/noise`) lost the Vulkan device at
frame 204, with approximately 16.392 GB peak device-local allocation. The four-frame full-bound
round lost it at frame 65 with 15.316 GB. A subsequent uncaptured 256-frame 4K validation run
passed with 12.263 GB, and host-buffer captures then completed. Failed runs contain no usable
quality capture and are excluded from quality and timing statistics.

## Reproduction and retained state

The evidence root contains `comparison.json` for static results and `motion-light-comparison.json`
for camera and moving-light results. The latter recomputes recovery errors from retained PPMs
and records their hashes and the common affected-region mask. `execution.json` records 25
successful static-matrix jobs. `motion-execution.json` records the control and dynamic jobs;
the phase1 camera report was reused after its parent runner was stopped while the child finished.
That successful report is verified separately instead of inventing a missing runner record.

The `reproduce/` directory archives the matrix runners, analysis scripts, measurement tools and
build/format logs. Matrix runners contain the exact arguments and frozen configurations. They
resume their existing evidence directories; use a new output root for a fresh experiment.
Run the analysis scripts from `E:/Dev/ZenEngine` with `build/venv/Scripts/python.exe`.
`provenance-audit.json` records the final hash audit of executable/shaders, their archived copies,
measurement inputs and immutable reference datasets, together with the test counts. These checks
verify measurement identity, not the image-quality acceptance gates.

`source-snapshot/` retains the engine and ZenTest base commits, binary Git patches and complete
copies of changed/new files, including files that Git patches omit while untracked. The new work
is uncommitted. The earlier Phase 0 commits remain the baseline; no source edits were made in
the ZenAssetProcessor submodule.

## 4K performance checkpoint (2026-11-10)

Work is stopped at the user's request after documenting this status. The changes remain
uncommitted. The historical ReSTIR acceptance failures above remain outstanding; this checkpoint
does not retroactively pass them.

The user accepted the four-ray Phase 3 quality baseline and allowed reconstructed GI at 4K,
targeting at least 30 FPS and an 8 GB GPU. The tested center view now exceeds 30 FPS on the
**RTX 5080 16 GB**. Sampled dedicated process memory is below 8 GiB, with limited headroom.
**Actual 8 GB hardware, other GPU models, and worst-case performance across the scene remain
unverified.** This is not a guarantee of 30 FPS on an unspecified 8 GB card.

### Implemented changes

- `pixelResolutionDivisor` is optional, defaults to 1, and accepts 1, 2 or 4. It scales the HW
  pixel GI receiver grid, histories, reservoirs and filters. The tested preset uses 960x540 GI
  with 3840x2160 rasterization, direct lighting, translucency and output. Compute/mixed GI,
  probes, reference accumulation and ray-count diagnostics retain native grids.
- Reconstruction checks surface identity, normals, plane distance and roughness. Unsupported
  full-resolution receivers trace their own rays, using at least sixteen diffuse samples because
  they lack temporal support. The ordinary GI grid retains four rays and diffuse/spatial ReSTIR
  plus rough-reflection reuse.
- Reduced GI keeps longer histories for unchanged receivers, scaled by the reduction in sample
  density and capped at 256 frames. Movement restores native limits immediately. Existing
  geometry/light/material/environment/settings invalidation still applies. This reduces steady
  noise without more buffers or rays, but extends stationary convergence.
- HW shadowing builds a per-light candidate-triangle table from current geometry every frame.
  A candidate skips traversal only after an interior intersection against the receiver's actual
  finite segment. Transparent candidates, edges, grazing rays and endpoints use the original
  HW query. No interpolated visibility is cached. The shortcut also applies to selected
  secondary-light queries in ray-shadowed forward materials. Diagnostic counts include building
  the table. The tested shadow-cache change produced an identical image.
- Unused forward scattering/refraction targets and disabled reconstructed sky outputs use small
  placeholders. Motion remains at 32-bit precision in two channels; previous clip W uses the
  position buffer's previously unused W. These storage changes produced identical images.
- The tested scene package uses the published processor's BC7/BC5/BC4 cooking with default
  quality gates. Rejected images retain their original pixels. No processor source or engine
  submodule revision was changed. The package remains an external artifact.
- ZenApp and its profiling summaries record actual GI/output extents. Full-resolution capture
  outputs remain available; raw/history diagnostics identify nearest samples of the GI grid.

### Measured performance and memory

All runs use standalone ZenApp, explicit `.zenrender` setups and startup overrides. No changes
were made to `Data/engine.cfg`. Final Nsight capture: `tools/profile_hybrid_gi.py`, base clocks,
60-second warmup, 30 frames, threaded RHI, async compute available, GPU culling, no UI/vsync,
windowed 3840x2160 framebuffer and render extent, two frames in flight. Pass intervals can overlap
and must not be summed as total frame time.

| Metric | Before | Final |
| --- | ---: | ---: |
| Total frame, accepted native four-ray Phase 3 baseline | 121.619 ms | 27.943 ms |
| Direct lighting, reduced-GI control before lighting optimization | 14.453 ms | 5.395 ms |
| Translucency, same reduced-GI control | 14.827 ms | 10.547 ms |
| Renderer device-local allocation | 14.139 GB | 7.845 GB |

Final median throughput: **35.8 FPS**. Frame time p95: **29.762 ms**;
maximum of 30 samples: **30.406 ms**. The two lighting passes total **15.942 ms**, about
46% less than their 29.280 ms control. Translucency remains the largest pass because layers that
cannot reuse opaque receivers still trace their own transport.

The initial baseline used native GI, uncompressed textures and three frames in flight. Its
memory run rendered 4K into a 1920x1080 window; final memory runs use a 4K window. The reduced-GI
lighting control used one queued frame and a 30-second warmup. These configuration changes are
part of the optimization, with exact inputs retained. One queued frame with compressed textures
measured 34.457 ms before the final quality refinement, so the preset uses two. Intermediate
traces ranged from 25.844 to 30.591 ms; the final table uses the final-build measurement.

The final 2,000-frame validation run recorded **7,845,134,496 device-local bytes** (7.306 GiB),
peaking at **7,878,557,856 bytes** including screenshot capture. Windows GPU Process Memory
counters sampled the NVIDIA process at up to **8,343,396,352 dedicated bytes**
(**8.343 decimal GB / 7.770 GiB**), including overhead outside the renderer's allocator.
The approximately 44-second counter recording is sampled, not an instantaneous lifetime peak
guarantee. This leaves about 235 MiB below 8 GiB physical capacity before other applications.
Actual 8 GB hardware validation remains open; allocator-only figures must not be presented as
complete dedicated-memory usage.

### Quality evidence and limits

Five 4K camera views were compared at frame 256 with retained native four-ray images using the
same cameras and lights. Final images include reconstruction and compression. The native images
precede changes confined to reduced-GI histories/fallbacks and disabled-sky storage; each capture
has its executable, setup, package and shader provenance. Mean difference is the mean of the
maximum absolute RGB channel error per pixel, in 8-bit levels out of 255.

| View | Mean difference | P99 difference | Pixels differing by more than 16 |
| --- | ---: | ---: | ---: |
| C1 | 0.246 | 2 | 0.0170% |
| C2 | 0.396 | 3 | 0.0461% |
| C3 | 0.173 | 1 | 0.0091% |
| C4 | 0.389 | 3 | 0.0533% |
| C5 | 0.058 | 1 | 0.0092% |

The reviewed views remain visually close, but are not identical. P99.99 differences are 16-35
levels; the largest isolated difference is 223/255. References are noisy native renders, not
noise-free ground truth. This does not replace the original 20-view estimator-accuracy gates.

The existing 4K noise protocol (256 settled frames, lag 16, normal maps off, 1920x1080 window)
measures mean absolute linear change relative to each term's mean:

| Term | Accepted native baseline | Final reconstruction |
| --- | ---: | ---: |
| Diffuse bounce | 1.444% | 1.421% |
| Specular GI | 5.615% | 2.185% |
| Total color | 0.289% | 0.145% |

Mean bounce radiance differs by -0.23%, specular by -4.73%, and total color by -0.13% from the
retained observation. Aggregate flicker is no worse here, but isolated fallback outliers remain
larger: maximum linear bounce change 20.61 versus 0.55, and specular 2.99 versus 0.41. Native
quality is not certified at every pixel. Extra fallback samples alone did not fix the initial
noise regression; longer steady histories supplied most of the improvement.

The 4K camera test uses eight poses over a 0.5-degree orbit and 0.005 dolly fraction after 160
warmup frames. Error against each pose's 256-frame stationary render averages **0.612/255**,
with **0.074/255** stationary lag-16 residual and worst-pose P99 **6/255**. Before the steady-history
refinement, this 4K test measured 0.657/255 with 0.258/255 residual. The earlier native motion
report was 960x540, so it is not a same-resolution comparator. The final reduced-GI moving-light
sequence has not been rerun; GPU tests do exercise light and geometry movement in the shadow cache.

Noise and motion runs precede only the disabled-sky storage change. A matched 2,000-frame image
before/after that change is byte-identical. Final five-view, timing and memory runs include it.

### Verification and reproduction

Release ZenApp, affected shaders and tests build. **866 tests pass**: 781 RenderCore, 21
RuntimeLaunch, 60 GPU ReSTIR/screen-probe tests, and four actual ray-query submission-mode tests.
Nine disabled RenderCore tests remain disabled. The shadow-cache fixture checks 49,152 finite
segments across light/geometry movement and four submission modes, with no false occlusion
observed. Final validation captures contain no Vulkan validation errors. Touched C++ was formatted
with the project configuration; Python tools compile and diff checks pass.

Evidence root: `E:/Dev/ZenGroundTruth/restir/optimization-4k-20261110`.

- `final-timing/`: final GPU Trace, exported tables, exact arguments, input hashes and summary.
- `quarter-bc-v12.*`, `dedicated-memory-v12.json`: allocator and Windows memory evidence.
  `quarter-bc-final-memory.*` is the matched pre-storage-change capture.
- `quality-final.json`, `matrix-final-C*`, `matrix-C*-native.*`: final view comparisons and references.
- `noise-v11/`, `motion-final/`: final temporal evidence before the image-identical storage change.
- `render-final.log`, `runtime-v7.log`, `gpu-final.log`, `rayquery-final.log`: test results.
- `cook-bc.provenance.json`, `cook-bc.log`, `sponza-bc.zenasset`: cooking evidence and scene package.
- `NewSponza-4K.zenrender`: usable local preset, validation disabled for normal use. Scene and
  environment paths are machine-local; engine defaults are unchanged.

From the ZenEngine workspace:

```powershell
.\build\x64-windows-msvc-release\bin\ZenApp.exe --setup=E:/Dev/ZenGroundTruth/restir/optimization-4k-20261110/NewSponza-4K.zenrender --backend=hardware --fullscreen=0 --width=3840 --height=2160 --rhi=threaded --async-compute=1 --gpu-culling=1 --vsync=0 --validation=0 --no-ui
```

Remaining: actual 8 GB hardware and broader performance coverage, final moving-light comparison,
isolated bright fallback outliers, and the original plan's unresolved acceptance,
refresh-versus-reset and optional Compute work. Work stops here; no commit or push was performed.

### Subsequent rendering cleanup

The next user request resumed work to remove confirmed unused rendering code. See
[Rendering cleanup](RenderingCleanup.md) for the removed paths, retained algorithms and verification.
The performance and quality measurements above describe the preceding checkpoint.

### Camera-motion flicker correction (2026-11-11)

The reconstructed 4K preset now retains validated history during camera motion and tolerates
roughness-texture variation across coarse reflection receivers. The previous checkpoint's
motion-history limits above are historical. See [HW RT flicker correction](HWRTFlicker.md) for
the current limits, isolation captures, motion measurements and remaining outliers.
