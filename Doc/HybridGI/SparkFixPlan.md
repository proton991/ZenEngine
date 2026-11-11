# HW RT spark fix — plan

2026-11-11. Status: implemented; results at the end. This plan follows the spark
investigation of the per-pixel HW RT gather with reconstructed GI and ReSTIR enabled
([ReSTIR plan](ReSTIRGIPlan.md), [flicker correction](HWRTFlicker.md),
[editor follow-up](EditorHWRTFollowup.md)).

## Problem

Bright yellow dots flicker in New Sponza, most visibly while the camera moves. They are GI only:
a direct-lighting capture has none.

Every spark is a raw one-frame GI sample that reaches the screen without temporal history,
spatial filtering or reservoir reuse. The raw estimator is heavy-tailed: a ray that lands beside
a lamp returns radiance hundreds of times the mean (`EvaluateLight` floors the squared distance
at 1e-6, so the peak is about 760 times the light colour). ReSTIR reuse is not the source;
disabling it leaves the one-pixel sparks unchanged and increases the larger dots.

Three paths bypass the denoiser:

| Path | Code | Sparks | Static camera |
|---|---|---|---|
| A. Blended decal layers trace their own GI each frame (one reflection ray, `samples` diffuse rays) | `forward_material.glsl`, `reconstructed` / `diffuse` selection and the `HardwareReceiver*` calls | 47% of one-pixel sparks | Yes |
| B. Full-resolution receivers without a compatible GI texel trace a raw fallback | `hybrid_reconstruct.comp` rejection tests, `hybrid_reconstruct_fallback.comp` | 36% of one-pixel sparks | Yes |
| C. GI texels that lose history under camera motion show their raw sample; reconstruction spreads it into a dot or an edge streak | Not isolated; surface tests in `hybrid_temporal.glsl` are the leading suspect | All dots of four or more pixels | No |

About 16% of the one-pixel sparks are on neither A nor B and are unattributed.

## Baseline

ZenApp, `editor-hw-20261111/quarter-normal-bc.zenrender` (3840x2160 render, quarter-resolution GI,
normal maps on, ReSTIR diffuse/spatial/rough reflection, four rays), 1280x720 window, fixed step.
After 128 warmup frames the camera alternates between two poses 0.5 degrees apart for 16 frames.
A spark is a pixel whose maximum channel exceeds the same pose two frames earlier and two frames
later by more than 48/255. Counts are per frame, averaged over 12 frames.

| Run | One-pixel sparks | Dots of 4+ pixels |
|---|---:|---:|
| Current, moving | 114.7 | 14.6 |
| Current, static camera | 101.3 | 0.1 |
| ReSTIR off, moving | 118.7 | 18.8 |
| Decal self-tracing zeroed, moving | 60.3 | 14.6 |
| Native GI resolution, moving | 230.5 | 4.9 |
| Native GI resolution, ReSTIR off, moving | 337.9 | 23.1 |

Fallback receivers: about 48,000 pixels per frame (0.58%). Rejection reason: geometric normal
(`dot < 0.9`) 81%, plane distance above the fixed 0.001 limit 9%, shading-normal weight underflow
7%, roughness weight underflow (specular only) 3%.

Removing the eight-frame moving reflection cap and the parallax rejection did not change the dot
count, so those two rules are not the cause of path C.

These numbers come from temporary captures of the diagnosis session. Phase 0 reproduces them
with a retained tool before any change.

## Phases

Each phase is measured against the Phase 0 baseline before the next starts. A phase that fails
its exit condition is reported as failed, not tuned until the count passes.

### Phase 0 — measurement tool and baseline

- Add `tools/gi_spark_metric.py`: the alternating-pose capture above plus a static-camera run,
  spark counting by size class, and location overlays. It records executable, argument list,
  setup and shader hashes, and actual extents, as the other GI tools do.
- Capture divisors 1, 2 and 4 with ReSTIR on, moving and static. Half resolution is the editor
  configuration in the follow-up report and has not been measured for sparks.
- Capture the energy reference for the later phases: mean `D_bounce`, mean specular GI and mean
  final colour on the 4K center view (existing noise protocol).

