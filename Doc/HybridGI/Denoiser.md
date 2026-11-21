# Render pipeline hardening measurements

## Environment shadow repair fix (2026-11-15)

Fast fixed-eye rotation in New Sponza exposed shadowed floor with young GI history. The history-fix
pass mixed lit pyramid samples into valid dark sky estimates on the same surface, then retained
that false light in temporal history. Its widest taps reached about 128 GI pixels; surface/normal
checks could not distinguish a shadow boundary on a continuous floor.

Sky now uses that broad repair only when it has no observation/history (age zero). Valid sky
estimates, including zero radiance, retain their temporal value. Preblur and the output spatial
filter still smooth sky; bounce/reflection keep their existing young-history repair. With anti-lag,
the fallback uses sky's own age, independently of bounce. Missing-sky repair leaves its response age
at zero, so a spatial fallback does not become an accepted observation.

The native `SkyRepairPreservesObservedShadowAndUsesIndependentAge` regression failed on the old
shader in all four submission modes. It passes with the fix, covering missing/young/mature sky,
independent bounce age, and diagnostic/anti-lag variants. All 44 current denoiser integration tests
pass with Vulkan validation. ZenApp, the editor and the integration-test target build successfully.

Matched 1280x720 ZenApp captures use hardware ray queries, native GI, ReSTIR/cache off, environment
on and a 30-degree rotation over ten frames after 128 warm-up frames. The same executable, input
setup and camera path were retained; only the history-fix shader variants changed. At the last
rotating frame, the previously identified 32x32 shadowed patch changed from 4.0433/255 to 0.0915/255
mean RGB in the sky debug image (97.7% less spurious light). Its sky-repair coverage fell from 100%
to zero. With the later spatial filter/stabilizer off, the patch changed from 3.9967 to 0.0042/255.
The fix also works with all three Phase 1 refinements off.

Across the sky image, mean difference from the same final pose held through frame 192 fell from
0.4783 to 0.3528/255; pixels above 4/255 fell from 3.2011% to 1.9936%. These are diagnostic replay
measurements against a settled image, not independent ground truth or full quality/performance
qualification. Other temporal noise remains. Commands, hashes, actual extents/configuration and
before/after measurements are retained in `build/checks/env-pan-fix`, with the original captures in
`build/checks/env-pan-investigation`. Live editor configuration was not changed.

## GI refinement (2026-11-14)