Exit: the quarter-resolution rows above reproduce within run-to-run variation.

### Phase 1 — decals reuse the receiver's GI (path A)

A blended layer resting on the visible opaque receiver takes that receiver's reconstructed
diffuse and specular GI instead of tracing its own.

- `HybridReceiverSupports` already implements the test, but `diffuse` only uses it when
  `hardwareDiffuse` is false. Extend it to the HW path.
- Its distance limit is half a voxel, which is far looser than a decal offset. Use a limit in
  the reconstruction's own units (plane distance scaled by view distance) for the HW path, and
  record what fraction of blended fragments pass.
- Specular reuse additionally requires similar roughness (the existing
  `HybridReflectionRoughnessTolerance`) and shading normal. A layer that fails keeps its own
  trace and falls under Phase 4.
- Layers with transmission, or not resting on a receiver (glass, free-standing blended
  geometry), are unchanged.

Exit: one-pixel sparks fall by at least 40% static and moving; decal appearance reviewed in
five views; ForwardTranslucent time recorded (it is the largest pass at 11 ms and should fall,
which is expected and not yet measured).

### Phase 2 — fewer and quieter fallback receivers (path B)

- Scale the plane-distance limit by view distance, as temporal and reservoir reuse already do.
- Before tracing, search a wider GI window for a texel that passes the existing tests. Edge and
  bevel faces usually continue along the edge beyond the current 4x4 window. Record how many of
  the 48,000 receivers this resolves.
- Decide the geometric-normal case on data: either accept the nearest same-surface texel with a
  relaxed normal test (stable, biased toward the neighbouring face) or keep tracing (unbiased,
  noisy). Compare both against a 256-frame accumulated reference on the fallback mask.
- Receivers that still trace fall under Phase 4.

Exit: fallback receivers below 0.2% of the frame at quarter resolution; one-pixel sparks on the
fallback mask reduced by at least 75%; no visible light leaking across edges in the five views.

### Phase 3 — motion dots (path C)

Diagnosis first; the mechanism is not isolated.

- Record, per GI texel, why bounce and reflection history were rejected (identity, plane,
  shading normal, geometric normal, roughness, view, out of view) and the resulting history
  length. `nextLength.b` already carries a sky reason code and is the natural place.
- Join that record with the dot locations from the spark tool. Half of the dots are elongated
  along edges and 45% touch fallback receivers (12% by chance), which points at thin receivers
  whose reprojected taps land on another face. The round dots on open surfaces are unexplained.
- Fix what the record shows. Candidates, in order of preference:
  1. A history tap search for thin receivers: when the bilinear taps fail, look for the nearest
     previous texel with the same identity and compatible plane and normal.
  2. Native resolution keeps the 0.02 roughness and 0.99 normal thresholds that were relaxed for
     reduced resolution only. Measure whether they explain the native-resolution sparks.
  3. Texels left without history fall under Phase 4.
- Screen-border dots (newly revealed pixels) are real disocclusion and are handled by Phase 4.

Exit: moving dots of 4+ pixels below 3 per frame at quarter resolution, with no increase in
ghosting on the existing eight-pose motion test.

### Phase 4 — bound samples that have no history

Whatever remains after Phases 1-3 is a receiver that legitimately has no history. Bound only
those samples; converged receivers are untouched.

- Applies to: self-traced forward layers, reconstruction fallback receivers, and GI texels whose
  bounce or reflection history length is below four frames.
- Limit the sample's luminance to a multiple of a local reference taken from receivers that do
  have history (the filtered neighbourhood for GI texels, the reconstructed neighbours for
  full-resolution receivers). Start at a factor of 8 and measure.
- This is biased. An earlier neighbourhood clamp in screen-probe mode removed 49% of reflected
  light, so the clamp is accepted only with the energy check below, and is not applied to
  receivers with history.

Exit: one-pixel sparks below 10 per frame static and 20 moving; mean `D_bounce`, specular GI and
final colour each within 1% of the Phase 0 reference; the 20-view ground-truth bias unchanged
within its noise.

## Verification

| Check | Tool | Acceptance |
|---|---|---|
| Sparks | `tools/gi_spark_metric.py`, divisors 1, 2, 4, moving and static | Phase exits above |
| Energy | 4K noise protocol means | Within 1% of Phase 0 |
| Accuracy | `tools/ground_truth_workflow.py run --dataset newsponza/v3` | Per-view `D_bounce` bias change within reference noise |
| Motion | Existing eight-pose orbit/dolly against settled references | Mean error not worse than 0.34/255 |
| Performance | `tools/profile_hybrid_gi.py`, 4K fullscreen preset | Not slower than 27.95 ms median; ForwardTranslucent reported |
| Correctness | ZenTest GPU fixtures: decal on receiver inherits, lifted layer does not; distance-scaled plane limit; wider fallback search; history-less clamp leaves receivers with history bit-identical | Pass in all four submission modes with validation |

## Not planned

- Raising the light falloff floor. It would shorten the tail but changes direct lighting and
  every lamp-lit surface.
- A global firefly clamp. It removes real lamp energy from converged receivers.
- Changes to reservoir weighting. The pairwise MIS code was read for a motion-specific fault and
  none was found; it was not tested in isolation.

## Open decisions

- The working tree already holds the uncommitted ReSTIR, reduced-resolution and cleanup work.
  Decide whether to commit that state before these fixes so each phase is a separable change.
  Resolved: both were committed together, since they share shader files.
- Phase 2's geometric-normal case trades bias for stability; the data from that phase decides.
- The spark thresholds above are proposals drawn from the current counts, not derived limits.

## Results (2026-11-11)

Evidence: `E:/Dev/ZenGroundTruth/restir/spark-20261111` (final runs: `r2-*`). Measurement tool:
`tools/gi_spark_metric.py` (`capture --views fallback rejection length`, `accumulate`, `analyse`).
Diagnostics added: the "GI reconstruction fallback" debug output, and history rejection reasons per
texel (diffuse reason + 16 x reflection reason; face 1 of the rejection and history-length views).

### Sparks per frame (final)

| GI resolution | One-pixel moving / static, Phase 0 | Final | Dots 4+ moving, Phase 0 / final |
|---|---:|---:|---:|
| Quarter | 114.7 / 101.3 | 10.5 / 7.4 | 14.6 / 1.2 |
| Half | 89.4 / 73.7 | 7.4 / 5.2 | 12.8 / 0.5 |
| Native | 230.5 / 55.9 | 18.2 / 2.8 | 4.9 / 0.7 |

Fallback receivers (quarter): 48,264 -> 15,646 px per frame (0.58% -> 0.189%).

### Phase exits

| Phase | Exit | Result |
|---|---|---|
| 1 decals | one-pixel sparks -40% static and moving | Measured alone (Phase 0 plus decal reuse; ring search, tap search and bounds disabled): 114.7 -> 68.0 moving (-41%), 101.3 -> 54.2 static (-46%). Passed |
| 2 fallback | < 0.2% of the frame; fallback-mask sparks -75% | 0.189%; 500 -> 32 moving (-94%), 546 -> 57 static (-90%) over 12 frames. Passed |
| 3 motion dots | < 3 per frame, no more ghosting | 1.2 per frame; eight-pose test 0.293/255 (limit 0.34; 0.314 before the native change). Passed |
| 4 bound | < 10 static, < 20 moving; energy within 1%; accuracy unchanged | 7.4 / 10.5; colour 0.00%, specular GI -0.17%, D_bounce -0.11%; bound on vs off -0.000 points mean, 0.002 max per view. Passed |

### What changed and why

- **Phase 1.** A decal resting on its receiver (plane distance within the reconstruction's limit)
  reuses the receiver's diffuse GI. Reflection reuse also accepts two broad lobes (both at least
  0.5 rough): New Sponza decals are roughness ~1 over receivers at 0.4-0.9, so the plain roughness
  tolerance rejected 86% of them. Against a 256-frame average of the decals' own traced
  reflections, broad reuse is +0.05% of the decal mean (RMS 0.18 levels). Five-view review
  (newsponza/v3 C1-C5, 64-frame averages, final vs. self-traced decals): image means within
  0.004 levels, at most 5 levels at any pixel, no visible change.