The [GI refinement plan](../GIRefinementPlan.md) adds independently selectable `bicubicHistory`,
`dataDrivenAntiLag`, `youngReceiverSamples` and `denoiserValidation` settings. Corresponding config keys
are `voxel_gi_bicubic_history`, `voxel_gi_data_driven_anti_lag`, `voxel_gi_young_receiver_samples` and
`voxel_gi_denoiser_validation`. Setups serialize each option, and the editor lists them under GI denoiser.
The first three became defaults on 2026-11-14 at the user's request
([denoiser-only path](#denoiser-only-path-2026-11-14)); `denoiserValidation` stays off.

Validation variants dump slow/fast ages, packed reprojection footprint/reason, clamp displacement
in sigmas, and actual repair mip. Hybrid captures now have 18 float4 components; readers retain
13-component support. `gi_noise_probe.py --diagnostic-capture ... --diagnostic-actual ...
--diagnostic-expected ...` reports these by error block. Optional `--diagnostic-traces` joins the
per-GI-texel trace categories. The binary GITR header includes extent and stride. Ray-count mode
retains native GI resolution and disables ReSTIR; those counts are not production ReSTIR counts.

The near-origin regression exercises both sides of a triangle at 0, +/-1e-8, +/-1e-4 and +/-0.03,
including grazing outward rays and an inward hit control, in all four submission modes. It passes:
the production origin already has a positive normal offset before its ULP step, so it is unchanged.

Bicubic history uses twelve validated Catmull-Rom taps and falls back to the existing bilinear
result. Anti-lag independently shortens sky, bounce and reflection ages when clamp movement exceeds
two times spatial plus temporal luminance sigma; CPU hints remain. Young receivers below four
accepted frames take twice the diffuse samples. No anti-firefly change was made without a spark
regression. Feature variants keep the default shader path separate.

Focused HW pose-32 fast-pan measurements (`build/checks/gi-refinement-phase1`) are:

| Option | Pixels above 4/255 | RMS /255 |
|---|---:|---:|
| Baseline | 0.28346836% | 0.63737113 |
| Bicubic | 0.28509597% | 0.63648564 |
| Anti-lag | 0.28348042% | 0.63720998 |
| Young samples | 0.23629196% | 0.57040351 |
| Combined | 0.24095775% | 0.56900912 |

These pilots fail the 0.1994% gate and do not establish full acceptance on HW, Compute or editor.
The anti-lag pilot predates the correction that expresses both spatial and temporal deviations in
luminance units. Its row is evidence for that measured candidate, not a result for the correction.
The young-receiver +0.5 ms moving-trace budget and the complete A/A/flicker/spark/lighting matrix
remain unqualified. Native tests cover validation equivalence and the independent variants;
final verification passes 784 RenderCore cases, 132 ray/field cases, 52 denoiser cases and 11 Python
cases. Existing fixture skips and disabled cases remain listed in the cache ledger.

The 2026-11-14 review reran the full suites (the 52 denoiser cases were a filtered subset of the 168
in ConeVoxelGIIntegrationTest). Every pilot above used the HW RT pipeline with ReSTIR and environment
lighting off. The review also ran each new trace and denoiser variant once on real Vulkan, with
validation:
- ray queries and Compute;
- environment lighting on and off;
- with and without ReSTIR.

All passed with no descriptor or validation errors. That shows the variants run; it is not quality
evidence for them. See [the cache ledger review](RadianceCache.md#review-2026-11-14).

### Phase 1 qualification (2026-11-14)

**Result: no Phase 1 option qualifies.** The user then turned bicubic history, anti-lag and young
receivers on by default anyway, accepting the costs below
([denoiser-only path](#denoiser-only-path-2026-11-14)).

How it was run:
- **Tool:** `tools/gi_quick_gates.py`, 25 minutes in total for five candidates and two A/A runs.
- **Preset:** HW at 3840x2160 fullscreen, GI at 1920x1080, RT pipeline with ReSTIR, environment
  lighting off, accepted-baseline camera.
- **Records:** commands, hashes and per-candidate reports are in
  `build/checks/gi-refinement-phase1-quick` (`summary.json`, `<candidate>/quality/report.json`,
  `<candidate>/nsight/summary.json`).
- **Not run:** Compute and editor, because no option passed HW.

**Single-pose scores are mostly noise.** Delaying the identical run by one or two frames changed
single-pose scores by up to +145%. For example, fast pan pose 32 went from 0.176% to 0.429% of pixels
above 4/255. Repeats under `--fixed-step` are bit-identical, so the hardening suite's A/A band of zero
hid this.

As a result, the hardening gate's pose-8/32/64 pan limits, including the −50% target, can't resolve
changes smaller than about 2x. The rows below therefore use the median over eight poses per motion
(8, 16, … 64). Two frame-shifted options-off runs serve as the A/A.

| Metric | Options off | A/A +1 / +2 | Bicubic | Anti-lag | Young | Combined |
|---|---:|---:|---:|---:|---:|---:|
| Slow pan, median % above 4/255 | 0.136 | 0.136 / 0.154 | 0.137 | 0.136 | 0.119 | 0.118 |
| Fast pan, median % | 0.203 | 0.208 / 0.231 | 0.207 | 0.204 | 0.215 | 0.217 |
| Dolly, median % | 0.122 | 0.125 / 0.125 | 0.122 | 0.122 | 0.116 | 0.116 |
| Static flicker, mean std /255 | 0.0153 | 0.0151 / 0.0150 | 0.0154 | 0.0155 | 0.0151 | 0.0152 |
| Static RMS vs reference /255 | 0.554 | 0.554 / 0.553 | 0.555 | 0.556 | 0.553 | 0.553 |
| Moving sparks, px/frame | 30.9 | 30.8 / 31.1 | 30.8 | 30.8 | 29.8 | 29.9 |
| Light recovery frame 16, % above 4 | 0.0619 | 0.0623 / 0.0607 | 0.0618 | 0.0618 | 0.0629 | 0.0637 |
| Static frame, ms (Nsight, base clocks) | 32.78 | — | 33.28 | 32.78 | 32.95 | 33.66 |
| Eight denoise passes, ms | 6.95 | — | 7.46 | 6.94 | 6.93 | 7.53 |
| HybridTrace during 1°/frame pan, ms | 8.65 | — | — | — | 11.51 | 11.64 |

Dispositions:
- **Bicubic history: rejected.** Motion error is unchanged; mean channel error is 2–3% lower than
  all three options-off runs. It costs +0.51 ms of denoise, which breaks both the 7 ms denoise budget
  (7.46 ms) and the 33.018 ms frame budget (33.28 ms).
- **Data-driven anti-lag: not qualified.** It changes nothing measurable, light recovery included,
  and costs nothing. The light-path move also sets the CPU response flag, so this test does not
  exercise the flagless lighting changes anti-lag exists for. No current test does.
- **Young receivers: rejected on cost.**
  - Gains: slow-pan median −13% and dolly −5%. It is lower than options-off at all eight poses of
    both motions, and below all three options-off medians.
  - Fast pan: within the A/A spread.
  - Cost: `HybridTrace` gains +2.86 ms during the pan against a +0.5 ms budget, and the pan frame
    gains 2.7 ms.
  - Even its gain is far from the −50% target.
- **Combined: rejected.** It adds bicubic's and young receivers' costs and gains nothing more.
- **Anti-firefly: not built.** No option worsened sparks. It was built on 2026-11-15 after the
  [moving-spark split](#moving-spark-split-2026-11-15); see [anti-firefly](#anti-firefly-2026-11-15).

**Moving sparks predate Phase 1.** Options-off moving sparks are 30.9 px/frame here, and 35.2 in the
old harness.

The 13.75 limit is a hardening Phase 0 value, measured with ray queries and the previous denoiser.
The accepted RT baseline switched to the RT pipeline and `relaxDenoiser`, and `gates-amended.json`
amended only its frame budget, so the RT path never had a spark limit of its own. Static sparks fell
from 5.33 to 0 across the same change. The cause of the moving increase is not isolated. Where
the current moving sparks come from is in the [moving-spark split](#moving-spark-split-2026-11-15).

The light-response result depends on the harness. Options-off passes the plan's limits here
(0.0619% and 0.349/255 against 0.0807% and 0.3973/255). The old windowed light tool scored the same
build at 0.0880%, which fails.

### Denoiser-only path (2026-11-14)

At the user's request after the Phase 1 qualification, the ReLAX-style denoiser became the only GI
denoiser. Its three Phase 1 refinements became defaults, although the screen above rejected them on
cost.

Removed:
- **Passes:** `HybridTemporal` except its reference variant, `HybridFilterPrepare`, `HybridFilter0`–`4`,
  `HybridSkySupport`, `HybridSkySearch` and `HybridReconstructFallback`.
- **Shaders:** `hybrid_temporal.comp`, `hybrid_temporal_cached.comp`, `hybrid_filter*.comp/.glsl`,
  `hybrid_sky_search.comp`, `hybrid_sky_support*.comp/.glsl`, `hybrid_sky_guide.glsl`,
  `hybrid_reconstruct_fallback.comp` and `hybrid_fallback_reference.glsl`.
- **Trace output:** the full-resolution sky guide, which only the old filter read. The trace no
  longer computes it or reads the environment harmonics.
- **History:** the `SearchWeight` texture.
- **Setting:** `relaxDenoiser`. `voxel_gi_relax_denoiser` is ignored with a log line. Setups that
  contain `relaxDenoiser` still load and ignore it; the field is no longer written. ZenApp's frame
  summary reports `giDenoiser` and `giSpatialFilter` instead of `relaxDenoiser`.

Kept:
- `HybridTemporalReferenceSP` for the reference diagnostic (`referenceSamples`), a plain mean without
  spatial filtering. Bounce moments and the previous-cache radiance remain for it only.
- `filter`, with a new meaning: off skips the three a-trous passes and output stabilization, while
  temporal accumulation and history fix still run. Before, off selected the old path's unfiltered
  temporal.

Behaviour changes:
- Toggling reflections resets GI history, because the denoiser's signal layout includes the
  reflection layer. The old path kept history.
- At reduced resolution, receivers the reconstruction cannot support take the nearest history-fixed
  texel on both tiers. On the old path HW traced them again in the fallback pass.
- The trace never stores previous-cache radiance outside the reference mean. Compute still copies the
  radiance snapshot each injection for that diagnostic.
- `rt-accepted-baseline-20261114/capture-1/setup.zenrender` has no Phase 1 keys, so it now renders
  with them on and no longer reproduces the accepted 32.83 ms frame.

### Moving-spark split (2026-11-15)

**Result: moving sparks are bright samples from the trace and ReSTIR, shown on texels whose history
was just reset.** History fix clamps only at age 4 or more, so no pass removes them on those texels.

How it was run:
- **Tools:** `build/checks/spark-split/split.py` and `age_baseline.py`, about 6 minutes.
- **Preset:** HW at 3840x2160 fullscreen, GI at 1920x1080, RT pipeline with ReSTIR, environment
  lighting off, accepted-baseline camera, Phase 1 options on.
- **Path:** 256 static frames, then the quick-gates moving window: 16 frames alternating between the
  base pose and +0.5°. A spark rises more than 48/255 above the same pose two frames before and after.
- **Records:** `report.json`, `age-baseline.json`, `ages.npz` and `default-overlay.png` in that folder.

| Run | Spark px/frame | Sparks per frame: 1 px / 2–3 px / 4+ px |
|---|---:|---:|
| Default | 32.2 | 9.75 / 3.58 / 1.67 |
| Default, 2 frames later (A/A) | 39.9 | 9.67 / 4.33 / 1.92 |
| ReSTIR off | 133.3 | 10.08 / 3.50 / 2.58 |
| Spatial filter off | 120.8 | 15.58 / 14.17 / 10.50 |
| Native GI (3840x2160) | 57.6 | 35.17 / 6.58 / 1.33 |

The default run's 180 sparks were joined with GI debug views of the same frames, each region widened
by 3 px:
- **Signal:** 90% are in the denoised reflection and 66% in the denoised bounce, against a chance
  overlap below 0.05%. 4% are in neither.
- **Origin:** 76% and 71% coincide with a bright outlier in the raw reflection and bounce entering
  the denoiser, against 0.9% and 2.9% by chance.
- **Age:** 98% touch reflection history and 92% touch diffuse history younger than 4 frames, against
  20% and 8.5% of random regions of the same size. At the spark centre, 66% and 53% are young
  against 3.8% and 1.8% of random pixels. The median centre age is 1 frame; the image median is
  128, the cap.

ReSTIR and the spatial filter each cut spark pixels about fourfold, and reduced resolution is not
the source. This harness is noisier than the quick-gates moving sparks: its A/A differs by 24%.

Sparks cluster at the screen edges, which the rotation reveals, and on interior arches and window
tracery. The rotation is about the eye, so the interior resets are not disocclusion; the
[rejection codes](#interior-resets-during-rotation-2026-11-15) show what resets them. The
[anti-firefly](#anti-firefly-2026-11-15) built on this result removes 72% of moving sparks.

### Interior resets during rotation (2026-11-15)

**Result: the shading-normal and geometric-normal history checks reset interior texels whenever the
view turns.** Most of the original sparks sit on those texels.

Why it happens:
- **Point-sampled receivers:** each reduced-resolution GI texel reads one fixed full-resolution pixel
  of its 2x2 block (`hybrid_downsample.comp`), so it holds one point on the surface.
- **Shifting points:** the GI grid stays fixed on screen. After a turn, the previous texels that
  surround a reprojected texel sampled other points of the same surface.
- **Per-tap normal checks:** `GIDenoiseTemporal` rejects each of the four bilinear taps whose normal
  differs too much. The shading-normal threshold is cosine 0.9 for diffuse and up to 0.99 (about 8°)
  for glossy reflection; the geometric-normal threshold is 0.9 for diffuse and 0.99 for reflection.
- **Full reset:** normal-mapped stone and faceted or creased geometry fail all four taps. With no
  wider search, the texel loses its history on every frame of the motion.
- **Control:** when the camera holds still, the taps are the texel itself and nothing is rejected.

Measured with `build/checks/spark-split/rejections.py`: the spark-split path with `antiFirefly` off
and the validation view's rejection codes (face 5), about 2 minutes. Records are in
`build/checks/spark-split/rejections`, including reset-reason images over the spark frame.

| Texels reset per frame, % of all pixels | Bounce | Reflection |
|---|---:|---:|
| Outside the previous view (screen-edge band) | 0.99 | 0.99 |
| Shading normal | 0.57 | 1.63 |
| Geometric normal | 0.23 | 0.74 |
| Roughness | — | 0.27 |
| Identity and plane | 0.09 | 0.15 |
| Camera held still (control) | 0 | 0 |

- **Sparks:** of the 180 original sparks, 135 (bounce) and 139 (reflection) sit on texels reset by a
  normal check; 19 and 18 are in the screen-edge band.
- **Normal maps off:** reflection shading-normal resets fall to 0.85% and sparks fall from 32.2 to
  20.7 px/frame. Geometric-normal resets are unchanged, so normal maps are only part of the cause.
- **Images:** red marks shading-normal resets, which are speckled over normal-mapped stone. Yellow
  marks geometric-normal resets, which follow arch edges, mouldings, column flutes and window
  tracery.


### Anti-firefly (2026-11-15)

**Result: moving sparks fall 72% at no measurable cost, with no darkening.** It is on by default.

What it does:
- **Where:** in `GIDenoiseHistoryFix`, on texels under four frames old, which have no slow history
  for the existing clamp.
- **How:** each signal's YCoCg luma is limited to the brightest fast-history luma among the texel's
  matching 5x5 neighbours, keeping its hue. The texel itself is excluded, so a lone outlier cannot
  widen its own bound. A texel with no matching neighbour is unchanged.
- **Inputs:** the shared tile the slow-history clamp already loads, so it adds no memory reads.
- **Controls:** setting `antiFirefly`, config key `voxel_gi_anti_firefly`, editor checkbox
  "Anti-firefly on newly revealed surfaces". The programs are
  `GIDenoiseHistoryFix{Validation}{AntiLag}AntiFirefly{NoSky}SP`.

How it was measured:
- **Tool:** `tools/gi_quick_gates.py`, 15 minutes, records in `build/checks/anti-firefly/quick`.
- **Preset:** the Phase 1 screen's HW preset, with Phase 1 options on. "Off" sets
  `antiFirefly=false`; the A/A runs repeat "on" 1 and 2 frames later.
- **Pose metrics:** pan and dolly values are means over eight poses. The Phase 1 table above uses
  medians, so the two don't compare directly.

| Metric | On (default) | A/A +1 / +2 | Off |
|---|---:|---:|---:|
| Moving sparks, px/frame | 8.42 | 8.25 / 7.67 | 29.83 |
| Sparks per frame, 1 / 2–3 / 4+ px | 3.50 / 1.17 / 0.33 | 3.50 / 1.08 / 0.33 | 8.50 / 4.92 / 1.33 |
| Slow pan, mean % above 4/255 | 0.105 | 0.100 / 0.108 | 0.122 |
| Fast pan, mean % | 0.186 | 0.145 / 0.133 | 0.223 |
| Dolly, mean % | 0.106 | 0.107 / 0.108 | 0.114 |
| Static RMS vs reference /255 | 0.552 | 0.553 / 0.554 | 0.554 |
| Static flicker, mean std /255 | 0.0152 | 0.0151 / 0.0148 | 0.0152 |
| Light recovery frame 16, % above 4 | 0.0603 | 0.0614 / 0.0580 | 0.0607 |
| `GIDenoiseHistoryFix`, ms static / pan | 0.807 / 1.132 | — | 0.791 / 1.147 |
| Frame, ms static / pan (Nsight, base clocks) | 34.62 / 40.63 | — | 35.01 / 40.45 |

Dispositions:
- **Sparks:** −72%, outside the A/A spread. Each size class falls 59–76%.
- **Motion:** slow pan −14% and dolly −7%, both outside the A/A spread. Fast pan is −17%, inside it.
- **Bias:** static RMS, flicker and light recovery are unchanged, so it does not darken the image.
- **Cost:** history fix moves by under 0.02 ms. The frame differences are run-to-run noise: passes
  whose code is identical between on and off moved by up to 0.4 ms.

Verification:
- **Native regression:** `AntiFireflyLimitsYoungOutliersToMatchingNeighbours` runs all eight
  history-fix variants on young and mature texels, with a foreign surface and an isolated receiver,
  in four submission modes.
- **Suites:** all nine pass; ConeVoxelGI now has 164 cases.
- **Real Vulkan:** ten configurations with validation, in `build/checks/anti-firefly/variants`.

Open:
- Both static frames are about 1 ms above the Phase 1 capture of the same settings (33.66 ms).
  Most of the difference is in passes unchanged here. `HybridTrace` is 0.19–0.27 ms higher, about
  twice its run-to-run spread; not investigated.
- The light-path move sets the CPU response flag, as in the Phase 1 screen.
- The remaining 8.4 px/frame are not attributed.
- Compute and the editor preset were not measured.

### Temporal 3x3 history search (2026-11-15)

**Result: a partial fix.** It keeps history on 30% of the interior texels that rotation used to
reset. The other 70% still reset, because their whole 3x3 neighbourhood fails a normal check too.

What it does:
- **When:** `GIDenoiseTemporal` searches only when every bilinear tap failed and no tap failed the
  identity or plane check, so a disocclusion still resets. The reprojected texel must also lie
  inside the previous view, so the screen-edge band still resets.
- **How:** it applies the same checks to the 3x3 previous texels around the reprojected position.
  Diffuse, reflection and visibility history each search only if their own taps failed. Accepted
  texels are weighted equally.
- **Diagnostics:** the validation footprint nibble records 3 for history found by the search.
- **Controls:** none. It always runs.

Measured on the same paths as the [interior resets](#interior-resets-during-rotation-2026-11-15) and
the [anti-firefly](#anti-firefly-2026-11-15) runs, comparing builds before and after the change.
Records are in `build/checks/spark-split/rejections-search` and `build/checks/temporal-search`.

| Metric | Before | After |
|---|---:|---:|
| Interior bounce resets, % of pixels (normal, identity and plane) | 0.88 | 0.61 |
| Interior reflection resets, % (normal, roughness, identity and plane) | 2.79 | 1.96 |
| Screen-edge resets, % | 0.99 | 0.99 |
| Moving sparks, anti-firefly off, spark path, px/frame | 32.2 | 28.4 |
| Moving sparks, anti-firefly on, quick gates (A/A +1 / +2) | 8.42 (8.25 / 7.67) | 6.50 (6.50 / 6.25) |
| Dolly, mean % above 4/255 (A/A +1 / +2) | 0.106 (0.107 / 0.108) | 0.109 (0.110 / 0.110) |
| `GIDenoiseTemporal` static, ms (two captures) | 1.09 / 1.24 | 1.25 / 1.40 |
| `GIDenoiseTemporal` pan, ms (two captures) | 1.41 / 1.41 | 1.38 / 1.47 |

- **Unchanged:** slow and fast pan, static RMS and flicker, and light recovery are within the A/A
  spread.
- **Dolly:** about 2% worse in all three runs.
- **Cost:** the static temporal pass costs +0.16 ms in both captures, although a still camera never
  runs the search. The larger shader is the likely cause; register counts were not checked.

The remaining resets come from the normal checks, which compare the normals of two different point
samples. On normal-mapped stone and faceted arches, nearby samples rarely agree within the reflection
threshold (cosine 0.99 to 0.9 by roughness), or within cosine 0.9 for diffuse.

**Loosening a check was tried and reverted.** Reflection history used a fixed geometric-normal
threshold of cosine 0.99; lowering it to 0.9, as for diffuse, was measured on top of the search.
Records are in `build/checks/spark-split/rejections-geometric` and `build/checks/temporal-geometric`.

| Mean of the run and its two A/A copies | Search | Search and reflection geometric 0.9 |
|---|---:|---:|
| Interior reflection resets, % of pixels | 1.97 | 1.31 |
| Moving sparks, anti-firefly on, px/frame | 6.42 | 6.22 |
| Dolly, mean % above 4/255 | 0.109 | 0.118 |
| Slow pan, mean % | 0.105 | 0.109 |
| Fast pan, mean % | 0.155 | 0.157 |

- **Effect:** a third of the reflection resets were removed, but dolly error rose 7.5% and slow-pan
  error 3.6% in all three runs, while sparks did not change beyond noise.
- **Conclusion:** the reused history was less accurate than a reset, so the 0.99 check was doing
  useful work. The diffuse shading-normal check also stays, because it matches the spatial filter's
  0.9 threshold.
- **Not removable:** in this setup, the remaining resets are a limit of point-sampled
  reduced-resolution GI. Where normals turn within a texel (column silhouettes, rounded edges, thin
  mouldings, normal-mapped bumps), no previous texel sampled the same orientation, so no valid
  history exists.
- **Reverted state:** the shaders and executable hash the same as the measured search build.

### Footprint receivers (2026-11-15)

**Result: motion error falls 16–24% and history resets about a third, and panning costs 0.6 ms less.
In exchange, the converged image moves 2% further from the CUDA reference.** It is on by default; the
user kept it on after these measurements (2026-11-15).

What it does:
- **Point sample kept:** `HybridDownsample` still takes depth, position, identity, motion and the
  geometric normal from one representative pixel per block.
- **Normal:** the texel's shading normal is the normalized mean over the block's pixels on the
  representative's face (same identity, geometric normals within cosine 0.9).
- **Roughness:** the block's mean GGX alpha² plus the spread of those normals, from a von Mises-Fisher
  fit: alpha² += 2 (1 - r²) / (3r - r³), where r is the mean normal's length.
- **Reconstruct:** the representative's own roughness is stored in the roughness texture's unused
  alpha channel, and reconstruct matches materials against it.
- **Controls:** setting `footprintReceivers`, config key `voxel_gi_footprint_receivers`, editor
  checkbox "Footprint-filtered GI receivers". The program is `HybridDownsampleFootprintSP`. Toggling
  it resets history; native-resolution GI is unaffected.

How it was measured:
- **Tools:** the quick gates and reset reasons, with records in `build/checks/footprint-receivers`
  and `build/checks/spark-split/rejections-footprint`.
- **Baseline:** footprint off renders the same images as the previous build. Its moving sparks match
  exactly (6.5), so the motion and light rows use each build's run against its own converged
  reference, means of the run and its two A/A copies.
- **Confound:** the off column of `footprint-receivers/quick` is scored against the footprint-on
  reference, so its motion and light rows are not comparable.
- **Cost:** compared within one session. The off static capture is an outlier (reconstruct 2.59 ms
  against its usual 1.15–1.19), so the previous build's capture is shown too.

| Metric | Footprint off (previous build) | Footprint on |
|---|---:|---:|
| Interior bounce resets, % of pixels | 0.61 | 0.43 |
| Interior reflection resets, % of pixels | 1.97 | 1.29 |
| Slow pan, mean % above 4/255 | 0.105 | 0.089 |
| Fast pan, mean % | 0.155 | 0.163 |
| Dolly, mean % | 0.109 | 0.084 |
| Light recovery frame 16, % above 4 | 0.059 | 0.044 |
| Moving sparks, anti-firefly on, px/frame | 6.42 | 7.94 |
| Moving sparks, anti-firefly off, spark path | 28.4 | 28.2 |
| Static RMS vs CUDA reference /255 | 0.554 | 0.565 |
| Static flicker, mean std /255 | 0.0152 | 0.0156 |
| Reconstruct fallback, % of receivers | 0.069 | 0.067 |
| `HybridTrace`, ms static / pan | 8.64–8.67 / 11.43–11.53 | 8.52 / 10.84 |
| `ReSTIRSpatial0` + `1`, ms static | 6.95–6.96 | 7.09 |
| Frame, ms static / pan | 33.88 / 38.96–39.49 | 33.37 / 38.43 |

- **Motion:** slow pan −16%, dolly −24% and light recovery −25%, each outside the A/A spread. Fast
  pan (+5%) is inside its ±25% A/A spread.
- **Static accuracy:** static RMS against the CUDA reference is 2% worse, outside its ±0.3% A/A
  spread. Averaged normals and widened roughness soften bump detail that the per-pixel reference
  keeps.
- **Sparks:** with anti-firefly on, sparks rise from 6.42 to 7.94 px/frame, at the edge of the
  on-build's own A/A spread (6.8–8.5). With anti-firefly off they are unchanged.
- **Cost:** the trace is 0.6 ms faster while panning, because fewer resets mean fewer young texels
  taking doubled diffuse samples. ReSTIR spatial costs +0.13 ms; downsample is unchanged at 0.32 ms.

The 4096-sample CUDA reference at `build/checks/gi-refinement-phase0/multibounce/reference` uses the
retained one-bounce reference's exact receiver positions and normals, with up to 16 diffuse vertices.
It measures finite-depth multi-bounce light, not an engine feature or proven infinite-bounce truth.
All-receiver means are 0.00500527 (one bounce) and 0.01171126 (16 vertices), a 57.26% deficit;
floor means are 0.00437968 and 0.01164619, a 62.39% deficit. Independent multi-bounce half means differ
by 0.00001856 overall and 0.00003926 on the floor. Per-pixel variance remains substantial. The old
raw engine capture/environment binaries were unavailable; the tool explicitly uses the retained
CUDA NPZ and labels that comparison. The engine multi-bounce decision remains deferred.

The accepted-baseline manifest comparison is in
`build/checks/gi-refinement-phase0/accepted-module-comparison-final.json`: non-count, non-capture production
modules were byte-identical after variant isolation. This is shader evidence, not an assertion of
an unrun 4K image comparison against the old executable. The previous executable was not copied.
See the [cache ledger](RadianceCache.md) for Phase 2 and its remaining gates.

Implementation and acceptance follow [the hardening plan](../RenderPipelineHardeningPlan.md).
The renderer has not yet switched denoisers. Phase 0 records the existing SVGF chain before
any feature removal; subsequent phases must retain these inputs and report their gates here.

## Phase 0 protocol

Run `build/venv/Scripts/python.exe tools/gi_noise_probe.py --setup=<half.zenrender>
--output=<new-directory>`. The default runs each of three presets twice. The tool writes
derived setups and exact ZenApp arguments, fingerprints the executable, shaders, source
assets, setup and reference, and checks the actual capture state. Validation is enabled for
quality captures. Timings come from `tools/profile_hybrid_gi.py`, with validation disabled,
after a warm launch. No performance measurements use the editor or demo executable.
At the user's request, subsequent application runs reuse the canonical
`build/x64-windows-msvc-release/bin/ZenApp.exe` path. Rebuilds replace that binary
in place; captures retain hashes and arguments without new executable copies.
Earlier runtime archives remain historical evidence for their recorded runs.

Frozen presets (other scene, material, light and exposure settings come from the input setup):

| Preset | Providers | GI extent at 4K | Diffuse resolution | ReSTIR |
| --- | --- | --- | --- | --- |
| hardware | All HW | 1920Ã—1080 | Full relative to GI extent | On |
| compute | All Compute field | 3840Ã—2160 | Auto (half diffuse after history becomes valid) | Off |
| editor | All HW | 3840Ã—2160 | Full | Off |

All use pixel gather, temporal accumulation, filtering, 32-frame history and four base
samples. Adaptive sampling, previous-frame radiance and the reference estimator are off.
Compute voxelization and async compute are on. These are frozen measurement presets;
the editor row describes its native-resolution/ReSTIR-off baseline, not a change to defaults.

- Static flicker: frames 289â€“320; population standard deviation of the maximum RGB channel
  at each pixel. The promoted tool uses online float64 accumulation to avoid a 3 GB stack.
- Pan: hold the input camera through frame 160; rotate its target around its fixed eye by
  0.25Â° or 1Â° per frame. Compare frames 168, 192 and 224 with each identical pose settled for
  256 frames. Both path and process continue to frame 225.
- Dolly: translate eye and target together along the view direction, by 0.5% of `sceneScale`
  over 64 frames. Use the same capture and settling schedule.
- Sparks: the protocol from `gi_matched_sparks.py` / `gi_spark_metric.py`: 128-frame warmup,
  16 captured frames, alternating fixed-eye poses 0.5Â° apart (and a static control), threshold
  48/255, with 12 analysed frames and 8-connected components.
- Static reference: frame 320 versus the masked `static/center/reference-mean.npy` in
  `E:/Dev/ZenGroundTruth/restir/matched-4k-20261112-v2`, using its existing weighted RMS metric.
  This is the sparse ZenApp Monte Carlo reference described by that dataset's `protocol.json`,
  **not an independent Mitsuba reference**. The original source setup matches its center setup.
- RDG: a separate 320-frame ZenApp run retains bounded node details, queues and GPU timestamps.
  The existing logger caps details at 32 nodes; `details_omitted` must be checked. The Nsight
  `D3DPERF_EVENTS.xls` export supplies the complete ordered GPU pass labels beyond this cap.
  GPU Trace: two captures per preset via the existing Nsight runner. Overlapping pass
  durations must not be summed to claim total frame time.

## Last-pose anomaly investigation

The original `build/gi-noise-20261113/noise_probe.py` called a target-centred **orbit** a pan.
Its eye moved 0.03424, 0.13685 and 0.27304 scene-coordinate units at poses 8, 32 and 64.
The corresponding saved moving images contain 6.56%, 6.16% and **45.84%** completely black
pixels. Pose 64 visibly intersects nearby geometry and exposes back-facing surfaces.
Its nearly zero motion error is not a valid denoiser quality sample.

Endpoint control: `build/render-hardening-phase0-endpoint` renders the corrected fixed-eye
pan twice with identical history through frame 224. One path/process ends at 224; the other
continues to 225. The frame-224 images differ by one channel value in one pixel:
RMS 0.00020047/255, maximum 1/255, **0% over 4/255**. Both runs completed with validation
enabled and no validation errors. This rules out a material last-key/capture discrepancy
for the corrected protocol. Old orbit metrics must not be mixed with the new pan baseline.

## Qualification status

Suite sizes below are the registered GTest counts, including disabled tests.
For the current 132-case ray suite, 116 pass, four fixture-dependent lantern
experiments skip, and twelve experiments remain disabled. The 785-case core
suite runs 776 passing tests with nine disabled; the focused denoiser suite
runs all 52 tests. Disabled or skipped cases are not verification evidence.

Phase 0's full capture matrix is complete in `build/render-hardening-phase0`, with
unchanged executable/shader/input hashes throughout the run. `gates.json` freezes the
two-repeat bounds, including the 50% pan-reduction requirement. All capture validation
checks passed. The original native suites passed 124 ray/field, 192 GI and 513 Vulkan
integration tests with zero Vulkan validation errors; their parameterized cases cover
all four submission modes. Twelve experimental ray/field cases are disabled.
The six Phase 1 removal groups are implemented and archived in order. The strict
Compute image-equivalence gate remains open. The optional Phase 2 denoiser passes
the static flicker/RMS limits once on all three presets; the 50% motion reduction,
repeated measurements, sparks and 7 ms budget are not qualified. Phase 3's RT
foundation is implemented and validated. Phase 4 has implemented the approved
forward GI fallback, receiver shadows, ReSTIR visibility and the light-occluder
cache pipeline. The primary GI pipeline, environment-guide bridge and counted
variant are implemented; native guide and sampling checks pass. Phase 4 timing
qualification is in progress. Defaults remain unchanged; Phases 5 and 6 are open.

The latest retained revision also compacts receiver visibility without changing
the 32 HW static images. Its production capture is
`build/render-hardening-phase4-compact-visibility`: 6.977102 ms for
eight denoiser passes, 34.10240 ms whole frame and 9,622 MiB committed VRAM.
Static limits pass once; focused fast-pan error improves to 0.283722%, still
above its 0.199424% gate. The private qualification candidate meets the
50% reduction at fast-pan poses 8 and 64, but not the other four measured
HW pan poses. These are progress measurements, not completion of the plan.

Both HW repeats have identical measured quality metrics: mean static flicker
0.029419/255; static RMS 0.556593/255; slow-pan pixels over 4/255 at poses 8/32/64:
0.23505%, 0.25998%, 0.19989%; fast-pan: 0.37471%, 0.39885%, 0.40803%.
Warm Nsight whole-frame medians: 31.7262 and 31.72175 ms, each across 30 frames
at base clocks. The first run's eleven denoise/reconstruction pass medians sum to 8.5721 ms; that
sum is a stage-cost diagnostic, not total frame time. The plan's older approximately
14 ms estimate came from a different RDG measurement and is not this run's timing baseline.

Compute static repeats measure 0.08725427 and 0.08725530/255 mean flicker, with static
RMS 3.38677437 and 3.38678516/255. Its unchanged executable does not produce strictly
identical captures: frame 320 differs at 1,390 pixels (out of 8,294,400), maximum 1/255,
RMS 0.00749550/255. Slow-pan frame 224 differs at 1,534 pixels, maximum 2/255,
RMS 0.00787973/255. These are observed repeat variations; their cause is not isolated.
Compute whole-frame medians are 85.18365 and 83.34685 ms.

Both editor quality repeats match: static flicker 0.298338/255, static RMS 1.175094/255,
slow-pan pixels over 4/255 at poses 8/32/64: 2.12681%, 2.10340%, 1.79083%; fast-pan:
2.33513%, 1.55448%, 1.92638%. Whole-frame medians are 65.07820 and 65.29395 ms.
Moving-spark pixels per analysed frame are 13.75 for HW, 2.66667 for Compute and
247.08333 for the editor preset; both repeats match in every spark-size class.

The original executables and SPIR-V are archived under the baseline directory's `runtime/`.
Before rebuilding, the original binaries passed 21 RuntimeLaunch and 790 RenderCore tests
(nine RenderCore tests are disabled). Vulkan RHI passed 85 of 86 tests; its source-walking
shader-layout test saw the new generic readback shader source before its SPIR-V was
built. The complete rebuilt suite remains required. Python tool checks pass 35 tests.

The Compute captures still contain background local-field construction. The main field
and the first local fields publish, but generation 4 remains building during the sampled
frames. Both GPU traces include construction ranges; the second reports some ranges
spanning almost the whole frame. These are the current renderer's measured frames, not
a claim that all local fields are settled. `DistanceFieldScene::NextCycleCount` reduces
its triangle batch toward one whenever total frame time exceeds its 16 ms target, even
when receiver rendering alone exceeds that target. Construction progress and final
steady-state timing need explicit coverage in the hardening phase.

`tools/gi_noise_gates.py --baseline=<directory> --output=<gates.json>` requires two
complete repeats of all six measurements for all three presets. Static RMS may rise by
at most the observed A/A difference beyond the worse baseline repeat; flicker, sparks,
dolly error and whole-frame median must not exceed the worse repeat. With `--denoiser`,
each pan pose must improve by at least 50% against the better baseline repeat at both
speeds. Add `--candidate=<directory>` to evaluate a later run. Missing data, nonfinite
measurements or mismatched scene/reference inputs reject the comparison. A successful
report covers only its stated metrics: it does not certify feature-removal byte identity,
the eight-pass/seven-millisecond budget, or the GPU/editor suites.

## Phase 1: screen-probe removal

The prepared patch removes `ScreenProbeGather`, its shared layout, the 34 probe shaders,
the probe debug shader, program registrations, gather/spacing settings and UI. Old config
keys are ignored by the existing loader; the local `Data/engine.cfg` is left intact.

Removed tests belong to the removed implementation: twelve probe shader integration
cases, three probe allocation/history/pool RenderCore cases, one curved-geometry probe
comparison in the editor, one gather-selection UI case and one gather/spacing config case.
The field-resolution validation test remains. Six existing pixel-GI temporal,
reconstruction and reuse tests move from `ScreenProbeGatherIntegrationTests.inl` to
`HybridDenoiseIntegrationTests.inl`, retaining their four submission modes. Their generic
texture-readback calibration shader is renamed accordingly. Other tests stop selecting
the removed gather and continue to cover their original rendering behavior.

Active measurement tools no longer offer a gather selector, and the example config
no longer contains gather/spacing keys. Eighty-six obsolete compiled probe modules and
dependency files were removed after baseline tests completed; their originals and the
capture helpers are archived with the baseline. The full release build passed. Verification
passed 21 RuntimeLaunch, 786 RenderCore, 86 VulkanRHI, 18 UIDrawPacket, 58 EditorModel,
144 GI integration and five UIRendering tests, plus five native shader-registration cases.
The editor suite passed 75 cases initially; two more passed after initializing both pixel-GI
guide buffers through the upload queue. The missing first-frame initialization was exposed
by removing the editor's probe path. Two editor and nine RenderCore cases remain disabled.
Validation reported no errors. Logs are in `build/checks/hardening-probe-tests` and
`build/checks/hardening-probe-init-tests`.

The strict capture result is retained as **not byte-identical** in
`build/render-hardening-phase1-probes-v2/report.json`. All surviving shader binaries
match the original hashes. HW slow pan and dolly match exactly; two fast-pan poses differ
at one pixel by 1/255. HW static images differ at at most 137 pixels, maximum 1/255,
RMS 0.00234643/255. The archived original executable, rerun with its original setup and
shader modules, reproduces the same static drift: 25 of 32 frames match the new executable
exactly; the others differ at at most one pixel by 1/255. This is repeat variation,
not a probe-removal lighting change. That control is retained in
`build/render-hardening-phase1-probes-original-control`; temporarily restored probe
modules were removed again afterward.

Compute static and fast-pan differences are small (maximum RMS 0.00810103 and
0.00928454/255). Slow pan and dolly instead differ by up to RMS 0.510473 and
0.417455/255. Both publish the main field after **56** graphs rather than the baseline's
55. The throughput builder uses elapsed frame time, while `HybridGIRenderer::m_frame`
advances only for submitted GI frames; a later field publication shifts the stochastic
sequence, including the half-diffuse rotating leader. These are expected startup-phase
differences, recorded separately from image equivalence. They are not a denoiser quality
pass. The later field-progress and deterministic-measurement work must address this
startup dependency; the frozen baseline remains unchanged.

`tools/gi_removal_gate.py --baseline=<phase0-directory> --setup=<half.zenrender>
--output=<new-directory>` replays the original static interval and three motion paths
through ZenApp for HW and Compute. It compares all 32 static images and the nine sampled
moving images per tier, verifies baseline capture hashes and matching scene inputs, and
retains changed-pixel counts, RMS and heatmaps. Any pixel difference fails its strict
identity result; expected differences must be reviewed and documented separately.

## Phase 1: cone-GI removal (qualification in progress)

Removed cone bounce, its shaders and dedicated composition programs, `BounceSource`,
aperture/step/distance/count settings, their config/setup fields and the cone environment
test (four submission-mode instances). Normal bias remains because field and cache rays
use it. Legacy sky-cache cone helpers remain only until the ordered legacy-cache removal.
Twelve obsolete compiled modules/dependency files were deleted; originals remain archived.

Cold Compute preparation now renders PBR/environment lighting and keeps submitting field
construction. The temporary status explains the fallback and clears when preparation
advances. No voxel radiance is injected until the field is usable. Tests cover cold startup,
HW-to-Compute switching and scene replacement without freezing construction.

Unsupported Compute forward layers and diffuse-transmission scatter use material
environment lighting instead of cone walks. Supported receivers keep reconstructed GI;
blending, refraction, transmission attenuation and per-layer shadows are unchanged.
This is an expected lighting difference under the user's forward-GI fallback decision.
HW forward diffuse/specular ray queries stay until Phase 4. Capture outputs read traced
and reconstructed bounce directly, with no cone estimate or voxel binding.

The release build passes, as do 786 RenderCore tests, 35 Python checks and 140 existing
GI integration tests across four submission modes, including the forward blend/core
regressions, with zero Vulkan validation errors. The completed editor run has 75 passes
and two stale scene-replacement counter expectations; both pass after correction.
The new capture regression passes in all four submission modes. UI rendering and native
shipped-program checks also pass. Logs are in `build/checks/hardening-cone-verified` and
`build/checks/hardening-cone-final-tests`.

Removal captures are in `build/render-hardening-phase1-cones`. HW static, slow pan and
dolly are byte-identical to Phase 0. Two fast-pan images differ by at most 1/255
(at most 3,904 pixels, RMS 0.012566/255). Compute differs by up to RMS 0.573258/255,
including the documented forward environment fallback and a changed construction
schedule: the main field is ready after 40â€“42 graphs instead of 55â€“56, and generation 4
now completes after 11 graphs. With cheaper PBR startup, the adaptive build budget no
longer stalls that generation. The baseline left it unfinished. This changes stochastic
phase and the available local field, so strict byte identity fails as expected. These
captures do not establish later denoiser quality gates.

Warm Nsight captures in the same directory use ZenApp, 4K fullscreen, async compute,
base clocks, a 60-second delay and 30 frames. HW whole-frame median is 31.99255 ms;
`ForwardTranslucent` is 3.380175 ms and `SceneLighting` is 5.722845 ms. These exceed
the frozen HW baselines, so the Phase 1 timing gate remains open. Compute whole frame
is 84.98205 ms (within the baseline range), and `ForwardTranslucent` is 1.09069 ms
(below both baseline captures). No transparency performance pass is claimed for HW.

## Phase 1: geometry-voxelizer removal (qualification in progress)

Removed `GeometryVoxelizer`, its raster voxelization and visualization shaders,
program registrations, `VoxelizerMode` and the config/setup/editor selectors.
Compute voxelization supplies both cache construction and voxel debug views.
Removed three obsolete mode-selection tests; retained the voxel coverage/reflectance
GPU tests in all four submission modes and the initial/cached voxel visualization
checks. The vertex-layout test still checks the G-buffer layout. The removal gate can
now also compare the previous removal capture via `--previous`, preserving the original
Phase 0 comparison and separate heatmaps for each reference.

The release build passes. Verification: 20 config, 21 runtime-launch, 786 RenderCore,
86 Vulkan RHI, 18 UI packet, 58 editor-model, 144 GI integration, 77 enabled editor
rendering, five UI rendering and five shipped-program tests pass, with no Vulkan
validation errors. The reflectance cases also pass with the corrected four submission
configurations. Python checks (35), formatting and the no-exceptions scan pass.
See `build/checks/hardening-geometry-tests`. All surviving SPIR-V modules are byte-identical
to the cone-removal archive; fourteen raster-only compiled/dependency files were removed.
Captures in `build/render-hardening-phase1-geometry` compare against both Phase 0 and
the preceding cone removal. All 41 HW sampled frames match the preceding build exactly.
Compute static differences are at most 1,700 pixels, 2/255 maximum, RMS 0.008285/255;
fast pan reaches RMS 0.009023/255 (two pixels over 4/255). Slow pan and dolly show the
same startup-sequence sensitivity seen in the probe removal, up to RMS 0.510487/255.
The strict identity result remains false; these timing-dependent Compute differences
are recorded as expected, with identical surviving shader modules and unchanged Compute
voxelization. The frozen Phase 0 comparison continues to include the previously documented
cone-removal lighting and field-publication differences.

## Phase 1: unified Compute resolution (prototype)

`pixelResolutionDivisor` now supports pure Compute as well as HW. Reference and ray-count
captures stay native; mixed providers remain native until their removal. Reduced Compute
traces both signals at the chosen resolution, without also enabling the half-diffuse
path. Reconstruction keeps the surface tests and supplies unsupported receivers from
the nearest filtered GI texel. It adds no output-resolution field trace. Phase 2 will
provide the history-fix signal and output stabilization. The old half-diffuse path remains
available pending qualification. `gi_noise_probe.py --compute-pixel-divisor=2` selects
the prototype while preserving the frozen native/half-diffuse default; the value is
retained in measurement identity.

The prototype passes 786 RenderCore, 86 Vulkan RHI, 18 UI packet, 144 GI integration
and two reduced-Compute editor scene-reload tests, with validation clean. Reconstruction
tests retain the HW unsupported-receiver behavior and check Compute's nearest-texel
fallback and untouched background in all four submission modes. There are 36 passing
Python checks. Logs: `build/checks/hardening-compute-divisor-tests`.

ZenApp now logs GI extent changes after cold field preparation. The profiler reads the
latest state and requires the requested active GI extent. Earlier Compute Nsight summaries
retained the first PBR frame's `0x0` GI extent; their actual native GI extent is confirmed
by the companion frame captures and RDG dispatches. Their historical summaries remain
unchanged. New reduced-Compute profiles must confirm `1920x1080` GI at `3840x2160` output.

Initial static runs in `build/render-hardening-phase1-compute-divisor2` have flicker
0.01052380 / 0.01052665 (Phase 0: about 0.08725) and reference RMS
3.38833508 / 3.38834115. RMS is better than the preceding native/half-diffuse captures
(cone removal 3.38898575; geometry removal 3.38898968), but still worse than the frozen
Phase 0 limit. The earlier expected forward-lighting and field-publication changes
already introduced this increase. The original RMS gate remains open; no full quality
pass or qualification to delete the half-diffuse path is inferred from the static result.
The two complete six-mode repeats now exist; `gates.json` records three failures.

First prototype repeat: slow-pan >4/255 shares at poses 8/32/64 are
0.096909 / 0.102250 / 0.107554%; fast pan 0.092942 / 0.077522 / 0.040835%;
dolly 0.089832 / 0.119768 / 0.127411%. The last dolly pose exceeds Phase 0.
Moving sparks total 2.25 pixels/frame, with one-pixel / two-to-three / larger clusters
1.33333 / 0.41667 / 0 per frame: the middle category exceeds Phase 0 despite the lower
total. The warm Nsight median is 33.45620 ms, `ForwardTranslucent` 1.089025 ms,
`HybridTrace` 17.43440 ms. Summed denoise/reconstruction pass medians are 8.390551 ms
(diagnostic sum, not whole-frame time), above the later 7 ms target. The active GI extent
is confirmed as 1920x1080. The second repeat is 33.4767 ms whole frame and reproduces
the same quality failures: final dolly 0.127773% and two-to-three-pixel clusters
0.41667/frame. The static RMS threshold also fails. This is a completed, failed gate;
the half-diffuse implementation has not been removed.

Current-build native controls in `build/render-hardening-phase1-compute-native-control`
isolate the reduced-resolution change from earlier removals. Two dolly runs finish at
0.120889 / 0.120177%; both moving-spark runs have 0.16667 two-to-three-pixel clusters
per frame (2.91667 total spark pixels/frame). Reduced resolution therefore regresses
these two measures against both the original and current native paths. Offline static
controls from the cone and geometry archives have flicker 0.08708531 / 0.08708573 and
RMS 3.38898575 / 3.38898968; the reduced path improves both current-build static metrics.
Control reports are also retained under `build/checks/hardening-compute-*-control.json`.

The planned output-resolution stabilization substep was brought forward as a
prerequisite for half-diffuse removal. `GIDenoiseStabilize` adds bilinear surface-tested
reprojection and a same-surface 3x3 neighbourhood clamp to reduced Compute GI. It keeps
sky, bounce and specular capture channels separate, preserves specular visibility alpha,
and gives output receivers up to eight frames of history. Opaque receiver guides own
that history; forward layers do not write it. Reference captures, native resolution and
the existing HW chain remain unchanged. This prototype is not a Phase 2 qualification
and does not relax any frozen gate. Allocation failure, resize, reset and declined-frame
handling follow the renderer's submitted-history ownership.

Stabilization verification passes 148 GI integration tests (including eight new shader
scenarios in each submission mode), 786 RenderCore tests, two Compute editor reload
tests and five native program-registration tests. Validation is clean. A further
RenderCore regression covers initial and steady declined recordings, resize and explicit
reset of output history. Formatting (39 changed files) and the exception scan pass.
Evidence: `build/checks/hardening-output-stabilize-tests` and
`build/checks/hardening-output-lifecycle.xml`.

The first stabilization repeat, `build/render-hardening-phase1-compute-stabilize`, clears
the two motion failures: dolly >4/255 shares are 0.075870 / 0.105481 / 0.115693%, and
moving spark clusters are 0.75 / 0.16667 / 0.08333 per frame (1.5 spark pixels/frame).
Slow pan is 0.077522 / 0.088735 / 0.094522%; fast pan is
0.075834 / 0.068408 / 0.032010%. Both static repeats have flicker about 0.0071701 and
RMS 3.38865749 / 3.38865341. The original RMS gate remains open; the current native
controls have higher RMS. These pan results do not meet the later 50% reduction at
every pose. The second repeat gives final dolly 0.115560%, moving sparks 0.75 pixels/frame
(0.75 / 0 / 0 clusters) and zero static sparks. Both six-mode repeats are complete.

The first warm stabilization trace is 37.21510 ms whole frame, with
`GIDenoiseStabilize` 3.07885 ms, `HybridTrace` 17.43325 ms and
`ForwardTranslucent` 1.08704 ms. This is below the native Compute frame baseline;
the added stabilization cost still leaves the old denoise chain above the later 7 ms
budget. Nsight reports 10,742 MiB committed for this scene and current chain; this is
not an 8 GB memory-pressure qualification. The executable, shader modules and capture
helpers are archived alongside the capture. The second trace is 35.98205 ms whole frame,
2.46573 ms stabilization and 1.08707 ms `ForwardTranslucent`. The stage-time variation is
retained rather than selecting the faster run.

`gates.json` fails only the original static RMS threshold. The resolution step is
accepted under Phase 1's documented expected-difference allowance: that RMS increase
already exists in the native controls after cone removal, while both reduced/stabilized
runs improve their RMS. `resolution-qualification.json` records this local decision and
retains `original_frozen_gates_passed: false`. It does not declare Phase 2 or the overall
plan qualified. The frozen RMS discrepancy remains open for later qualification.

Half-diffuse removal is now applied: `DiffuseResolution`,
the half trace, diffuse upsample and indirect fallback programs/shaders/resources are
gone. Ray-count diagnostics now allocate one tile range for the remaining trace instead
of an unused second range. Removed one obsolete setting test and the deleted-path parts
of the lifecycle test; retained the native and reduced-history lifecycle checks.
The measurement tool now defaults Compute to divisor 2; archived helpers preserve the
original native/half-diffuse preset. Removal captures will compare HW to the preceding
geometry removal and Compute to the qualified stabilization capture, alongside Phase 0.

The removal build passes, followed by 21 runtime-launch, 86 Vulkan RHI, 18 UI packet,
785 RenderCore, 148 GI integration, 77 enabled editor rendering and five native
program-registration tests, with no Vulkan validation errors. All 36 Python checks,
formatting (43 changed files) and the exception scan pass. Logs are in
`build/checks/hardening-half-removal-tests`. Ten obsolete compiled/dependency files were
removed after verifying their archived copies. All surviving normal-rendering SPIR-V
modules are byte-identical to the stabilization build; nine diagnostic count variants
changed with the smaller count-buffer layout (`build/checks/hardening-half-shader-diff.json`).
Removal captures are complete in `build/render-hardening-phase1-half`. HW is exact
against the preceding geometry removal in all 41 frames; the comparison to Phase 0
retains the same two fast-pan frames differing by one channel level (maximum RMS
0.012566). Compute's strict identity gate remains false: against the pre-removal
stabilized run, maximum RMS is 0.041046 static, 0.007599 slow pan, 0.007444 fast pan
and 0.277131 dolly. The two pan comparisons change at most two channel levels.
Static and dolly launches publish their first field on different frames than the
preceding run; normal-rendering shader modules remain identical. These differences
are retained as timing-dependent field/history evidence, not a byte-identity pass.

The previously noted throughput starvation persists for a later local field: cone,
unstabilized divisor-2 and stabilized profile logs all begin generation 5 (215,361
triangles, 925,107,920 scratch bytes) without publishing it during the recorded launch.
Generation 4 completing does not mean all local-field construction has settled.
`DistanceFieldScene::NextCycleCount` still reduces its triangle batch against total
frame interval, including unrelated rendering. Preserve this evidence for the progress
and memory hardening work; these measurements must not be described as fully built
field measurements.

## Phase 1: one tier instead of mixed providers

The six provider overrides are removed from settings, serialization, config loading
and UI. The global choice selects HW RT or Compute and preserves the independent
diffuse/specular/shadow enables and explicit shadow-map preference. Auto continues
to resolve the complete tier by capability. Removed five mixed HW/field trace
sources, four hardware radiance-injection sources and two hardware sky-cache sources,
their program registrations and 104 archived SPIR-V/dependency files. All 320
surviving modules match the preceding build byte for byte.

The editor now switches whole tiers in its integration test, retaining checks for
cold preparation, warm field reuse, scene replacement and shadow-map preflight.
Mixed-provider UI tests were replaced by one tier-switch preservation test and
updated drawing checks (four fewer UI tests overall). Reflection access checks now
cover the remaining Compute cache producer/consumer. Capture tools drop obsolete
provider arrays when deriving setups. The earlier cone removal had left stale cone
controls in the editor UI translation unit; those controls are removed here, and
the actual `zen_editor` executable is now included in the build checks. The format
helper now includes ZenApp and ZenEditor sources.

Release builds pass for ZenApp, zen_editor and the affected suites. Results:
21 runtime-launch, 86 Vulkan RHI/reflection, 14 UI packet/control, 785 RenderCore
(nine disabled), 148 GI integration, 77 enabled editor rendering and five shader
registration tests; no validation errors. A pre-existing timing assertion compared
2.3 against a summed 2.3000000000000003; its assertion now allows four floating-point
rounding units, and RenderCore passes. All 36 Python checks, formatting of 49 changed
files and the 553-file exception scan pass. Logs: `build/checks/hardening-tier-*`.
Capture comparisons are complete in `build/render-hardening-phase1-tiers`, against
both Phase 0 and the half-diffuse removal. All 41 HW frames match the preceding build;
the Phase 0 comparison retains the same two one-level fast-pan differences. Compute
strict identity remains false: maximum RMS versus the preceding build is 0.041038
static, 0.007652 slow pan, 0.010105 fast pan and 0.277135 dolly. Static and dolly field
publication moves from 40 to 42 submitted graphs; pan publication stays at 42. The
slow-pan difference is at most one channel level; fast pan changes at most 1,542 pixels
and eight channel levels. Shader identity and the recorded publication timing support
the existing field/history timing explanation; these are not reported as strict passes.

## Phase 1: remaining trace experiments

The final removal group deletes adaptive sampling, the delayed `previousFrameRadiance`
option, legacy sky tracing, buffered environment-guide programs and arbitrary trace-module
overrides. The qualified no-environment 8Ã—8 path is retained as explicit `NoGuide` programs;
normal learned-guide and exact capture specializations remain. The pooled guide buffer,
prepare/finish passes and experimental combined/ablated branches are gone. CMake no longer
generates `.noguide` modules from experimental parents. The experimental module builder was
removed, and candidate/reference helpers now use the supplied ZenApp build.

Fixed sampling uses the selected 1/2/4 budget. The removed adaptive budget history is no
longer allocated or bound. Delayed radiance no longer owns an alternate mip chain or a
third cache snapshot. The old denoiser still needs a base-level previous radiance/cache
snapshot for its cache-delta correction; that snapshot and its submission lifecycle remain
until the new chain replaces the correction. The existing 18-frame history regression was
retargeted to that contract, retaining decline, failure, resize, idle, lighting and reload
coverage. One adaptive-budget unit test and three dead cone-visibility-bound tests were
removed; the occupancy visibility calibration keeps its local two-vector layout. The DDA
visibility helpers used by radiance injection remain.

Empty cache scenes now clear sky irradiance instead of tracing legacy geometry. The
invalidation test includes transfer nodes so it continues to observe that update. Production
field sky tracing remains unchanged. Sparse reference capture is a built-in diagnostic:
`referenceTileStride=8` requires a nonzero `referenceSamples`; 1 is the dense default.
The fixed 8Ã—8 pixel tiles reproduce the archived 4K mask exactly (130,560 pixels), independent
of trace workgroup size. The first-frame 64-sample dense/sparse comparison passes the existing
precision bound: RMS 0.018703/255, maximum 2/255, 90 changed selected pixels. This is a precision
bound pass, not byte identity. Setup, actual diagnostic state, executable and shader identity
are retained in `build/render-hardening-phase1-final-removal`.

Release builds pass for ZenApp, zen_editor and affected tests. RenderCore has 782 tests (nine
disabled), Vulkan RHI/reflection 86, runtime launch 21, UI 14, GI integration 148, editor 77
enabled and registered-program integration 10. All pass; GPU validation logs contain no
VUID, synchronization-hazard or binding errors. All 36 Python checks pass. Logs are under
`build/checks/hardening-final-removal-*`. Formatting passes for 55 changed files and the
553-file exception scan passes. The profiling helper's obsolete module options were removed
after capture completion; the archived helper bundle retains the version used during capture.

The HW/Compute comparisons are complete. All 41 HW frames match the preceding tier-removal
build byte-for-byte. Against Phase 0, the same two fast-pan frames differ by at most 1/255
(maximum RMS 0.012566). Compute strict identity remains false. Versus the preceding build,
maximum RMS is 0.041045 static, 0.007849 slow pan, 0.008417 fast pan and 0.008146 dolly;
maximum channel differences are 9, 1, 8 and 1 respectively. The static field publication
moves from 42 to 40 submitted graphs; dolly remains at 42. The small motion differences
affect at most 1,649 pixels. This group changes trace control flow and cache snapshot code,
so shader binary identity is not claimed. The recorded differences are consistent with the
existing field/history timing sensitivity and floating-point variation, but are not strict
identity passes. The original Compute RMS and HW frame-time gates remain open as documented
above. Forty-two retired SPIR-V/dependency files were archived with hashes before removal.

## Phase 2 prototype: implemented, not qualified

The opt-in `relaxDenoiser` / `voxel_gi_relax_denoiser` path records preblur (including
three lower mip levels), temporal slow/fast histories, history repair, three spatial
iterations, reconstruction when reduced, and output stabilization. It records eight
passes at reduced resolution and seven at native resolution. It does not record
`HybridReconstructFallback`. Sky and bounce remain separate output channels for lighting
controls and capture, with common diffuse geometry weights and combined diffuse variance.
The old denoiser remains the default; reference captures retain their FP32 estimator.

The prototype keeps independent diffuse, reflection and environment-visibility ages,
rejects bilinear support below 0.01, and excludes missing observations from the mip chain.
Fast histories clamp slow histories without motion-dependent age caps. FP32 length
storage retains the full configured history limit (up to 4,096 frames at divisor four).
The current repair uses sixteen spaced mip samples for histories under eight frames,
with coordinates clamped to the mip bounds. This extends the plan's initial
four-frame repair window based on the measured correlated motion noise; the quality
and timing acceptance thresholds still apply.
Reflection toggles reset prototype history; its trace skips the old cache-delta evaluation.
The old cache snapshots still exist while the old denoiser remains available.

Validation-enabled ZenApp smokes pass for HW and Compute in all four submission modes
at 1280Ã—720. GPU regressions cover missing samples, mip repair across non-aligned extents,
surface isolation, independent visibility, small bilinear overlaps, motion retention,
history lengths and repaired reconstruction. The full GI suite passes 156 tests;
RenderCore passes 782 total (nine disabled), including prototype resize, declined-frame
and output-history checks. Runtime launch passes 21. Logs and identities are under
`build/checks/denoiser-prototype-*`. These establish correctness checks, not quality gates.

The first 4K HW run in `build/render-hardening-phase2-prototype` **fails qualification**:

| Metric | Prototype run 0 | Frozen Phase 0 |
|---|---:|---:|
| Static flicker (mean standard deviation, /255) | 0.087690 | 0.029419 |
| Static reference RMS (/255) | 0.684960 | 0.556593 |
| Slow-pan error, poses 8 / 32 / 64 (%) | 0.462167 / 0.514070 / 0.502170 | 0.235050 / 0.259983 / 0.199894 |
| Fast-pan error, poses 8 / 32 / 64 (%) | 0.549154 / 0.752496 / 0.633644 | 0.374711 / 0.398847 / 0.408034 |
| Moving spark pixels/frame | 78.5833 | 13.75 |
| Whole-frame median (ms) | 34.08145 | 31.7262 |
| Denoise + reconstruction sum of pass medians (ms) | 8.84069 | target â‰¤ 7 |
| `ForwardTranslucent` median (ms) | 5.22451 | 3.32696 |

The prototype timing breakdown is preblur 0.71456, temporal 0.88323, repair 0.21067,
spatial iterations 0.85363 / 0.84990 / 0.82459, reconstruction 1.40782 and output
stabilization 3.09627 ms. The transparent/complex-forward regression is unresolved and
must be measured against the immediately preceding removal build to attribute it.
The complete two-run, three-preset matrix finished with its final executable, shader,
setup and input hash checks passing. The measured runtime is retained under
`build/render-hardening-phase2-prototype/runtime`, with 694 files and a hash manifest.
The quality results are deterministic for HW and editor; Compute retains its existing
field-publication variation. Both HW runs have the quality values above.

| Metric | Compute run 0 / 1 | Editor run 0 / 1 |
|---|---:|---:|
| Static flicker (/255) | 0.021549 / 0.021551 | 0.289597 / 0.289597 |
| Static reference RMS (/255) | 3.419984 / 3.419970 | 1.541668 / 1.541668 |
| Moving spark pixels/frame | 6.0833 / 6.5 | 149.5 / 149.5 |
| Static spark pixels/frame | 0 / 0 | 2.75 / 2.75 |
| Whole-frame median (ms) | 33.7249 / 33.6889 | 58.9599 / 57.96135 |
| `ForwardTranslucent` median (ms) | 1.08888 / 1.08815 | 3.38082 / 3.38160 |

Compute slow-pan errors for poses 8 / 32 / 64 are 0.101767 / 0.112956 / 0.116983%
in run 0 and 0.099272 / 0.116151 / 0.118043% in run 1. Fast-pan errors are
0.101924 / 0.084961 / 0.061632% and 0.101972 / 0.085009 / 0.061632%.
These improvements do not reach the required 50%. Editor slow-pan errors are
2.970932 / 2.844160 / 2.947724%; fast-pan errors are 2.812343 / 2.670211 / 3.369382%.
Both editor runs match, and regress against the baseline. All three presets fail the
reference-RMS gate; HW and Compute also fail the moving-sparks gate.

HW run 1 measures 31.102 ms whole-frame, 3.38546 ms forward and 8.14614 ms for the
denoise/reconstruction chain. The chain still exceeds 7 ms. Its stabilization pass
is 2.48071 ms versus 3.09627 ms in run 0. The large timing spread requires controlled
warm-launch comparisons against the preceding build; run 1 does not erase run 0.
The Compute time reduction also includes the Phase 1 resolution change, so it is
not attributed solely to denoising. No defaults have switched and no baseline gates
have been relaxed. Fast-history clamping remains a candidate cause of the static
regression; an isolated comparison is required before attributing it.

## Phase 2 clamp investigation

An isolated shader ablation removes only the three RGB fast-history clamps, using the
archived prototype ZenApp and otherwise identical shader modules. All installed SPIR-V
was restored after capture. `build/checks/denoiser-clamp-ablation` retains the compiler
command, original/candidate shaders and hashes, setup, capture arguments and results.

| Metric | Prototype | RGB clamp disabled |
|---|---:|---:|
| HW static flicker (/255) | 0.087690 | 0.033899 |
| HW static reference RMS (/255) | 0.684960 | 0.550600 |
| Editor static flicker (/255) | 0.289597 | 0.279873 |
| Editor static reference RMS (/255) | 1.541668 | 1.684344 |

This establishes the per-pixel clamp's contribution to HW instability; it does not
qualify removing clamping. HW flicker still exceeds the original baseline, and editor
reference accuracy gets worse. The next revision computes surface/lobe-aware 5Ã—5 fast
history statistics in YCoCg, expanding the box with the center fast estimate. It runs
inside history fix, using shared tile loads. Temporal signals are transient until this
pass writes the persistent slow history; spatial-filter outputs remain outside the
feedback loop. The pass count stays unchanged. New native checks cover chromatic noise,
uniform lighting changes, foreign surfaces and partial workgroups. Measurement of this
revision is under `build/render-hardening-phase2-spatial-clamp`. All 160 GI integration
checks pass with clean validation, including the new spatial-clamp cases. The one-run
4K static measurements improve over the first prototype, but still fail qualification:

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.038725 | 0.023513 | 0.274718 |
| Static reference RMS (/255) | 0.571230 | 3.395692 | 1.277106 |

The executable/shader/setup/input hashes remained unchanged through the captures.
The runtime and its manifest are retained in the same directory. HW flicker and all
three reference RMS values still exceed the frozen baseline. Motion and timing were
not rerun for this revision; these static failures are sufficient to keep it opt-in.

## Phase 2 spatial support and bandwidth

The stride-only ablation in `build/checks/denoiser-stride-ablation` keeps the three
Ã -trous lattices at 1/2/4 instead of rounding mature histories down to 1/1/2.
Everything else matches the spatial-clamp revision. HW flicker/RMS becomes
0.036605 / 0.573260, Compute 0.022402 / 3.394119, and editor 0.250043 / 1.216713.
The experiment restores the original installed module after verifying capture hashes.

The following revision keeps those lattices and adapts each signal's kernel width and
blend strength with history length. Shared tiles reduce guide/variance reads in the
first two iterations, including signal reads in the first. History fix decodes guides
once per tile. Temporal combines surface-projected reflection and diffuse gathers;
glossy virtual-motion reprojection remains separate. Temporal and output stabilization
skip zero-weight bilinear taps. Output stabilization shares its current 3Ã—3 neighborhood.
No passes were added. All 164 GI integration tests pass with clean validation, including
a new long-history coarse-support and foreign-surface regression in all four modes.

`build/render-hardening-phase2-kernel-cache` retains the measured runtime, manifest and
one-run static captures. Their final executable, shader, setup and input hashes pass.

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.036605 | 0.022451 | 0.235206 |
| Static reference RMS (/255) | 0.573260 | 3.396469 | 1.176160 |

The reference-RMS gates and HW flicker gate remain open; no defaults have switched.
Motion for this combined revision is pending. Its first HW timing run measures
32.71615 ms whole-frame and 3.38744 ms forward. The denoise/reconstruction chain
totals 10.25998 ms: preblur 0.72616, temporal 0.80341, history fix 1.08239,
spatial iterations 1.22994 / 1.67564 / 0.94382, reconstruction 1.37991, and output
stabilization 2.41872. Run 1 measures 32.99415 ms whole-frame, 3.43714 ms forward,
and 10.41421 ms for the chain. Both fail the 7 ms budget.

An isolated output-tile comparison, before the other bandwidth changes, is retained
in `build/checks/denoiser-stabilize-tile`. Both settled HW and editor 4K images match
the spatial-clamp revision byte-for-byte. Four warm Nsight launches use A/B/B/A order:

| Launch | Stabilization (ms) | Whole frame (ms) | ForwardTranslucent (ms) |
|---|---:|---:|---:|
| Original 0 | 2.77621 | 35.10830 | 3.44858 |
| Tile 1 | 2.37632 | 34.68450 | 3.40802 |
| Tile 2 | 2.72557 | 33.12920 | 3.37751 |
| Original 3 | 3.11811 | 33.40675 | 3.38280 |

The adjacent comparisons reduce this pass by 0.39989 and 0.39254 ms. Whole-frame
and individual-pass spread remains substantial, so these establish the local reduction,
not a pass of the total 7 ms budget or final transparency performance gate.

## Phase 2 separable kernels and clamp range

The separable-kernel revision evaluates the two non-unit B-spline weights once per
receiver instead of once per tap. Temporal and stabilization use an exact same-pixel
coordinate when the CPU confirms unchanged camera, geometry and surface state.
The existing history-publication/reset test now also checks that uniform flag and
passes (`build/checks/denoiser-wide-clamp-candidate/core.*`). All 48 focused denoiser
GPU checks pass in all four submission modes with clean validation. The measured
runtime is archived in `build/render-hardening-phase2-separable`.

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.036610 | 0.022405 | 0.235329 |
| Static reference RMS (/255) | 0.573157 | 3.394162 | 1.178090 |

Its one-run HW profile measures 32.23730 ms whole-frame and 3.375 ms forward;
the eight denoise/reconstruction passes total approximately 9.72 ms, still above
the 7 ms limit. These results do not qualify the revision.

`build/checks/denoiser-wide-clamp-candidate` tests a 3-sigma fast-history RGB box
and spatial strength `clamp(8 / sqrt(age), .25, 1)` instead of 2 sigma and 4.
The fixed 1/2/4 lattices and adaptive radius are unchanged. All 48 focused native
checks pass with clean validation. Executable/shader/input hashes are verified;
the original installed modules are restored after the one-run static captures.

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.030181 | 0.014180 | 0.215064 |
| Static reference RMS (/255) | 0.556736 | 3.388654 | 1.105270 |

The editor static measurements improve beyond baseline. HW flicker and HW/Compute
reference RMS still miss the frozen gates. This is an experiment, not a default
change or full qualification; motion, sparks and cost are not measured for it.

The next experiment, `build/checks/denoiser-wide4-candidate`, uses 4 sigma and
spatial strength `clamp(10 / sqrt(age), .25, 1)`. Its 48 focused GPU checks pass
with clean validation, including matching unchanged-receiver output histories.
All three static results beat the frozen flicker/RMS limits in this single run:

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.028506 | 0.011653 | 0.216029 |
| Static reference RMS (/255) | 0.553942 | 3.386702 | 1.090287 |

This is not full qualification: repeated captures, motion, sparks and timing remain.
Original installed modules were restored and verified after the captures.

The access-only experiment in `build/checks/denoiser-access-candidate` replaces
the first two spatial caches with direct reads and caches the initial upsample
window. Its 48 native checks pass, and settled HW/editor images are byte-identical
to the separable revision. It is rejected on cost: whole-frame time is 34.20410 ms,
forward 3.39996 ms, upsample 2.68131 ms, and spatial passes 1.57973 / 1.03320 /
0.94570 ms. The upsample and first spatial pass are slower despite fewer explicit
loads. Installed modules were restored; this experiment is not production code.

The 13-tap circular spatial experiment in
`build/checks/denoiser-sparse-stable-candidate` also reduces fast-history statistics
to 3Ã—3, retains only the first spatial cache, and skips redundant previous output
guide loads when receivers are unchanged. Its 48 focused GPU checks pass. It costs
7.686 ms for the eight-pass chain (30.15060 ms whole-frame, 3.396 ms forward), but
fails quality: HW flicker/RMS 0.033299 / 0.576807, Compute 0.018647 / 3.399724,
editor 0.240805 / 1.203217. It remains an experiment. Original modules were restored
and final capture hashes verified.

The preceding attempt in `build/checks/denoiser-sparse-candidate` failed four native
bright-uniform clamp cases before capture: cancellation in `E[xÂ²] - E[x]Â²` allowed
99.875 instead of 100. Centering the samples on the center fast color before
accumulating their first and second moments fixes this without relaxing the test.
The centered calculation is retained in production with the wider 5Ã—5 statistics.

## Phase 2 preblur history age

Preblur previously read the previous age at the current pixel without reprojection
or receiver rejection. A newly visible surface could therefore inherit a mature
radius from a different surface. It now reads age at the reprojected coordinate,
using the same identity/plane/normal/roughness rejection helper as temporal, and
uses the reflection history's own age for specular blur. The production prototype
also retains the 4-sigma box and spatial strength 10 from the successful one-run
static experiment; it keeps the 25-tap spatial filters and 5Ã—5 fast statistics.

All 52 focused denoiser checks pass in all four submission modes with clean
validation (`build/checks/denoiser-preblur-age`), including seven preblur scenarios:
mature history, identity rejection, reprojected young history, no history, plane
rejection, normal rejection and an independently young reflection. The existing
history-publication CPU test and four hardware transparency cases also pass, with
no reported memory leaks. The runtime and captures are retained in
`build/render-hardening-phase2-preblur-age`. One-run static flicker/RMS is
HW 0.028507 / 0.553934, Compute 0.011653 / 3.386698, editor 0.216029 / 1.090255.
All six static metrics beat baseline. HW slow-pan errors at poses 8/32/64 are
0.197555 / 0.245829 / 0.246793%, and fast-pan errors are
0.284602 / 0.499253 / 0.310221%. These still fail the 50% reduction gate.

## Phase 2 output clamp and disabled sky work

The output clamp now uses the current 3Ã—3 color neighborhood while retaining strict
identity, plane, normal and lobe checks on reprojected history samples. This keeps
a thin receiver's own valid history from being clamped to one current observation;
neighboring receivers supply bounds, never history samples. The shared tile stores
signals, roughness and validity; center guides are decoded once. CPU-confirmed
unchanged receivers also skip redundant previous-guide loads.

`build/render-hardening-phase2-output-bounds` retains the runtime and verified
one-run captures. All 168 GI integration tests, 785 RenderCore tests (nine disabled)
and four hardware transparency cases pass, with clean validation and no reported
leaks (`build/checks/denoiser-output-bounds`).

| Metric | HW | Compute | Editor |
|---|---:|---:|---:|
| Static flicker (/255) | 0.027930 | 0.010524 | 0.212590 |
| Static reference RMS (/255) | 0.552368 | 3.385779 | 1.087126 |

HW slow-pan errors are 0.176264 / 0.227057 / 0.231867%; the motion gate still
fails. HW whole-frame time is 31.46510 ms, forward 3.37416 ms, and the eight-pass
chain 8.44745 ms. Output stabilization is 0.972 ms, compared with 2.191 ms in the
separable revision. This combined revision still exceeds the 7 ms budget.

The next revision uses one-texel slow/fast sky histories and intermediate sky
signals when sky lighting is disabled, with bounded reads and writes. Preblur
keeps its per-receiver observation flags. Only the first spatial pass retains a
shared cache; the wider passes use direct reads, following the earlier timing
comparison. All 168 GI, 785 RenderCore (nine disabled) and four transparency cases
pass again (`build/checks/denoiser-disabled-sky`). The CPU history test covers
sky lighting enable/disable, the GI environment setting and a declined recording,
and verifies actual bound image extents. The retained one-run captures in
`build/render-hardening-phase2-disabled-sky` preserve the static scores:
HW flicker/RMS 0.027930 / 0.552368, Compute 0.010525 / 3.385792, and editor
0.212590 / 1.087126. Reported committed VRAM falls from 9,841 to 9,636 MiB;
this still does not qualify the 8 GB budget. Whole-frame time is 33.22945 ms,
forward 3.37920 ms, and the denoise/reconstruction chain 9.72228 ms. In particular,
the widest spatial pass rises from 0.933 to 2.211 ms. This single run establishes
a regression requiring investigation, not its cause; other passes also vary.

The following revision selects sky-enabled/disabled pipeline specialization
constants for temporal, history fix, spatial filtering and reconstruction. The
disabled specialization removes sky loads and shared storage while retaining
preblur's per-receiver observation flags. All 168 GI, 785 RenderCore (nine disabled)
and four hardware transparency checks pass with clean validation
(`build/checks/denoiser-sky-specialization-2`). The two editor debug regressions also
pass with reduced-resolution prototype GI and explicitly enabled environment light.
Debug signals now expose sky, reflection and bounce independently, and sample history
with its own extent when the signal is at output resolution.

`build/render-hardening-phase2-sky-specialization` retains the matching runtime and
one-run static/timing measurements. HW and editor static scores equal the prior
revision; Compute flicker/RMS is 0.010554 / 3.386099, still within the baseline gate.
The eight-pass chain totals 8.26020 ms, whole frame 31.59020 ms, and forward
3.38023 ms. The widest spatial pass returns to 0.93349 ms; history fix is 0.87090 ms
and reconstruction 1.18383 ms. The middle spatial pass remains expensive at
1.63965 ms. These are single-run measurements; the 7 ms gate still fails.

All 32 HW and all 32 editor static images also match the disabled-sky revision
byte-for-byte after decoding. A slow-pan endpoint ablation retains exact setups,
arguments, hashes and pixels under `build/checks/denoiser-pan-components`: full
lighting has 0.231867% error, diffuse plus direct 0.062982%, specular plus direct
0.130739%, and direct-only is byte-identical to its settled pose. These display-space
threshold scores are not additive. The first folder named `reflection-only` had
`specularOcclusion=false`, which also disables reflected radiance in the existing
renderer; the corrected reflection measurement is named `specular-enabled`.

## Phase 2 camera rotation and reflection history

The prototype treated any screen motion as glossy reflection parallax. For a
perspective rotation around a fixed eye, surface and virtual-image coordinates
project identically; requiring a fresh hit distance unnecessarily rejected history
on misses or noisy hit-distance changes. Temporal now uses surface reprojection in
that case, retaining its normal, plane, identity, roughness and view tests. Translation
and orthographic view-direction changes retain virtual-motion reprojection. Geometry
changes already invalidate history before dispatch.

The regression failed on the previous shader in all four submission modes: a valid
65-frame history reset to one frame on a pure-rotation miss. It passes after the fix,
while a translated-eye miss still rejects reflection history. All 52 focused GI
checks, the history-publication CPU check and four transparency checks pass with clean
validation (`build/checks/denoiser-camera-parallax`). The one-run HW motion captures
under `build/render-hardening-phase2-camera-parallax` measure slow-pan errors of
0.175890 / 0.226997 / 0.231807%, and fast-pan errors of
0.261839 / 0.484713 / 0.296706%. The targeted fix does not materially change these
scene-wide scores and does not pass the 50% gate; rough reflections remain a larger
source of error in this scene.

An arithmetic-only spatial-filter experiment combines exponential edge stops before
evaluating the final three signal weights, retaining all 25 taps and existing guide
tests. `build/checks/denoiser-exponent-candidate` retains the source, compiler command,
original/candidate SPIR-V hashes, 52 passing native checks and standalone captures.
All production modules were restored and verified afterwards. One-run flicker/RMS is
HW 0.027930 / 0.552368, Compute 0.010554 / 3.386109, editor 0.212590 / 1.087127.
The three spatial medians are 0.92778 / 0.91816 / 0.83645 ms; the complete chain
is 7.67453 ms, whole frame 30.61575 ms and forward 3.38861 ms. There is one frame-time
outlier at 37.9257 ms. The candidate preserves the static gates but remains above
7 ms. It is now adopted into the prototype source, with shared surface/lobe term
helpers rather than duplicated rejection logic. The pre-blur and history-fix
passes retain their original arithmetic; only the spatial passes combine the
exponents. All 52 focused denoiser GPU checks pass after factoring
(`build/checks/rt-reuse-and-cache-2`). Standalone qualification is pending.

## Phase 3 foundation

The foundation now includes device-reported RT limits, ordered SPIR-V modules (including
multiple miss shaders), shader groups, a pipeline-owned aligned SBT, cached RT pipelines,
direct/indirect commands and RDG RT passes with reflected stages and async eligibility.
Shared dispatch plumbing retains the existing compute pipeline behavior. Optional EXT/NV
invocation reordering is selected from enabled feature/property support, with plain RT
modules retained. Calibration any-hit shaders share the ray-query alpha/material helpers.

Release builds pass. All 95 RHI/reflection tests pass, including SBT layout, launch limits
and all RT shader stages. A missing RT creation forwarding overload in the RHI thread
executor was fixed. RenderCore passes 785 total tests (nine disabled), and the Vulkan
capability suite passes 21, including forced EXT/NV feature, extension, hint and
indirect-tracing masks (`build/checks/rt-capability-optional`). The full ray-query
integration executable passes with 124 total tests, twelve disabled and four
fixture-dependent lantern experiments skipped.
Its eight RT calibration cases pass across all four submission modes with validation
enabled: plain/EXT reordered pipelines, direct/indirect launches, alpha changes, motion,
deformation, removal and four-state opacity micromaps agree with ray-query results.
There are no VUID or synchronization hazards and no reported memory leaks in these runs.
NV shader variants compile, but this device selected EXT for the native run. Logs are
under `build/checks/rt-foundation-verified`; the RHI/reflection log is in
`build/checks/rt-foundation-tests`. Production GI still uses the old HW query path.

## Phase 4 forward-material decision

On 2026-11-13 the user approved PBR/environment fallback for forward GI that cannot
reuse a screen-space receiver. Existing forward diffuse/reflection ray queries will be
removed; forward shadow ray queries remain the documented exception. Matching forward
receivers continue to reuse reconstructed GI. This permitted lighting difference must be
measured and documented separately from the opaque RT-pipeline equivalence gates.

The forward fallback is now implemented in source while production opaque tracing
still uses ray queries. Unsupported forward layers retain material environment
lighting; supported receiver/decal reuse, per-layer shadow rays, transmission,
refraction and attenuation remain. The forward passes no longer bind environment
sampling distributions or select GI-only ray-query shader variants when shadow maps
are used. Those three unused shader entry points and their registrations are removed.
Release verification passes 168 GI tests, 785 core tests (nine disabled), and all four
transparency submission modes with clean validation and no reported leaks
(`build/checks/forward-gi-fallback`). The transparency regression now also checks
screen refraction, absorption, thickness and CPU/GPU transmission-image equality.

The first standalone ZenApp run is retained in
`build/render-hardening-phase4-forward-fallback` with the executable, shader modules,
setup, hashes and capture provenance. Static flicker/RMS are HW
0.0242904842 / 0.5515046304, Compute 0.0105542626 / 3.3860959018, and editor
0.2094574814 / 1.0869110919. All six static metrics pass the frozen limits once.
The permitted forward-lighting change improves HW flicker and RMS relative to the
pre-fallback prototype; it is not an opaque RT-pipeline equivalence result.

At 3840Ã—2160 fullscreen, 1920Ã—1080 GI, HW backend, threaded RHI and async compute
available, the first warm Nsight run measures whole-frame median 28.47055 ms,
`ForwardTranslucent` 1.542765 ms and `SceneLighting` 4.649665 ms. The preceding
sky-specialization capture measured 31.5902 / 3.380225 / 5.16797 ms respectively.
The eight denoise/upsample passes total 7.746227 ms, still above 7 ms. Requested
VRAM is 9612 MiB, still above the 8 GB qualification budget. Repeated timing,
motion and sparks gates remain open.

The user also requires correct transparent-object rendering without a performance
regression. Preserve blend ordering, transmission/refraction and per-layer shadows;
transparent layers must not replace opaque GI receiver guides or inherit another
surface's visibility. Verify overlapping layers, opaque occlusion and alpha-tested
geometry, retaining the existing transparency regressions. Record `ForwardTranslucent`
and related forward-pass timings separately alongside whole-frame and GI budgets.

Current `ForwardTranslucent` medians (the pass also includes applicable complex forward
materials): HW 3.32696 / 3.32688 ms; Compute 1.10750 / 1.30931 ms; editor
3.33923 / 3.32749 ms. These use the same Nsight captures as the whole-frame measurements.

The shared blend fixture now supports an opaque receiver behind four interleaved
transparent triangles. The new hardware-GI regression passes all four submission modes
with validation enabled (`build/checks/denoiser-separable/RayQueryIntegrationTest.*`).
It confirms CPU/GPU draw-order pixel equality, a lit front layer over a shadowed opaque
receiver, restored opaque lighting when the layers are hidden, and unchanged opaque
positions/normals/depth while transparent layers move or are hidden. The checks use
both lighting and hybrid captures with the prototype denoiser enabled. All twenty
existing blend ordering, material-UV, opaque-core and draw-cache regressions also pass
after extracting the shared fixture (`build/checks/transparent-hardware-gi-3`). These
establish the pre-port correctness checks; final forward timing and transmission
coverage after the approved fallback change remain required.

## Phase 4 receiver-shadow pipeline prototype

The optional `rayTracingPipeline` setup setting (config
`voxel_gi_ray_tracing_pipeline`) is being wired into the production HW renderer.
It remains false by default. Its first port is an output-resolution
`ReceiverVisibility` raygen before deferred lighting. GI trace and ReSTIR are
still the query implementations at this stage. The new raygen uses the shared
scene-bound clipping, surface offsets, finite-light endpoints and alpha test;
terminate-on-first-hit rays use an any-hit group and a miss shader, with no
closest-hit execution. Invocation reordering follows the reported capability.
The diagnostic variant records deferred shadow-ray totals.

Deferred lighting reads the visibility image without issuing fragment queries.
The image retains exact/visible masks for all 32 scene lights and a separate
camera-light value, avoiding a shift by 32 or aliasing the first scene light.
The initial RGBA32UI layout uses 16 bytes per output pixel. Forward layers read
the existing two masks under the same receiver-matching rules and continue
their own queries otherwise; they never write the opaque receiver guides.
Release native checks pass 128 ray-suite tests (twelve disabled,
four fixture-dependent lantern experiments skipped), 785 core tests (nine
disabled) and 52 focused denoiser tests with clean validation and no reported
leaks (`build/checks/rt-receiver-visibility` and
`build/checks/rt-receiver-visibility-full`). The expanded transparency regression
compares query and pipeline images byte-for-byte in all four submission modes,
including transmission, layer motion/hiding, 32 scene lights and a separately
colored camera light.

The first standalone shadow-pipeline capture is retained in
`build/render-hardening-phase4-receiver-visibility`. HW and editor flicker/RMS
match the forward-fallback run exactly; Compute gives 0.0105256828 / 3.3857679924.
This version bypassed the candidate cache and used EXT reordering. It fails the
timing gate: receiver visibility 15.44485 ms, deferred lighting 1.028225 ms,
whole frame 40.41945 ms and forward 1.652195 ms. Its images are correct, but it
is not a qualified replacement.

An isolated module substitution with the same archived executable, setup and
all other SPIR-V modules measures plain tracing at 11.76955 ms for visibility,
whole frame 37.43395 ms and forward 1.54899 ms
(`build/checks/receiver-visibility-plain`). The script records both module
hashes and restores/verifies the original in `finally`. Receiver visibility
now selects plain tracing: these rays terminate at a hit without closest-hit
shading. Reordering remains available for other RT programs.

The current source restores the candidate-cache test before each receiver ray
and ports `LightOccluders` itself to a raygen/closest-hit/miss/any-hit pipeline.
It continues to cache actual opaque triangles and tests each receiver's exact
finite segment, without approximating visibility. The cache's continued use
is supported by the uncached timing regression; its new pipeline and the
cached receiver pass still need native and standalone verification.

The ReSTIR temporal and spatial visibility ports are implemented. They share the same
reservoir math and use the pipeline's shadow miss/any-hit groups. The two
spatial checkerboard launches remain separate: the second consumes the first's
updated reservoirs. Merging them in place would introduce read/write races;
using separate input/output reservoirs would add 398 MB at native 4K and change
the estimator's update order. No merge is justified before those costs and
quality differences are qualified.

The combined cache/ReSTIR source passes the full 128-test ray executable
(twelve disabled, four skipped), 785 core tests (nine disabled), and all 52
focused denoiser checks with clean validation and no reported leaks
(`build/checks/rt-reuse-and-cache-2`). The original new parity assertion compared
every reservoir byte and exposed a one-ULP specular MIS weight difference between
the compute and ray-generation shader stages. The regression now requires exact
position, packed normal/count and packed radiance, with floating-point equality
for the weight and an XML property recording the largest observed ULP distance
(one in each mode). Existing analytic energy, rejection and occlusion checks
still pass. Cache visibility results match the query builder byte-for-byte
through light and geometry movement, and transparency images and opaque guides
match through the pipeline switch.

The first combined capture, including the factored spatial exponential weights,
is `build/render-hardening-phase4-reuse-cache` (758 archived runtime files).
Static flicker/RMS are HW 0.0242601836 / 0.5514139220, Compute
0.0105244475 / 3.3857929660, and editor 0.2094573960 / 1.0869122751.
All six static limits pass once. Warm HW timings are whole frame 33.02065 ms,
forward 1.65144 ms, primary GI trace 6.081985 ms, receiver shadows 5.209215 ms,
deferred lighting 1.02861 ms and occluder-cache build 0.04232 ms. Restoring the
cache materially reduces receiver-shadow cost, but the replacement remains
slower than the query composition it replaces. Reordered ReSTIR temporal/spatial
passes take 0.683792 / 4.18202 / 4.23117 ms, also a regression. The denoiser's
eight passes total 7.16794 ms. Neither the RT timing gate nor the 7 ms denoiser
gate is qualified.

The isolated plain-ReSTIR comparison (`build/checks/restir-visibility-plain`)
measures temporal/spatial passes at 0.696208 / 6.08154 / 6.12454 ms, whole frame
36.85945 ms and forward 1.65085 ms. It uses the same archived executable, setup
and other modules; the script records substitutions and restores/verifies both
original modules. Reordering remains enabled for ReSTIR because plain tracing
increases its measured cost.

The first primary-GI pipeline covers the no-environment trace, including
diffuse-hit reflection reuse. It evaluates hit materials in closest-hit and
uses a separate miss index and payload for nested shadow rays. The extended
hit-lighting regression compares query and pipeline results for six light,
occluder, environment and material-update scenarios in every submission mode.
The complete 128-test ray suite, 785 core tests and 52 denoiser checks pass with
clean validation (`build/checks/rt-primary-noguide`).

`build/render-hardening-phase4-primary-noguide` retains 782 runtime files.
Its static flicker/RMS are HW 0.0242753082 / 0.5515036359, Compute
0.0105241633 / 3.3857748293, editor 0.2094564204 / 1.0869070378: all six limits
pass once. Warm HW primary trace is 10.7703 ms, receiver visibility 5.206145 ms,
ReSTIR temporal/spatial 0.546816 / 4.189185 / 4.238335 ms, forward 1.65088 ms,
and whole frame 37.6586 ms. The primary trace is substantially slower than the
6.081985 ms query trace in the preceding capture, so this is not qualified.
The next source revision restores compact hit-stream access in closest-hit,
groups ray launch pixels into 8x8 screen tiles, and implements the environment
guide's prepare/finish passes. These changes still require verification.

The 65x65 production-path regression exposed a constant-alpha cutoff defect:
the weighted sum of three identical vertex alphas could round 0.5 below the
occlusion threshold in one shader stage. Affine interpolation now preserves
constant alpha exactly, and hit-color interpolation uses the same form.
This removed the large first-frame query/pipeline hit/miss differences.
The remaining first-frame raw-signal differences are one RGBA16F storage ULP;
opaque receiver guides match exactly. Guided-path descriptor failures in the
first two iterations were fixed by binding each declared guide resource.

The guide investigation is retained in `build/checks/guide-buffer-probe-4` and
`build/checks/guide-query-aa`. After the first frame, query/pipeline guide arrays
differ by at most 2.98e-8; this grows to 1.81e-5 and 2.27e-4 on the next two
frames as the proposal changes. The ordinary query A/A control also diverges
over learned frames (347 raw half ULP by frame eight; counted control 688),
while its exact-accumulation control repeats. Cross-stage differences grow
faster and remain an open image-qualification question. Per-sample regression
checks now reset guide reuse between sample sequences, and separate exact-RT
repeatability checks retain eight learned frames. This does not waive the
captured-image A/A gates. Temporary guide-readback instrumentation was removed.

The first tiled-launch capture (`build/render-hardening-phase4-guided-tiled`,
820 runtime files) fails timing: HW whole frame 43.4517 ms, primary trace
11.48355 ms, receiver visibility 9.54163 ms, ReSTIR temporal/spatial
0.589824 / 4.7063 / 4.80973 ms, forward 1.637905 ms. Manual 8x8 launch
permutation is therefore removed; native ray-launch coordinates are restored.
The following source revision also removes redundant validity and cache-cell
fields from the ray payload while retaining exact surface metadata.

The tiled capture's static flicker/RMS are HW 0.0243030901 / 0.5521394110,
Compute 0.0105239200 / 3.3857785326, and editor 0.2094563988 / 1.0869070378.
All six frozen static limits pass once, despite its performance regression.

The compact-payload/native-launch revision passes 132 ray-suite tests (twelve
disabled, four skipped), 785 core tests (nine disabled) and 52 focused denoiser
tests, with clean validation
and no reported leaks (`build/checks/rt-guided-compact-2`). The production-path
test covers 65x65 output, reduced GI, ordinary/exact guides, ReSTIR, 64-sample
reference tracing, counted tracing and separate view histories in all four
submission modes. Receiver guides are exact. Matched-proposal raw signals differ
by at most one half ULP, or three in the instrumented count variant (four-ULP
diagnostic limit, reported separately). Issued-ray feature totals match exactly;
candidate counts are populated. Two repeat pairs, with and without ReSTIR,
retain exact raw-signal equality across eight learned RT frames. Adaptive
cross-stage image parity and performance qualification remain open.

`build/render-hardening-phase4-guided-compact` retains 820 runtime files for
the validated compact-payload/native-launch revision. Static flicker/RMS are
HW 0.0242770320 / 0.5514739387, Compute 0.0105246481 / 3.3857777730, and
editor 0.2094563782 / 1.0869070378; all six limits pass once. Warm HW whole
frame is 38.2262 ms, primary trace 11.88415 ms, receiver visibility 4.785105 ms,
ReSTIR temporal/spatial 0.545792 / 4.16768 / 4.20864 ms, deferred lighting
1.02885 ms, and forward transparency 1.654755 ms. The eight denoiser passes
sum to 7.1699025 ms. Compacting the payload does not establish an improvement
over the first primary port, and both timing gates still fail. Transparent
rendering remains below its frozen HW baseline, with the native layer,
transmission, attenuation, shadow and opaque-guide checks passing.

The isolated no-environment primary plain-trace comparison is retained in
`build/checks/primary-compact-plain`. With the same executable and all other
modules unchanged, primary trace drops from 11.88415 to 9.483375 ms and whole
frame from 38.2262 to 35.89775 ms. Forward remains 1.65478 ms; receiver visibility
is 4.78685 ms and ReSTIR temporal/spatial 0.542208 / 4.17997 / 4.22707 ms.
The helper records module hashes and restores/verifies both original modules.
No-environment primary programs now select plain tracing. Guided and diagnostic
programs retain reordering pending comparable measurements. The pipeline still
fails the replacement timing gate; this improvement does not qualify it.

The plain-primary selection passes the 132-test ray suite, 785 core tests and
52 denoiser checks with clean validation (`build/checks/rt-primary-plain`).
The existing `--pipeline-statistics` diagnostic now covers RT pipelines and
reports each shader group's stack sizes. This driver exposes no executable
statistics for these RT pipelines. Plain primary reports 320 raygen stack
bytes and 96 closest-hit bytes (reordered: 256 and 96); miss/any-hit report
zero. Logs and extracted Nsight counters are in
`build/checks/rt-pipeline-statistics`. These diagnostic launches are excluded
from timing results.

A temporary stack probe uses the actual primary-to-closest-hit-to-shadow call
chain, reducing the plain primary's automatic 512-byte stack to 416 bytes.
The calculation follows the [Vulkan stack contract](https://docs.vulkan.org/spec/latest/chapters/raytracing.html#ray-tracing-pipeline-stack);
the final nested rays skip closest-hit execution. Native validation passes
in `build/checks/rt-stack-probe`. The specialized probe is archived separately
for timing, and its renderer-specific backend code has been removed from
production sources. A production stack policy is not justified without a
measured benefit.

`build/render-hardening-phase4-stack-probe` measures primary trace at
9.470605 ms versus plain automatic-stack 9.483375 ms, receiver visibility
4.787805 ms, ReSTIR 0.544256 / 4.16358 / 4.20864 ms, forward 1.54299 ms,
and whole frame 35.36915 ms. Most of the frame difference is in the unchanged
G-buffer (1.445955 versus 1.88493 ms) and forward pass. The primary difference
does not demonstrate a useful stack optimization, so the probe is not retained.

Output stabilization now joins the existing disabled-sky specialization: it
omits sky shared storage, neighbourhood bounds and history reads when sky
lighting is disabled. The sky image remains one texel. The ten stabilization
clamping/reprojection scenarios now also run with that specialization and
one-texel sky inputs/history/output. The 132-case ray suite, 785-case core suite and 52-test
denoiser checks pass with clean validation and no reported leaks
(`build/checks/rt-stabilize-nosky-2`). The first iteration exposed a missing
new-program registration in the CPU mock fixture, which is fixed. Timing and
captured-image qualification of the specialization are pending.

`build/render-hardening-phase4-stabilize-nosky` records the plain-primary and
specialized-stabilization revision (820 runtime files). Static flicker/RMS:
HW 0.0242622984 / 0.5516560748, Compute 0.0105238321 / 3.3857767284, editor
0.2094563937 / 1.0869070378. All six frozen limits pass once. Warm HW whole
frame is 35.18985 ms, primary trace 9.49622 ms, receiver visibility 4.785505 ms,
ReSTIR 0.542208 / 4.13901 / 4.18406 ms and forward 1.5431 ms. Stabilization
falls to 0.820944 ms. The eight denoiser passes total 7.020493 ms, narrowly
above the budget; repeated timing and motion qualification remain open.

The next candidate enables the existing exact light-occluder triangle cache in
primary-hit lighting, for both the query reference and RT closest-hit paths.
`BindHitRadianceInputs` supplies the HW geometry cache or Compute radiance
inputs according to the active tier. The cache is built for HW primary lighting
independently of the receiver-shadow selection; with shadows disabled it writes
empty entries without rays. The same candidate specializes disabled-sky preblur.
It passes 132 ray tests, 785 core tests and 52 denoiser checks with clean
validation (`build/checks/rt-primary-cache`).

Review then identified that the first preblur specialization replaced the
per-receiver sky traversal-availability flag with a constant. Even without sky
radiance, temporal accumulation still consumes that flag. The source now keeps
the flag in scalar shared storage while omitting sky RGB work. The preblur
regression gains an unavailable-sky/valid-bounce case for both variants. This
correction postdates the `phase4-primary-cache` capture and requires its own
validation; that archive's preblur is a superseded candidate.

`build/render-hardening-phase4-primary-cache` fails the primary-cache timing
experiment: trace rises to 11.5438 ms from 9.49622 ms, whole frame 37.36155 ms,
receiver visibility 4.78744 ms, ReSTIR 0.539136 / 4.14925 / 4.20045 ms and
forward 1.654675 ms. The primary cache bindings/includes and the extra cache
build condition are removed. The receiver-shadow cache remains justified by
its separate measurements. Preblur measures 0.692912 ms and the eight-pass
chain 6.99163 ms in this superseded candidate; that number does not qualify
the corrected flag-preserving preblur. Static flicker/RMS are HW
0.0242622401 / 0.5516560748, Compute 0.0105244760 / 3.3857771083, and editor
0.2094564098 / 1.0869070378; all six frozen static limits pass once.

After removing primary-cache consumption and preserving disabled-sky
availability flags, the ray, core and 52-test denoiser suites pass with
clean validation and no reported leaks (`build/checks/rt-preblur-flags`).
Preblur now tests eight scenarios with sky enabled and disabled, including
unavailable sky beside a valid bounce signal. The no-sky variant retains
per-receiver raw alpha and scalar shared availability rather than reading a
one-texel constant. Only its RGB work and shared RGB storage are omitted.

`build/render-hardening-phase4-preblur-flags` retains the corrected revision
(820 runtime files). Static flicker/RMS are HW 0.0242622899 / 0.5516560748,
Compute 0.0105543820 / 3.3861080551, and editor 0.2094563824 / 1.0869070378;
all six frozen static limits pass once. Warm HW whole frame is 37.6384 ms,
primary trace 10.2842 ms, receiver visibility 5.144945 ms, ReSTIR
0.546816 / 4.2496 / 4.403715 ms, and forward transparency 1.53725 ms.
The eight denoiser passes total 7.033088 ms (preblur 0.720928 ms,
stabilization 0.823952 ms). Several unchanged passes also vary from the
preceding capture, so these individual captures do not establish a timing
distribution. The primary replacement and denoiser budgets still fail.

The isolated explicit-material reordering probe in
`build/checks/primary-material-reorder` passes the 132-case ray suite with
clean validation, but increases primary trace to 12.11345 ms (whole frame
38.0023 ms, receiver visibility 4.78579 ms, forward 1.65512 ms). It passes
the low eight material-index bits as an EXT invocation-reordering hint.
This does not improve on plain tracing; the original plain modules are
restored and hash-verified. No material-hint code is retained in production.

Environment-lit A/A qualification in `build/checks/guided-pipeline-aa` fails
cross-path parity. Four sequential 4K launches use query/RT/RT/query order,
identical input and shader hashes, and frames 289â€“320. Query A/A mean-image
RMS is 0.1017584/255 and RT A/A 0.0938360/255; cross-path RMS is
14.23717 and 14.23778/255, with about 51.23% of pixels differing by more
than 4/255. The RT image is visibly brighter. Static flicker is
0.0412425 / 0.0412218 for query and 0.0370940 / 0.0370011 for RT.
This is a correctness failure requiring signal-level diagnosis, not ordinary
guide accumulation noise. Environment lighting is enabled for this comparison;
these captures are separate from the frozen no-environment static-reference gate.

Signal isolation in `build/checks/guided-signals` places the large error in
visibility-only sky rays: first-frame sky RMS is 50.0181/255 with a -34.242
query-minus-RT bias. Bounce and specular first-frame RMS are only 0.04877 and
0.02575/255. Replacing only the two guided primary SER modules with their plain
counterparts reduces first-frame sky RMS to 0.001203/255
(`build/checks/guided-primary-plain`); the original modules were restored and
hash-verified afterward.

The primary pipeline now gives radiance and visibility rays separate hit groups,
including a shadow group with no closest-hit shader. Any-hit shaders declare the
incoming payload type for their group; the light-occluder group has its own
triangle payload declaration. This follows the
[Vulkan ray payload interface contract](https://docs.vulkan.org/spec/latest/chapters/interfaces.html#interfaces-raypipeline),
which requires matching payload types across the shaders reached by a ray.
The native hit-lighting check now follows each shaded primary with blocked and
unblocked visibility rays. The ray, core and 52-test denoiser suites pass with
clean validation and no reported leaks (`build/checks/rt-typed-anyhit`).
Production pipeline selection also requires recursion depth two, because primary
closest-hit lighting launches a shadow ray; the capability test covers depth-one
rejection and depth-two acceptance.

The matching 824-file runtime archive is
`build/render-hardening-phase4-typed-anyhit`. Its environment signal comparison
in `build/checks/guided-signals-typed` reduces first-frame sky RMS to
0.001203/255, with no pixels above 4/255 and a maximum channel difference of
1/255. Frame 64 sky RMS is 1.08029/255 and mean bias -0.00145/255.
Bounce/specular first-frame RMS remain 0.04877 / 0.02575/255. Settled-image
A/A qualification is still required: adaptive guide sequences diverge over
time, so raw later-frame differences alone are not a cross-path quality gate.

`build/checks/guided-pipeline-aa-typed` completes the same four-launch
environment-lit comparison. Query A/A mean-image RMS is 0.103707/255 and RT
A/A 0.104955/255; cross-path values are 0.108210 and 0.108322/255. Cross-path
mean biases are -0.000646 and +0.000022/255. Flicker is query
0.0412869 / 0.0411978 and RT 0.0411176 / 0.0411964. The large systematic sky
error is removed, but both cross-path RMS values remain about 3.2% above the
larger measured A/A value. This run does not yet qualify strict environment
parity; its executable, shader and input identities remained unchanged.

The typed-group revision's warm timing capture records whole frame 37.0558 ms,
primary trace 10.183 ms, receiver visibility 5.142205 ms, ReSTIR
0.544768 / 4.137985 / 4.18355 ms and forward transparency 1.65466 ms.
The eight denoiser passes total 7.053548 ms. These are the last captures
launched from a per-run executable archive.

The next revision keeps triangle indices and barycentrics local to closest-hit
shading. Raygen receives position, normal, radiance and material identity,
reducing the normal payload from 17 scalars to 12 without changing ray sampling
or material evaluation. `build/checks/rt-material-payload` passes 116 ray tests
(four fixture skips, twelve disabled), 776 core tests (nine disabled) and all
52 denoiser tests, with clean validation and no reported leaks. This revision
also corrects the debug history-length scale for divisor-squared history and
describes the new reconstruction fallback as history-fixed reuse.

`build/render-hardening-phase4-material-payload` uses the canonical executable
path, retaining hashes without copying the runtime. Warm whole frame is
34.7144 ms, primary trace 8.9332 ms, receiver visibility 4.785155 ms, ReSTIR
0.538624 / 4.13184 / 4.17843 ms and forward transparency 1.65451 ms. The
eight-pass denoiser totals 7.016563 ms. OffScreen and receiver visibility also
improve despite unchanged code, so the full frame-time reduction cannot be
attributed to the smaller payload. Primary trace is below the earlier plain
captures at approximately 9.49 ms, but remains above the query replacement
budget. Static flicker/RMS are HW 0.02418555 / 0.55160599, Compute
0.01055490 / 3.38610236 and editor 0.20945637 / 1.08690704; all six frozen
static limits pass once. Motion, repeated timing, strict environment parity
and the seven-millisecond denoiser gate remain open.

Rechecking no-environment primary SER with the smaller payload still loses:
`build/checks/primary-ser-material-payload` measures primary trace 11.25675 ms
and whole frame 37.102 ms, versus 8.9332 / 34.7144 ms for plain tracing.
Forward transparency is unchanged at 1.655645 ms and receiver visibility at
4.785345 ms. The native ray suite passes with its existing skips/disabled cases.
Only the two primary SPIR-V modules were substituted; both were restored and
hash-verified. The canonical executable was unchanged throughout. Plain tracing
remains selected for the no-environment primary path.

The preblur regression now includes a mature diffuse history beside a young
reflection history, with a specular impulse outside the one-texel diffuse
footprint. Before the fix, both sky variants read zero instead of reaching
that impulse; all four submission-mode cases fail for that reason
(`build/checks/preblur-footprint-red`). A neighbouring receiver with a different
identity remains excluded. Preblur now expands specular sample coordinates
when its radius exceeds the diffuse radius and reuses the diffuse fetches
when the texels coincide. Mature/smaller specular footprints retain their
existing sampling and weights. All 52 denoiser cases pass with validation
and no reported leaks (`build/checks/preblur-footprint-green`). Its 4K quality,
motion and timing qualification is in progress.

`build/render-hardening-phase2-preblur-footprint` completes that qualification
attempt. Static flicker/RMS remain within all six frozen limits: HW
0.02418549 / 0.55160599, Compute 0.01053597 / 3.38553233 and editor
0.20945549 / 1.08690349. HW slow-pan error is
0.173792 / 0.223802 / 0.230662%; fast-pan is
0.261454 / 0.484640 / 0.296188%. The footprint correction does not resolve
the 50% motion gate. Warm whole frame is 34.6754 ms, primary trace 8.911295 ms,
receiver visibility 4.784255 ms, ReSTIR 0.539648 / 4.127745 / 4.174335 ms
and forward transparency 1.534525 ms. Preblur costs 0.787152 ms; the eight
denoiser passes total 7.102894 ms, above budget.

The isolated `build/checks/visibility-hitobject` probe reads visibility directly
from EXT hit objects, omitting reordering and hit/miss shader execution for
visibility-only rays. It covers primary closest-hit shadows, raygen visibility,
receiver shadows and ReSTIR; no executable copies are made. The native ray
suite passes, and primary trace falls to 8.660575 ms, but spatial ReSTIR rises
to 5.95251 / 5.99347 ms. Whole frame regresses to 38.1649 ms, receiver
visibility is 4.765105 ms and forward transparency 1.65467 ms. The combined
candidate is rejected and all substituted modules are restored and verified.
An isolated follow-up retains reordering for ReSTIR while still avoiding its
miss-shader execution.

The follow-up `build/checks/visibility-hitobject-reorder` passes all twelve
focused hit-lighting, GI-trace and ReSTIR native checks with clean validation.
It measures primary trace 8.670225 ms, receiver visibility 4.76643 ms,
ReSTIR 0.5376 / 4.04992 / 4.096 ms, forward transparency 1.65421 ms and whole
frame 34.3659 ms. All temporary modules are restored afterward. This version
is being integrated: capability-selected EXT/NV variants now include closest-hit
shaders, visibility reads the hit object directly, and ReSTIR preserves its
reordering before consuming visibility. No-guide primary shading remains plain.
The shader build also handles the combination of invocation-reordering and
compact-hit-stream variants explicitly.

The next denoiser candidate lets output-resolution GI retain the configured
slow history, scaled by divisor squared and bounded by the eight-bit stored age.
The previous eight-frame limit remains for the legacy chain. Surface/lobe
rejection and the current 3x3 radiance clamp remain in place. Native coverage
adds mature reflection/diffuse edge history and a short configured history limit.
This is an unqualified motion-noise candidate until its native and 4K gates run.

`build/checks/rt-hitobject-output-history` passes 116 ray tests (four fixture
skips, twelve disabled), 776 core tests (nine disabled), and all 52 denoiser
tests with clean validation and no reported leaks. The integrated direct
hit-object visibility and extended output history are measured in
`build/render-hardening-phase2-output-history`, using the canonical executable.
Static flicker/RMS are HW 0.01544060 / 0.54806138, Compute
0.00498837 / 3.38543608 and editor 0.17868973 / 1.05615868; all six frozen
static limits pass once. HW slow-pan error is 0.141999 / 0.193251 / 0.217183%;
fast-pan is 0.230481 / 0.475140 / 0.274281%. These improve on the prior
candidate but still fail the required 50% reduction.

Warm whole frame is 34.25035 ms, primary trace 8.665505 ms, receiver visibility
4.76784 ms, ReSTIR spatial 4.05504 / 4.09395 ms and forward transparency
1.54197 ms. The eight denoiser passes total 7.093789 ms. Nsight reports
9,938 MiB committed VRAM, still above the eight-GB budget. Neither the
motion, timing nor memory gates are closed by this change.

`build/checks/pan-output-history-debug` captures diffuse and reflection history
at fast-pan pose 32 and at the same settled camera. The worst 96x96 error patch
(x=192, y=1536) has median encoded history 8/255 while moving, corresponding
to approximately four samples of the 128-frame limit; its settled history is
255/255. A trial that fades luminance rejection from age four to 32 passes
all 52 native denoiser cases (`luminance-confidence-native`) but changes this
pose's >4/255 error only from 0.475140% to 0.473669%, with RMS slightly worse
(0.735820 to 0.736060). The trial and its trial-specific test cases are removed.
This result does not establish luminance edge stopping as the main cause.

The combined exponential-weight trial (`build/render-hardening-phase2-weight-fusion`)
passes 52 native denoiser cases after rebuilding the reverted test source
(`denoise-weight-fusion-native-fresh`). Its first native run used a stale test
binary because restoring a file preserved its older timestamp; the four failures
were the removed luminance-confidence cases, not weight-fusion checks.
The combined trial measures whole frame 34.43265 ms. History fix improves from
0.807376 to 0.565088 ms, but the eight denoiser passes total 7.306042 ms;
the combined change is rejected. A follow-up isolates fusion to history fix.

The isolated history-fix change (`build/render-hardening-phase2-history-weight-fusion`)
passes all 52 native denoiser cases and all six frozen static flicker/RMS limits:
HW 0.01546449 / 0.54793158, Compute 0.00500314 / 3.38558194 and editor
0.17868972 / 1.05615868. Its first warm trace measures whole frame 34.17895 ms,
history fix 0.722592 ms and the eight denoiser passes 6.984883 ms. Primary trace
is 8.689905 ms, receiver visibility 4.76664 ms and forward transparency
1.541555 ms. This is a first sub-seven-ms result, not a repeated timing pass.
The isolated history-fix optimization is retained for subsequent qualification.

The preblur regression now also varies diffuse age while keeping reflection
history fixed, for both mature and young reflections and both sky variants.
`build/checks/preblur-independent-red` fails all four submission modes: the wide
diffuse footprint loses the reflection impulse entirely (zero instead of
0.010178 or 0.468262). Giving reflection its own coordinates exposes a second
dependency: an out-of-view diffuse tap still skips an in-view reflection tap
(`preblur-independent-green`, despite that trial directory's name, fails).
Preblur now samples each signal at its own radius and checks their viewport
bounds independently. Subpixel reflection kernels retain a one-texel sampling
footprint with their original narrow weights. All 52 cases pass with clean
validation and no reported leaks in `preblur-independent-bounds-green`.
The matching 4K quality and performance checks are pending.

The focused fast-pan pose-32 check (`build/checks/preblur-independent-pan-pose`)
measures 0.471680% over 4/255 and RMS 0.732077, versus 0.475140% and 0.735820
before the independent-footprint correction. This is a small improvement and
does not meet the motion gate. The full three-preset static, HW pan and warm
timing sequence is running in `build/render-hardening-phase2-independent-preblur`.

Its static sequence passes all six frozen limits once: HW flicker/RMS
0.01544196 / 0.54809644, Compute 0.00498153 / 3.38544701 and editor
0.17472296 / 1.06277633. The current Compute log still starts local field
generation 5 (215,361 triangles, 925,107,920 scratch bytes) without publishing
it before frame 320; the previously recorded construction-progress issue is
not fixed by these denoiser changes.

The independent-preblur sequence completes with HW slow-pan errors
0.142494 / 0.196277 / 0.218883% and fast-pan errors
0.232145 / 0.472487 / 0.272569%; the 50% reduction still fails. Warm whole
frame is 34.2227 ms, primary trace 8.653105 ms, receiver visibility
4.76751 ms, ReSTIR 0.539648 / 4.07347 / 4.115455 ms and forward transparency
1.54472 ms. Preblur increases to 0.82784 ms; the eight-pass denoiser totals
7.032063 ms, just above budget. The correctness regression remains fixed,
but the timing and motion qualification remain open.

`build/checks/pan-reconstructed-signals` isolates the reconstructed bounce and
reflection debug views at fast-pan pose 32. In the previously identified 96x96
patch, bounce display-space red averages 30.061 while moving versus 12.975
settled; reflection averages 22.281 versus 12.075. The patch error is therefore
not confined to reflections. These are sRGB debug-image values, not linear
radiance ratios. `RecordDenoiser` already writes clamp/repair output into the
slow-history textures consumed by the next frame; missing repair feedback is
not the explanation. A separate primary-RT probe will compare explicit
hit-object execution with the plain trace, retaining the same executable.

`build/checks/primary-hitobject-execute-hit` tests tracing a hit object and
executing closest-hit only on hits, leaving the initialized miss payload alone.
All 116 native ray tests pass (four fixture skips, twelve disabled), with clean
validation and no reported leaks. Warm primary trace is 8.867855 ms versus
8.653105 ms for the current plain trace; whole frame is 34.5103 ms versus
34.2227 ms. The candidate is rejected. Both original shader-module hashes
were verified after restoration; the canonical ZenApp executable is unchanged.

`build/checks/pan-raw-signals` and `pan-unreused-signals` compare the same
moving/settled patch before denoising, first with ReSTIR and then with its
diffuse/specular reuse disabled. With reuse, approximate linear red means
(decoded from the debug PNG) are bounce 0.010604 / 0.003989 and reflection
0.008650 / 0.003947. Fresh samples give bounce 0.003717 / 0.002739 and
reflection 0.002414 / 0.003295. These are single-frame diagnostics: fresh
samples have high variance, and up to 0.174% of patch pixels clip. They
identify reuse correlation as a candidate source, not proof of estimator bias.
Disabling ReSTIR also changes the primary shader variant (including diffuse-hit
normal-map treatment and reflection proposal reuse), so this is not a pure
reuse ablation. No sampling or reuse setting is changed in production by it.

The independent-footprint preblur now loads reflection only at its own sample
coordinate instead of loading and discarding the diffuse-coordinate reflection.
All 52 denoiser tests pass with clean validation and no reported leaks
(`build/checks/preblur-signal-loads-native`). The focused fast-pan check reports
0.471933% over 4/255 and RMS 0.732199; it does not materially improve motion.
The moving/settled captures are not byte-identical to the preceding revision
(maximum differences 4/255 and 6/255 respectively).

`build/render-hardening-phase2-preblur-signal-loads` passes all six static
limits once: HW flicker/RMS 0.01544191 / 0.54809644, Compute
0.00499733 / 3.38553807, editor 0.17472296 / 1.06277633. Its warm trace
measures preblur 0.763328 ms (previously 0.827840 ms), whole frame
34.12785 ms, primary trace 8.668575 ms, receiver visibility 4.76712 ms
and forward transparency 1.54325 ms. The eight denoiser passes total
6.986957 ms. The redundant-read removal is retained; repeated timing,
motion and the other outstanding gates are still required.

`build/checks/restir-temporal-cap-8` privately caps inherited reservoir confidence
at eight while retaining the primary shader and spatial reuse. Fast-pan pose 32
has RMS 0.688743 (previously 0.732199), but its >4/255 error rises from
0.471933% to 0.473693%. The motion gate does not improve; the candidate is
rejected. The temporal module and canonical executable hashes are verified
after restoration. The production confidence limits remain unchanged.

`build/checks/relax-light-path-qualified-setup` moves authored point light 3 by
0.04 scene units in four steps, then captures 16 recovery frames. The canonical
ZenApp renders 3840x2160 with 1920x1080 GI in a 1920x1080 window; this is a
lighting-response diagnostic, not a fullscreen timing capture. All five launches
have clean validation and no reported leaks, with unchanged provenance hashes.
The first attempt (`relax-light-path-current`) rejected legacy fields in the
unconverted archived input before rendering; the successful run uses the probe's
current `hardware.zenrender` instead.

The settled first/last light poses differ by more than 4/255 at 235,795 pixels,
so the light meaningfully affects the view. At the final moving pose, mean
max-channel error is 0.422765/255, falling to 0.347698 after 16 recovery frames;
the settled reference's own residual is 0.032229. In the affected pixels, error
falls from 2.825539 to 1.876647/255, versus reference residual 0.018143. This
establishes a remaining response gap, not a lighting-lag pass. A private output
stabilization experiment uses the existing lighting-response flags to test
whether its long output history contributes to that gap.

The output-only lighting-response cap experiment completes in
`build/checks/output-lighting-responsive`. Limiting output accumulation to eight
frames during the existing response window does not improve recovery: final
whole-image error is 0.358/255 versus 0.348/255 before. The experiment is
rejected and the stabilization module restored with matching hashes. Inspection
of the new temporal shader shows it ignores the CPU's per-signal history-reset
flags, which are asserted for traced-lighting revisions; a separate three-stage
probe checks honoring those flags in preblur, temporal and output stabilization.

`build/checks/denoiser-lighting-reset` honors the per-signal reset flags in
preblur, temporal accumulation and output stabilization. It improves the changed
region's final error from 1.876647 to 0.227515/255 (p99 from 6 to 1), but
whole-image error increases from 0.347698 to 0.459043/255; the last moving
frame rises from 0.422765 to 1.076010/255. This exposes the response/noise
tradeoff of restarting mature histories globally. The probe is not adopted;
all three modules were restored and the executable hash verified. The next
isolated probe selects the already-updated fast bounce/reflection histories
during the existing lighting-response window, preserving geometry history ages.

`build/checks/history-fast-lighting-response` selects the updated per-pixel fast
history during the response window. At the last moving frame / recovery frame
16, changed-region error is 1.124264 / 0.378145, compared with production
2.825539 / 1.876647. Whole-image error is 0.621288 / 0.427908 instead of
0.422765 / 0.347698. It improves response but still raises global noise, and is
not adopted. All five launches have clean validation/no reported leaks; the
original history-fix module and executable hashes are verified afterward.
A follow-up keeps the slow history and tightens only the response-window clamp,
so unchanged estimates inside the fast-history bounds can remain accumulated.

`build/checks/history-responsive-clamp` tightens the bounce/reflection clamp
from four to one sigma only during the existing response window. It still
increases mean whole-image recovery error (0.396842/255 versus 0.347698/255).
However, recovery pixels over 4/255 fall from 0.221764% to 0.085986%, and
changed-region mean error falls from 1.876647 to 1.031823/255 (p99 6 to 3).
At the last moving frame, pixels over 4/255 fall from 0.982470% to 0.243405%.
This trades slightly more low-level noise for fewer visible large errors. Its
history-fix module is restored; the candidate needs native and static-quality
checks before adoption. The lighting-response gap is not declared closed.

The next camera-motion probe expands the preblur footprint only for histories
younger than 16 frames. The measured bright patch spans roughly the 32-GI-texel
ReSTIR spatial radius, while the current age-four preblur reaches only a few
texels. This tests a spatial-support hypothesis without changing mature-history
kernels, ray counts or transparent-object shading. It is an isolated shader
experiment, not a qualified kernel change.

`build/checks/preblur-young-wide-pan` measures 0.462445% over 4/255 and
RMS 0.713992 at fast-pan pose 32, versus 0.471933% and 0.732199 for
production. The improvement is small relative to the required 50% reduction;
the broader kernel is not adopted. The private preblur module is restored and
the canonical executable hash verified. The lighting-clamp candidate is now
being checked with a native regression that distinguishes responsive hit
radiance from stable history and unchanged sky.

The lighting-response clamp is implemented for qualification: bounce and reflected
radiance use a one-sigma fast-history box while the existing response flags are
set, returning to four sigma afterward. Sky retains its separate invalidation
and four-sigma behavior. The box still contains the center fast estimate; no
geometry history age or sample count is reset.

The expanded native spatial-clamp case includes a chromatic gradient whose
four-sigma box admits stale radiance, both with and without the response flag.
It also checks that sky retains the same mature estimate. All four submission
modes fail the added response checks in `build/checks/responsive-clamp-red`
(old value 3 instead of a value below 2). All 52 denoiser tests then pass in
`responsive-clamp-green`, with clean validation and no reported leaks. Matching
three-preset static checks and warm timing are running in
`build/render-hardening-phase2-responsive-clamp`; defaults remain unchanged.

`build/render-hardening-phase2-responsive-clamp` passes all six static limits
once: HW flicker/RMS 0.01543342 / 0.54815602, Compute
0.00496320 / 3.38550448, editor 0.17472334 / 1.06279448. Warm whole frame is
34.17370 ms, primary trace 8.685505 ms, receiver visibility 4.766705 ms,
forward transparency 1.543475 ms, and history fix 0.732160 ms. The eight
denoiser passes total 6.983725 ms. This retains the candidate's static quality
and timing, but does not close whole-frame or motion qualification.

The private compute probes above used glslang's SPIR-V 1.4 target, whereas the
production compute shader build uses SPIR-V 1.3 (RT shaders use 1.4). The
history-clamp disassembly differs only in entry-point interface declarations and
unsigned image-fetch extension operands, with the same numeric operations, but
the production lighting sequence is being rerun rather than assuming identical
driver output. Future compute probes must match the production target.

`build/checks/responsive-clamp-production-light-path` confirms the change with
the production SPIR-V 1.3 module and canonical executable. At recovery frame
16, pixels over 4/255 are 0.080693% (baseline 0.221764%), changed-region
mean error is 1.032583/255 (baseline 1.876647), and changed-region
p99 is 3/255 (baseline 6). Mean whole-image error is
0.397325/255 versus 0.347698: the measured tradeoff is fewer large errors
with more low-level noise. All five launches have clean validation, no reported
leaks and unchanged provenance. The response clamp is retained in the opt-in
prototype; these results do not declare lighting response fully qualified.

All four transparent-layer integration modes also pass with the production
shader (`build/checks/responsive-clamp-transparent`), with clean validation
and no reported leaks. The tests cover layer ordering, per-layer shadows,
transmission/refraction, 32 lights plus the camera light, and preservation of
opaque receiver guides. No default or tier switch is made by this revision.


`build/checks/history-young-pyramid` extends the existing mip repair to ages
under 16, selecting progressively finer levels and fading the extra weight.
It uses the production SPIR-V 1.3 target. Fast-pan pose 32 improves from
0.471933% to 0.448315% over 4/255 (RMS 0.707662), still far short of the
required 0.199424% limit. The candidate is not adopted; the history-fix module
and canonical executable hashes are verified after restoration.

`build/checks/fast-pan-query-current` runs the same current denoiser through
the query path at that pose. Its error is 0.472355% over 4/255, RMS 0.732356,
versus 0.471933% / 0.732199 for RT. This single pose does not qualify overall
A/A parity, but does not support attributing the remaining motion error to the
RT port. Both launches reuse the canonical executable without copying it.

A subsequent data-flow review finds that spatial ReSTIR replaces raw bounce
and reflected radiance while leaving the original ray batch's hit-distance
guides unchanged. ReLAX uses those guides for the preblur footprint and virtual
reflection reprojection. A candidate publishes the selected reservoir distance
alongside each reused radiance estimate only when ReLAX is enabled. It keeps
the reflection availability marker and the legacy-chain guides unchanged.
Native and 4K qualification are pending; no motion improvement is assumed.


The distance regression initially fails in all four submission modes:
`build/checks/restir-distance-red` reports 24 guide assertions per mode plus
eight assertions caused by aliasing the new reflection fixture's active and
unused reservoir outputs. After fixing that fixture alias, all 116 ray tests,
776 core tests and 52 denoiser tests pass in `restir-distance-native`, with
clean validation and no reported leaks (four ray fixture skips and twelve
ray/nine core disabled cases remain). The intermediate `restir-distance-green`
run still contains the fixture error and is not a passing result.

The shader publishes a distance only in the spatial pass that publishes the
reused radiance. It preserves the other reflection-guide channel, inactive
signals and background; it keeps the original guides when ReLAX is disabled.
The expanded fixture covers both checkerboard parities, diffuse/reflection,
query/RT and legacy preservation. `build/checks/restir-distance-pan` measures
0.457827% over 4/255 and RMS 0.728041 at fast-pan pose 32, compared with
0.471933% / 0.732199 before. The partial improvement does not close the pan
gate. Static quality and warm timing are being measured in
`build/render-hardening-phase2-restir-distance` using the canonical ZenApp.


`build/render-hardening-phase2-restir-distance` passes all six static limits
once: HW flicker/RMS 0.01535259 / 0.54811127, Compute
0.00498046 / 3.38562036 and editor 0.17472333 / 1.06279448. Warm whole
frame is 34.315 ms, trace 8.64174 ms, receiver visibility 4.765775 ms,
ReSTIR spatial 4.08986 / 4.13696 ms and forward transparency 1.65494 ms.
The eight denoiser passes total 6.984670 ms. This is a single-run result;
unchanged forward rendering also varies from the prior capture, so the whole
frame difference is not attributed entirely to the extra guide writes.
The guide correction is retained in the opt-in denoiser. Whole-frame cost,
repeated qualification and the 50% motion requirement remain open.


The isolated `build/checks/occluder-cached-area` probe stores twice each cached
triangle's area in the unused `edge1.w` slot, avoiding a cross product and length
in every receiver's determinant-tolerance test. It adds no buffer memory and
keeps the finite-segment and triangle-border predicates. Warm receiver visibility
falls from 4.765775 to 4.682145 ms; whole frame is 34.18875 ms. The unrelated
forward pass also varies (1.542785 ms), so this is evidence for the local shadow
pass improvement, not an isolated attribution of whole-frame savings.

The private probe's four native tests pass, but its replaced calibration compute
module does not include the opacity-micromap variant selected on this device.
That private native run therefore does not establish the new cache predicate's
correctness. The integrated build recompiles all consumers and variants. Its full
ray suite (`build/checks/occluder-area-native`) passes all 116 active tests, with
four fixture skips/twelve disabled experiments, clean validation and no reported
leaks. This includes the changed predicate's candidate-versus-HW checks and the
four transparent-layer modes. Application static/timing qualification is running
in `build/render-hardening-phase4-occluder-area` with the canonical executable.


`build/render-hardening-phase4-occluder-area` confirms receiver visibility at
4.682580 ms with the production shader variants (previously 4.765775 ms).
Whole frame is 34.151 ms, primary trace 8.701920 ms, forward transparency
1.525840 ms and the eight denoiser passes 6.990800 ms. All six static limits
pass once: HW flicker/RMS 0.01540391 / 0.54797545, Compute
0.00496522 / 3.38549521 and editor 0.17472333 / 1.06279448. The HW final
image is not byte-identical to the previous capture: 157,099 pixels change,
383 by more than one level and six by more than four, maximum seven. The
cache optimization is retained; these captures establish the measured local
cost reduction and static acceptance, not final A/A or motion qualification.

A resource-consumer audit finds no read of guide-pyramid level zero: history
repair reads only signal levels one through three. The next revision removes
that unused texture level and its writes while keeping the shared-memory
reduction and all signal levels. It reduces declared guide storage by
66,355,200 bytes (63.28125 MiB) per half-resolution 4K GI pyramid, before
allocator/in-flight overhead. This is a format-based saving, not a measured
peak-VRAM reduction or completion of the eight-GB gate. Existing native tests
cover all reduced levels, missing samples, separate surfaces and partial groups.


The guide-pyramid revision passes all 52 denoiser tests in
`build/checks/guide-mip-native`. Renderer integration passes 776 core tests
(nine disabled) and all four transparent-layer cases in `guide-mip-renderer`.
All logs have clean validation and no reported leaks. The change packs logical
guide levels one through three into physical levels zero through two; signal
levels and the 8x8 shared reduction are unchanged. Focused motion and the
three-preset static/warm timing checks are running before acceptance.


`build/checks/guide-mip-pan` retains the focused fast-pan error at
0.45782697% over 4/255 and RMS 0.72804121. Against `restir-distance-pan`,
the moving image differs by one channel value and the settled image by two,
all by 1/255. This comparison includes the intervening cache-area optimization.

`build/render-hardening-phase2-guide-mips` passes all six static limits once:
HW flicker/RMS 0.01540391 / 0.54797545, Compute
0.00498113 / 3.38561185 and editor 0.17472334 / 1.06279448. HW static
metrics exactly match the preceding cache-area capture. Warm preblur is
0.748064 ms (previously 0.765264), the eight denoiser passes total
6.962097 ms, receiver visibility is 4.682960 ms and forward transparency
1.525485 ms. Whole frame is 34.02550 ms and primary trace 8.638785 ms;
the unchanged trace also varies, so the total improvement is not attributed
entirely to removing the guide level.

Nsight `BASE/REPRO_INFO.xls` reports requested/committed VRAM
9,735 / 9,750 MiB versus 9,924 / 9,938 MiB in the preceding capture,
with zero demoted bytes in both. The measured committed reduction is
188 MiB across the application's retained allocations, not just the single
63.28125-MiB texture-level saving. The guide-layout change is retained.
All three quality logs have clean validation and no reported leaks; capture
provenance checks pass. The eight-GB budget and full motion/RT cost gates
remain open, and no defaults switch in this revision.

The controlled reuse probes `build/checks/restir-reuse-without-temporal` and
`restir-reuse-without-spatial` preserve the primary shader and its proposals;
they only disable the selected reuse loop. All three RT variants are compiled
with SPIR-V 1.4 and substituted, then restored with hash checks. At fast-pan
pose 32, removing temporal reuse measures 0.607796% over 4/255, RMS
0.706962; removing spatial reuse measures 1.135176%, RMS 0.940124.
Both worsen the threshold metric against 0.457827% / 0.728041. Neither
ablation is retained. This corrects the earlier confound where disabling
ReSTIR also changed primary proposals; it does not prove that all correlated
reuse noise has been eliminated.

`build/checks/history-wide-repair` extends repair to histories under eight
frames and spaces the existing 4x4 mip samples four texels apart, preserving
surface/lobe rejection and scaling the kernel distances with the footprint.
The focused fast-pan result is 0.369201% / RMS 0.666552. The eight-texel
spacing probe (`history-wide-repair-8`) measures 0.267470% / 0.638577.
Both use the production compute SPIR-V 1.3 target, the same canonical
executable and restored modules. These are experimental footprints, not
retained changes: neither meets the 0.199424% gate and the wider blur still
needs native, other-pose, lighting-response and static qualification.

The next memory revision gives `BounceMoments` a one-texel placeholder in
ReLAX, whose temporal/repair/atrous passes never consume that legacy texture.
It also sizes both learned-guide buffers as descriptor placeholders when the
selected shader has `HYBRID_NO_GUIDE`; environment and diagnostic variants
still allocate the full tile arrays. A storage change clears guide validity
even if recording is declined, because the old allocation no longer exists.
The same-view native trace fixture now alternates no-guide and deterministic
guide variants, covering growth and shrinkage for both query and RT paths.
`build/checks/unused-history-native` passes 116 ray tests (four fixture skips,
twelve disabled), 776 core tests (nine disabled) and all 52 denoiser tests.
Validation and leak checks are clean.

`build/render-hardening-phase2-unused-history` passes all six static limits
once: HW flicker/RMS 0.01540394 / 0.54797545, Compute
0.00498181 / 3.38557896 and editor 0.17472333 / 1.06279448.
The warm capture measures whole frame 34.03340 ms, primary trace 8.643970,
receiver visibility 4.683635, forward transparency 1.525215 and the eight
denoiser passes 6.954144. These are essentially unchanged from the preceding
capture. The resource-size reduction at half-resolution 4K is approximately
55.61 MiB (31.64 MiB of legacy moments and 23.97 MiB of learned-guide arrays).
Nsight still reports requested/committed VRAM 9,735 / 9,750 MiB, with zero
demotion. Thus the smaller resources do not demonstrate a reduction in
committed device memory; allocator block retention is a possible explanation,
not a measured breakdown. The memory gate remains open. The native and
capture runs use the rebuilt canonical executable, SHA256
`f8913cb7996df16bd9ae75c911e4617e72bf35ae4ee220837623597efe90247a`.
Frame 320 differs from the preceding guide-mip capture at two pixels in
HW, 1,119 in Compute and two in editor, all by at most 1/255. Thus these
images are not byte-identical, although the static reference limits pass.

`build/checks/primary-shared-control` removes one payload scalar by using
the material word for the incoming normal-map flag, then overwriting it
with the closest-hit material or the miss sentinel. Forty private RT modules
cover normal, counted, reordered and compact-hit-stream variants. The 116
native ray tests pass (four fixture skips, twelve disabled), with clean
validation and no leaks. However, warm primary trace rises from 8.643970
to 8.814270 ms; whole frame is 34.22705 ms. Unchanged forward/reconstruction
timings also vary, so the whole-frame delta is not attributed solely to the
payload. The probe is rejected and all original module hashes are verified
after restoration; the canonical executable is unchanged.

The sixteen-texel repair-spacing probe (`history-wide-repair-16`) measures
0.267011% over 4/255 and RMS 0.645283 at fast-pan pose 32. It provides no
useful improvement over eight-texel spacing. The subsequent native check of
the eight-texel candidate (`history-wide-repair-8-qualification`) fails all
four submission modes of `RelaxPyramidRepairsMissingSamplesWithoutCrossingSurfaces`:
near a small viewport's boundary, its widely spaced coordinates can all fall
outside the mip image, leaving zero output that later spatial passes spread.
The other 48 tests pass, but no quality sequence runs after this failure.
The module is restored. A private follow-up clamps those sample coordinates
to the mip bounds; native and three-preset static/HW pan qualification are
pending. The production repair remains unchanged.

The bounded-footprint candidate passes all 52 native denoiser checks
(`history-wide-repair-8-clamp-qualification`). Its private capture sequence,
`build/render-hardening-phase2-history-wide-repair-8-clamp`, passes all six
frozen static limits once: HW flicker/RMS 0.01540333 / 0.54811914, Compute
0.00501791 / 3.38592742 and editor 0.17472289 / 1.06278843.
HW slow-pan error at poses 8/32/64 is 0.127170 / 0.160108 / 0.170983%;
fast-pan is 0.160289 / 0.282600 / 0.201980%. This improves every measured
pose against the preceding full-pan revision (independent preblur), but only
fast-pan poses 8 and 64 meet the required 50% reduction against phase 0.
The candidate remains opt-in and cannot justify switching defaults. The
private module is restored after capture. Integration adds native checks
that a six-frame history receives repair while an eight-frame history does
not; production-build validation and timing are pending.

`build/checks/wide-repair-age-red` fails twelve six-frame signal assertions
per submission mode with the original shader. After integration,
`wide-repair-age-green` passes all 52 denoiser tests, including empty, six-frame
and eight-frame histories, viewport boundaries and unrelated receivers.
The production shader removes the redundant bounds branch after clamping,
so its module hash differs from the private candidate. The production
focused capture (`wide-repair-production-pan`) measures 0.283722% over 4/255
and RMS 0.637575, versus 0.457827% / 0.728041 before this repair.
`wide-repair-production-checked` passes all four transparent-layer tests with
clean validation and no leaks. The earlier `wide-repair-production` attempt
selected zero tests with an incorrect filter and is not passing evidence.
The final production static and timing sequence is pending.

`build/render-hardening-phase2-wide-repair-production` completes with all
six static limits passing once: HW flicker/RMS 0.01540332 / 0.54811914,
Compute 0.00499578 / 3.38581222 and editor 0.17472290 / 1.06278843.
All three validation logs are clean and report no leaks. The warm capture
measures history fix 0.712336 ms, the eight denoiser passes 6.980947,
primary trace 8.625090, receiver visibility 4.681810, forward transparency
1.522275 and whole frame 34.07015. Nsight requested/committed VRAM remains
9,735 / 9,750 MiB, with no demotion. The unchanged first atrous pass varies
from 0.928177 to 0.966577 ms, so the small whole-chain timing change is not
attributed solely to repair. The current repair is retained behind the
existing opt-in setting; defaults and the old chain remain unchanged.

The full four-mode ray/core suites passed after the allocation change;
the 52 denoiser and four transparency cases passed after integrating repair.
The latter tests are the verification scope for this final shader revision,
not a second full ray/core-suite run. Formatting and both repository diff
checks pass. All forty primary payload-probe modules are verified restored;
the only retained shader change in this continuation is history repair.
The canonical executable remains at the SHA256 recorded above. Production
pan repeats across every preset, dolly, sparks, complete lighting response,
strict query/RT A/A parity, RT replacement cost and the memory gate remain
open before phases 5 and 6 can close.

### Compact receiver visibility (2026-11-14)

The receiver-shadow image now uses RG32UI when fewer than 32 scene lights
are enabled. Its two scene-light masks remain intact for forward reuse;
the unused top visibility bit stores the camera light. At 32 scene lights,
the renderer selects an RGBA32UI shader variant and keeps camera visibility
in its own component. The selection uses the same uploaded enabled-light
count as the shaders. Per-layer forward shadow queries and GI matching are
unchanged.

The transparency regression now also removes an unused light after testing
all 32 slots, exercising the wide-to-compact transition with the camera light
enabled and disabled. Both formats match ray-query colors, direct lighting
and opaque receiver guides across all four submission modes. The expanded
test passed before and after the change (`compact-visibility-baseline` and
`compact-visibility-native`). The complete ray suite then passed 116 cases,
with four fixture-dependent skips and twelve disabled experiments, clean
Vulkan validation and no reported leaks.

`build/render-hardening-phase4-compact-visibility` records the canonical
ZenApp executable SHA256
`ef654d7a9c2736acb0385f957317311940cdc7131c14cf9c9278d2b166afd2e0`.
All 32 HW static PNG hashes match `wide-repair-production` byte for byte;
flicker/RMS remain 0.01540332 / 0.54811914. Warm 4K fullscreen profiling
uses 1920x1080 GI, base clocks, threaded RHI and the recorded async policy.
Receiver visibility measures 4.661005 ms, forward transparency 1.524335 ms
and whole frame 34.10240 ms, versus 4.681810 / 1.522275 / 34.07015 ms
previously. This establishes a memory saving, not a whole-frame speedup.

Each full-size visibility allocation is 63.28125 MiB smaller. Frame 320's
RDG pool reports 1,088,492,000 bytes assigned and the same amount available,
down from 1,154,847,200 each. Nsight requested/committed VRAM falls from
9,735 / 9,750 MiB to 9,608 / 9,622 MiB, with no demotion. The measured
128 MiB committed reduction does not close the 8 GB pressure qualification.

### Rejected unused filter-output allocation experiment

`build/render-hardening-phase2-unused-filter-output` replaces the legacy sky
guide and the Compute previous-cache outputs with 1x1 bindings during ReLAX.
It also skips the unused sky-guide calculation and moves the legacy calculation
after tracing. The candidate sources are retained in that capture directory;
both production source files have been restored byte for byte.

The native ray suite passed 116 cases (four skips, twelve disabled), and all
52 denoiser cases passed. The core suite exposed a test assumption: a pooled
texture's allocation label can describe a resource from an earlier graph.
`HybridHistoriesPublishOnlySuccessfulFramesAndResizePerView` now checks the
actual shader/descriptor bindings instead of allocation labels. It checks all
four slow/fast history bindings and four intermediate outputs. All 776 enabled
core tests pass with this correction, both during the candidate and after its
revert; nine cases remain disabled (`unused-filter-correct-bindings` and
`compact-visibility-restored`). Validation and leak checks are clean. This
test correction remains; the experimental allocation assertions were removed
with the allocation change.

All six static quality limits passed once. HW flicker/RMS measured
0.01540086 / 0.54811182, Compute 0.00501054 / 3.38596014 and editor
0.17472289 / 1.06278843. These captures are not byte-identical to the preceding
production build: frame 320 differs at 4,535 HW pixels (maximum 1/255),
22,200 Compute pixels (maximum 21/255) and zero editor pixels. The Compute
publication-timing equivalence issue remains open; this run does not isolate
the cause of its differences.

The warm capture measures trace 8.633470 ms, eight-pass denoising 6.953470,
forward transparency 1.637535 and whole frame 34.12580. Requested/committed
VRAM is 9,665 / 9,679 MiB with no demotion, 57 MiB higher than the compact
visibility capture. The transient pool's assigned and available byte counts
each increase by eight bytes, while within-frame reuse drops from eleven to
ten allocations: the removed full-size sky guide had already shared storage
with a later resource. The experiment did not establish a memory or frame-time
improvement and is rejected. The measured forward/VRAM increase is recorded
without claiming its cause was isolated.

### Primary launch-tiling probe

`build/checks/primary-tiles-8x8` privately remaps the primary ReSTIR/no-guide
raygen's launch coordinates into 8x8 tiles. It keeps the samples and hit
shaders unchanged, and is restricted to the tile-aligned 1920x1080 GI
extent; it is not a general implementation or an odd-extent qualification.
The base, NV and EXT raygen modules are restored and their hashes checked
when the probe ends. The canonical ZenApp executable is not copied or changed
by the probe (SHA256
`a0898db4f581f3af8fe7515ba915bf6f6f6ec22f823848775d1cca14250a28c1`).

Warm trace time increases to 10.323150 ms and whole frame to 36.85480 ms,
versus 8.660865 / 34.10240 ms in the compact-visibility capture. Receiver
visibility and other unchanged passes also vary. The probe is rejected;
no tile remapping remains in production and no image-equivalence claim is
made for it.

### Diffuse closest-hit specialization probe

`build/render-hardening-phase4-diffuse-hit` adds a separate closest-hit group
for ReSTIR diffuse rays, omitting their already-disabled normal-map path.
Reflection rays retain the original hit shader, and the twelve-scalar payload
is unchanged. `build/checks/diffuse-hit-native` passes 116 ray cases (four
fixture skips, twelve disabled), 776 core cases (nine disabled) and all 52
denoiser cases, with clean validation and no reported leaks.

The warm capture measures primary trace 8.607075 ms, receiver visibility
4.659840, forward transparency 1.637085, eight-pass denoising 6.875920 and
whole frame 33.87240. Requested/committed VRAM is 9,604 / 9,618 MiB with
no demotion. The approximately 0.05 ms primary difference from compact
visibility is small relative to the variation observed between recent
captures, and unchanged stages also vary. This run does not establish a
useful gain from the extra group. The specialization is reverted; its sources
are preserved under the capture's `candidate` directory, with executable
SHA256 `9421c59371238095b810af8fa960e44aeb85fd13335b06078b29741745abc824`
recorded in provenance. No executable copy was created.

### Rejected packed-normal payload probe

`build/render-hardening-phase4-packed-normal` stores the hit normal in the
same 24-bit octahedral encoding as a persistent ReSTIR reservoir. Closest-hit
lighting retains its full-precision normal; raygen decodes the returned normal.
This reduces the primary payload from twelve scalar values to ten. All 116
enabled ray cases, 776 core cases and 52 denoiser cases pass with clean
validation and no reported leaks (`build/checks/packed-normal-native`);
the usual four ray fixture skips and disabled cases remain unverified.

The warm capture measures primary trace 8.921120 ms, receiver visibility
4.661025, forward transparency 1.636945, denoising 6.954388 and whole frame
34.43705. Requested/committed VRAM is 9,677 / 9,691 MiB with no demotion.
This does not improve on the retained compact-visibility capture's
8.660865 ms trace and 34.10240 ms frame. The four source files are restored
byte for byte and the canonical executable rebuilt in place. Candidate
sources remain under the capture's `candidate` directory. No 4K image
equivalence claim is made for this rejected candidate.

### Rejected sixteen-frame repair window

`build/checks/history-repair-window-16-compact` extends the current wide
history repair from eight to sixteen frames and advances its mip selection
every four frames instead of every two. The fast-pan pose-32 check measures
0.29645303% of pixels above 4/255 and RMS 0.63896720, versus the retained
eight-frame repair's 0.28372155% and 0.63757545. The longer window is rejected;
no production source changed. The helper restores the production shader and
verifies its SHA256
`e5880fa859ef857d64a5fbe231669c0f333e13cbb99fb04bdc36a9678d26c25f`.
The canonical executable remains unchanged throughout this probe (SHA256
`162696501d73aca71d6bd9d1acb10d4b9c16c7c103d2c9c4c677c0d616e341d7`).

A spatial count of the retained eight-frame pose-32 errors is recorded in
`build/checks/wide-repair-production-pan/error-blocks.json`: 10,159 of the
23,533 threshold-exceeding pixels lie in the bottom-left 240x240 block.
This locates a major remaining error region; it does not establish its cause.