- **Phase 2.** Reconstruction searches the rings of 8x8 and then 16x16 windows with the unchanged
  tests before a receiver traces. Before the 16x16 ring, 40% of the remaining fallback receivers
  had a passing texel within 16x16 (the others: plane distance 29%, geometric normal 25%, shading
  normal 6%). The view-distance plane scaling is a no-op here (everything is within one unit).
  Accepting the neighbouring face under a relaxed geometric-normal test was rejected: against the
  256-frame reference it leaked lamp light across edges (1.47% of the mask off by > 8 levels).
- **Phase 3.** History tap search: when every bilinear tap fails, diffuse and reflection history
  continue from the nearest passing previous texel of the 4x4 window. Native resolution now uses
  the reduced-resolution reflection tolerances (`mix(0.99, 0.9, roughness^2)` for the shading
  normal, relative roughness): 42% of the native moving sparks had reflection history rejected for
  the shading normal. Native ghosting 0.221/255 with the change, 0.236 with the strict thresholds.
- **Phase 4.** Samples whose history is shorter than four frames are bounded to 8x a reference:
  - GI texels: previous texels around the reprojection whose history is valid this frame and at
    least four frames long, or current raw neighbours when the reprojection is off-screen. A raw
    reference whenever no history existed bounded the first samples of every history and moved
    D_bounce bias -0.57 points (A/A noise 0.001); it was removed.
  - Fallback receivers: the brightest interpolated neighbour (fallback-mask bias -1.3%, against
    -2.0% for the mean), gathered after tracing.
  - Forward layers: only decals within half a voxel of their receiver. Transmissive and
    free-standing layers stay unbounded: bounding window glass darkened 2,743 pixels by more than
    2 levels (up to 31) in the center view.

### Verification

| Check | Result |
|---|---|
| Accuracy, ReSTIR `phase1` derivative of `newsponza/v3` | bound on vs off: mean -0.000, max 0.002 points; all changes vs the 2026-11-10 build: mean +0.08 points, relative RMS 11.65% -> 8.04%. The largest per-view change is C3_L2, -0.83 -> +0.81 points; no view's absolute bias grew by more than 0.15 points |
| Performance, `tools/profile_hybrid_gi.py`, 4K fullscreen, base clocks | median of three 20.41 ms (20.02-24.07) vs 27.95 ms. ForwardTranslucent 3.27-5.30 ms (was 11.11); HybridReconstructFallback 1.91-2.56 ms (was 2.16); the fallback reference costs about 0.1-0.5 ms (1.83 ms without). The first launch after a shader rebuild is the slow one in each set. Earlier builds were slower in this pass: one that gathered the reference before tracing swung 2.2-15.2 ms between launches, and the next, which already gathered it after tracing, measured 6.1-8.0 ms. The cause of the 6-8 ms was not established; it does not appear in six launches of the final build |
| GPU suites, validation, four submission modes where parameterized | ConeVoxelGI 192, RenderCore 781, RayQuery 108, VulkanRHI 508, EditorRendering 79; no VUIDs or hazards |
| Independent re-check of the final build | Fresh captures reproduce the spark table and the fallback receiver count exactly. Three more Nsight launches: 19.65-21.05 ms, HybridReconstructFallback 1.90-2.30 ms. Static center view, 16-frame mean against the Phase 0 build: +0.01% over the image. The accuracy set and the five GPU suites give the results above |
| Fixtures added | 8x8 and 16x16 ring search and view-scaled plane limit; temporal bound only without history; history tap search; reuse decisions and references through `Calibration/hybrid_reuse_check.comp` (decal rests / lifted / flipped / view-scaled, lobe matching, bound arithmetic, fallback reference excluding missing receivers, forward receiver reference). The forward and fallback passes call the same shared functions; their pass wiring itself has no raster or ray-query fixture |
| Format | 71 changed C++ and GLSL files pass |

Also fixed while verifying: `LightOccluders*SP` and `HybridReconstructFallbackSP` are registered only
on ray-query devices (they failed to create otherwise), and the cold HW-to-Compute editor test now
expects the coarse Compute preview that commit a22f4cdf made the contract.
