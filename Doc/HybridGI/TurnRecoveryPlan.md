# Turn Recovery Plan: world-space reuse for new screen probes

Goal: after a fast 180° turn at 4K, the image is clean from the first frame and settles within one second.
The plan takes two ideas from Wenyou Wang's thesis (*Light Transport Beyond Connected Paths: From Path Segments
to Latent Probes*, University of Waterloo, December 2026):

- **Keep transport in world space.** Probes in world space keep their lighting when the view changes (the
  thesis's latent probes).
- **Reuse path segments, resampling before debiasing.** A new probe reuses segments already traced
  elsewhere, and only the segments it selects pay for a visibility test.

## Status (2026-11-28): paused, accepted by eye

**In the tree:**

- **Faster fade.** The diffuse filter's luminance edge stopping is full at 32 sample frames instead of 128.
  The tail falls 20–57% at frames 32–64; the first visit settles in about 2.5 s instead of 5, the return in
  about 1.3 s instead of 2.5.
- **Energy-preserving young-probe limit** (`ScreenProbeSpread`). Newly exposed surfaces no longer start dark:
  the mean error at frame 8 goes from −0.14 to −0.03 levels.
- **Seed-counter stall fix.** The world-cache seed used to copy its counters every frame and stall the CPU.
- **The turn test,** `tools/gi_turn_noise.py`, which also splits flicker from stable error.

**Where the turn stands:**

- **Frames 1–3 are still 3–7.5% noisy,** but almost none of it flickers (0.13–0.20% of pixels). The error
  is soft and stable, and drops under 1% by about 0.6 s.
- **The first two targets are not met.** No lever tried at a reasonable cost gets frames 1–3 under 1%, or the
  first visit settled within one second.
- **Tried and reverted:** a stronger world-cache prior (T2), more young rays (T3a), and current-frame segment
  reuse (T3).
- **Not built:** world-space probe memory, the learned transport, and keeping the cache current behind the
  camera.
- **Next:** the upscaler phase ([UpscalerPlan.md](../UpscalerPlan.md)).

## Target (2026-11-27)

The test is `tools/gi_turn_noise.py`: one ZenApp launch on New Sponza, 3840 × 2160 fullscreen, HW RT, about
3 minutes.

- **Timeline.** After the start-up warm-up, the camera settles at pose A for 300 frames. It then makes three
  180° turns, each over 6 frames:
  - A to B, the first visit;
  - B back to A, the return;
  - A to B again, the revisit.

  Each landing is held for 1,024 frames.
- **Noisy pixels.** A pixel is *noisy* when it is more than 10% and more than 2 display levels away from the
  settled image of the same pose. The settled image is the mean of the last 8 frames of the hold.

| Gate | Requirement, on every leg |
|---|---|
| **First frames** | Frames 1, 2 and 3 after landing are each under **1%** noisy |
| **Settle time** | Settled within **1.0 s** at the measured frame time. Settled means at most 0.1% noisy from that checkpoint on. |
| Guard: specks | Pixels more than 24 levels off stay ≤ 0.1% |
| Guard: still camera | The settled floor (8 held frames against the reference) stays 0.000% noisy, RMS ≤ 0.04 |
| Guard: cost | Median frame time during the holds rises at most 1 ms over the baseline (21.0 ms) |

The tool prints PASS or FAIL for the first two gates. Checkpoints 40, 44 and 48 fall around one second
(0.85, 0.94 and 1.02 s at 21.3 ms per frame).

Slow pans and the separate still-camera run are no longer per-change tests. The old three-case run
(`tools/gi_noise_fps.py`) is optional, before committing changes to temporal or spatial filters, because it is
the only check for blur or lag during a slow pan.

## Baseline

Main at f9a97f57, run in `build/turn-noise-tool/main-d69f51af`:

| Leg | Frames 1 / 2 / 3, noisy % | Frame 32 | Frame 64 | Settled at |
|---|---|---|---|---|
| First visit | 7.56 / 4.83 / 3.63 | 0.89 | 0.55 | frame 256, 5.5 s |
| Return | 3.20 / 2.15 / 1.71 | 0.36 | 0.22 | frame 128, 2.6 s |
| Revisit | 7.29 / 4.61 / 3.47 | 0.75 | 0.43 | frame 128, 2.7 s |

- **Specks and floor.** Pixels more than 24 levels off stay at 0.008% or less. The still-camera floor is
  0.000% noisy with RMS 0.033–0.035. The noise is faint and widespread: about 3% of pixels are more than
  4 levels off at frame 1.
- **First frames come from new probes, not the cache.** The revisit is almost as noisy as the first visit,
  although the cache behind the camera is warm by then.
- **A cold cache doubles the tail.** The first visit settles at 256 frames, the revisit at 128.
- **Size of the gap.** The targets need about 7.6× fewer noisy pixels at frame 1, and settling three to five
  times sooner.

## How a new probe gets its lighting today

- **Rays.** Each probe traces 16 rays per batch, up to 3 batches (48 rays). They are drawn from a mixture of
  uniform, lamp, learned, environment and 64-bin CDF lanes. The learned and CDF lanes need the probe's own
  history, so a new probe has none of them.
- **World-cache prior, off by default.** `worldCacheSeed` defaults to false (`VoxelGIRenderer.h`), and the
  baseline setup does not set it. So neither of the two priors below runs in the baseline.
  - **Probe seed** (`screen_probe_prior.glsl`). A fresh probe, or a relocated one without history support,
    reads its cell's indirect seed record (`WorldIndirect`) once that record has 4 updates. The prior
    counts as at most one batch: at 3 batches it gets about a quarter of the weight.
    - It is uniform over the hemisphere (L0 plus an L1 term along the normal), so it carries no direction.
    - It seeds the diffuse SH only.
    - With the seed on, each capture's JSON reports `newProbeCache`: age-zero probes, exact warm seeds and
      warm parent seeds.
  - **Pixel-history seed** (`gi_denoise_temporal.glsl:435`). A pixel without history takes `WorldIndirect`
    once the cell has 32 updates.
  - **Earlier result.** With the seed on, young-probe RMS dropped only 3%
    ([GIDenoiserFinalFix.md](GIDenoiserFinalFix.md), stage 2).
- **Reflections.** A fresh probe's radiance atlas has no seed. Missing weight is filled with this frame's
  mean radiance (`screen_probe_prefilter.glsl`).
- **Cache updates.** Every update path starts at an on-screen G-buffer pixel (`world_cache_start.glsl`).
  Surfaces behind the camera are refreshed only when a secondary bounce happens to reach them. The
  start-up warm-up looks only toward the first camera direction.
- **Young window.** Probe youth is `1 − smoothstep(32, 192, age)`. It widens the diffuse and specular
  spatial filters from 3 × 3 to 7 × 7 probes. By construction the filtered image keeps changing for
  192 frames (about 4 s), which conflicts with a one-second settle.
- **Not reusable as is.** Per-pixel ReSTIR GI ([ReSTIRGIImplementation.md](ReSTIRGIImplementation.md)) is
  off by default and keeps its reservoirs in screen space, so a turn loses them. The R3 experiment in
  [ProbeRadianceAtlasPlan.md](ProbeRadianceAtlasPlan.md) adopted predecessors' screen-space history and
  gained nothing for the same reason.

## From the thesis to this engine

| Thesis idea | What it means here | Stage |
|---|---|---|
| Latent probes on a world-space grid. Transport does not depend on the view and reacts to lighting without temporal accumulation. | The world cache must stay converged around the camera in every direction. It should seed a new probe with a weight that matches how reliable the cache is, not a fixed quarter. | T1, T2 |
| Path segments as reusable units, combined with marginal MIS | A ray segment (receiver → hit point, with the radiance leaving the hit point) is stored in world space. Any nearby probe can reuse it after the camera has turned away. | T3 |
| Resample before debiasing | A new probe resamples many stored segments by their unshadowed contribution. It traces visibility only for the few it selects, and combines them with its own rays. | T3 |
| Amortised transport replaces accumulation | The filters' youth follows effective sample count, not frame age, so a well-seeded probe stops looking young at once. | T4 |

The shipped real-time counterpart of T3 is world-space ReSTIR (Boissé 2021): reservoirs kept in a GPU hash
grid, so reuse survives disocclusion. TransportFormer itself, a cross-scene learned model, is not part of this
plan.

## Stages

Every stage ends with one `gi_turn_noise.py` run and a 30-frame validation launch. T1 and T3 also end with a
`--ball` run, because stale cells and stored segments must respond to the moving light.

### T0. Find out where the frame-1 noise comes from (no rendering change)

A per-term capture (the ground-truth batch's LightingCapture) writes 10 RGBA32F terms per pixel, about
1.3 GB per 4K frame. That is too heavy for the roughly 24 frames a turn test scores. T0 therefore removes one
component at a time and runs the same turn test, one launch each:

| Run | Setup override | What remains |
|---|---|---|
| Reflections off | `rendering.gi.cone.reflections=1` | diffuse GI, direct light, shadows |
| Diffuse off | `rendering.gi.cone.indirectDiffuse=false` | reflections, direct light, shadows |
| GI off | both | direct light, shadows, emission, sky |
| Seed on | `rendering.gi.cone.worldCacheSeed=true` | everything, with today's prior and its counters |

- **Caveat.** The noisy threshold is relative to each run's own settled image. Removing a component
  therefore makes the threshold slightly tighter for what remains. The ablation ranks the sources rather than
  splitting them exactly.
- **Not worth splitting by probe age:** after a 180° turn every visible probe is new.
- **Exit:**
  - The share of frame-1 noise from diffuse GI, from reflections, and from everything else.
  - The warm-seed rate on the first visit and the revisit, and the existing seed's effect.
  - If diffuse GI is not the largest share, T2 and T3 start with the term that is.

**T0 results (2026-11-28, `build/turn-noise-tool/t0`).** Noisy % on frames 1 / 2 / 3 after landing. RTSS was
running; the launcher disables its Vulkan layer, and today's seed-on run (in effect a baseline, see below)
repeats yesterday's baseline within 1%.

| Run | First visit | Return | Revisit | Targets |
|---|---|---|---|---|
| Baseline (2026-11-27) | 7.56 / 4.83 / 3.63 | 3.20 / 2.15 / 1.71 | 7.29 / 4.61 / 3.47 | fail |
| Reflections off | 6.89 / 4.45 / 3.39 | 2.91 / 2.02 / 1.62 | 6.51 / 4.09 / 3.11 | fail |
| Diffuse off | 0.023 / 0.014 / 0.009 | 0.011 / 0.009 / 0.008 | 0.028 / 0.018 / 0.014 | pass |
| GI off | 0.010 / 0.003 / 0.001 | 0.001 / 0.001 / 0.000 | 0.009 / 0.003 / 0.001 | pass |
| Seed on (not applied) | 7.56 / 4.95 / 3.74 | 3.23 / 2.15 / 1.72 | 7.25 / 4.62 / 3.45 | fail |

- **Diffuse GI is the noise.**
  - Without diffuse GI, the turn passes both targets with reflections still on (0.028% worst, settled at
    frame 1).
  - Without reflections, 90% of the baseline noise remains.
  - Direct light, shadows and sky are clean (GI off: 0.010% at most).
  - T2 and T3 therefore target the diffuse probe lighting and the pixel history first; the radiance atlas
    can wait.
- **The seed run did not enable the seed.** With `gi.optimal: true`, `ApplyOptimalDefaults`
  (`VoxelGISettings.cpp`) rebuilds the settings from the optimal preset, which resets `worldCacheSeed` to
  false. The frame summaries confirm it: `newProbeCache` is 0 on every frame. Content switches such as
  `reflections` and `indirectDiffuse` survive the preset, so the other runs are valid.
  - Setting `rendering.gi.optimal=false` is no way around it: it resolves to per-pixel ReSTIR at
    half-resolution GI, because the setup's cone fields are not the optimal ones.
  - The ZenApp override `--gi-world-cache-seed=true` keeps the optimal configuration (screen probes, RT
    pipeline, full resolution, spatiotemporal denoiser, ray-traced shadows with reuse) and adds the seed. The
    tool passes it with `--zenapp-arg`. A settings test requires the seed to mark a configuration custom, so
    T2 should not simply fold it into the preset.

**Seed on (2026-11-28, `--zenapp-arg=--gi-world-cache-seed=true`, RTSS closed):**

| Leg | Frames 1 / 2 / 3, noisy % | Against baseline, frame 1 | Settled at | New probes at landing, warm seed |
|---|---|---|---|---|
| First visit | 7.77 / 5.06 / 3.80 | +3% | frame 256 | 61,442, 99.4% |
| Return | 2.82 / 1.95 / 1.59 | −12% | frame 128 | 61,568, 99.7% |
| Revisit | 6.91 / 4.33 / 3.22 | −5% | frame 128 | 61,442, 99.5% |

- **The cache already covers the reverse view.** 99.4% of new probes find a warm seed even on the first
  visit, so world-space information is there at the new probes. The existing prior uses it too weakly: about
  a quarter of one frame's weight, with no direction. Settling is unchanged.
- **The seed path tripled the frame time:** 65.7 ms against 20.8 ms over the holds. The resolved settings
  differ only in `worldCacheSeed`.
- **Cause and fix (2026-11-28).** A per-pass profile of 400 static 4K frames (`build/turn-noise-tool/seed-profile`)
  found the cost on the CPU, not the GPU:

  | Run | Summed pass GPU | CPU frame (median) |
  |---|---|---|
  | Seed off | 17.46 ms | 21.09 ms |
  | Seed on | 16.92 ms | 38.76 ms |
  | Seed on, after the fix | 17.26 ms | 21.82 ms |

  - Every frame copied the seed counters into one CPU-readable buffer (`ReadProbeSeedCounts`). So each frame
    waited for the previous one, and the CPU and GPU stopped overlapping.
  - Now only frames whose summary reads the counters copy them: captured frames, ground-truth term frames and
    the profile's last frame. ZenApp requests the copy (`HybridGIRenderer::RequestProbeSeedCounts`), and
    `ReadSeedCounts` returns zeros unless that frame's copy was submitted.
  - The seed now costs 0.7 ms per frame, almost all of it the `ScreenProbePrior` pass (0.6 ms GPU).
  - Changed files: `WorldRadianceCache.h/.cpp`, `HybridGIRenderer.h`, `ScreenProbeGI.h/.cpp` (`Record` takes
    the cache by non-const reference) and `ZenApp/RuntimeApplication.cpp`.

### T1. Keep the world cache current in every direction

- **Warm-up in all directions.** The 128 hidden start-up frames turn through four yaw directions,
  32 frames each, instead of staying at the first camera.
- **Off-screen refresh.**
  - A fixed share of the cache's path budget (start at 1/8 of the tiles) starts from existing cells instead
    of on-screen pixels.
  - Cells are picked least recently updated first, within the current cache level's reach of the camera.
  - Each cell keeps the surface point and normal of its last receiver sample (16 bytes per cell, 16 MB at
    1,048,576 cells), so a refresh path starts on real geometry.
  - Stale cells (failing `WorldHistoryUsable` after a light edit) come first.
- **Exit:**
  - The first visit settles no later than the revisit (128 frames today).
  - Frames 1–3 of the first visit match the revisit within launch variance.
  - With `--ball`, the first visit is no worse than without it.
  - Cost at most +0.5 ms.

### T2. A world-space prior weighted by its reliability

- **Turn the seed on.** Enable `worldCacheSeed` in the turn configuration, starting from T0's seed-on run.
- **Weight by variance, not a fixed share.** Weight the seed by inverse variance: the cache record's
  variance and update count against the probe's current-frame variance (`probeLighting` moments). A
  well-converged cell then outweighs 48 rays on frame 1.
  - The previous attempt (ProbeRadianceAtlasPlan, "effective-sample prior", reverted) weighted by age, not
    by measured variance.
- **Directional prior.** Build the L1 SH from the cell's six directional irradiance records instead of a
  uniform hemisphere.
- **One threshold.** Use the same warm threshold for the probe seed (4 updates today) and the pixel-history
  seed (32 today).
- **Fade.** The prior fades as the probe's own estimate accumulates. It never enters the settled value
  once the probe is mature.
- **Exit:** revisit frames 1–3 under 2% noisy. The still-camera floor is unchanged.

**T2 result (2026-11-28, `build/turn-noise-tool/t2`): rejected.** The seed's weight followed its cell's sample
count, `updates / 16` batches, capped at 4 or 12 batches (64 or 192 ray-equivalents against 48 own rays). Noisy
% at frame 1:

| Seed weight | First visit | Return | Revisit | Bright pixels at frame 1 |
|---|---|---|---|---|
| Off (baseline) | 7.56 | 3.20 | 7.29 | 3 / 38 / 8 |
| Existing, at most 1 batch | 7.77 | 2.82 | 6.91 | 19 / 8 / 7 |
| Cap 4 batches | 10.72 | 3.37 | 8.23 | 867 / 376 / 383 |
| Cap 12 batches | 11.11 | 3.95 | 9.62 | 88 / 38 / 254 |

- **More seed weight means more error.** The seed is the cell's indirect irradiance: a spatial and
  normal-bin average, spread uniformly over the hemisphere. The test compares against what each probe settles
  to, so the cell's bias counts as noise, and a few bright cells leave specks.
- **The other measures did not move.** Settle times stayed at 256 / 128 / 128 frames and the still-camera
  floor at 0.000%. Frame time was 20.4 ms, so the stall fix holds in the turn test too, and the seed counters
  now read 61,442 fresh probes, 99.4% warm.
- **Reverted.** A directional seed from the six records would still carry the cell's spatial bias. The
  directional records also include the cell's direct light, which the probe's lighting excludes.
- **Consequence for the plan.** A biased world-space prior cannot meet the target, because the target
  measures error against each probe's own converged value. The extra information has to be unbiased at the
  probe's own position: more of its own rays, or reused segments that are reconnected to the probe and pass a
  visibility test (T3).

### T3a. Calibrate: how many samples a new probe needs

Before building the reuse, measure how frame-1 noise falls with a new probe's sample count.

- **Change.** Young probes get 3 batches today (`ZEN_SCREEN_PROBE_MAX_BATCHES`); mature ones get 2. Raise the
  young count to 6, then 12 (96 and 192 rays). The ray buffers are sized from that constant.
- **What it tells us.** How many effective samples T3 has to deliver to reach under 1% on frames 1–3. If a
  Gaussian error held, 7.5% to 1% would need about 2.1× the samples. The real distribution has a heavy tail.
- **Cost.** It is a calibration only: the trace cost on landing frames grows with the batch count. If 6 batches
  already passes the frames 1–3 gate, a capped first-frame boost may be a cheaper answer than T3; weigh it by
  its landing-frame time.

**T3a result (2026-11-28, `build/turn-noise-tool/t3a`).**

- **Batch counts changed to 5 and 9 (80 and 144 rays).** Later batches use a power-of-two Sobol interval
  (`batches − 1`), and these counts keep it. The cold-probe stratification in `screen_probe_trace.glsl` now
  works for any young batch count: its row step is 1 or `batches − 1`, which is coprime with the count. It
  behaves identically at 3 batches.
- **Results.** Turn ms is the median of the six uncaptured turn frames; hold ms is the median of the hold. The
  world cache kept 1,048,576 cells in every run.

| Young batches (rays) | First visit, frames 1 / 2 / 3 | Return, frame 1 | Revisit, frame 1 | Turn ms | Hold ms | Settled at |
|---|---|---|---|---|---|---|
| 3 (48), today | 7.46 / 4.79 / 3.55 | 3.18 | 7.32 | 27.5 / 33.7 / 33.8 | 20.0 / 19.2 / 19.9 | 256 / 128 / 128 |
| 5 (80) | 4.65 / 3.01 / 2.40 | 1.93 | 4.50 | 30.9 / 35.2 / 35.5 | 20.3 / 19.6 / 20.2 | 256 / 128 / 128 |
| 9 (144) | 4.14 / 2.93 / 2.33 | 2.38 | 4.15 | 36.5 / 44.4 / 44.6 | 20.6 / 19.9 / 20.6 | 128 / 44 / 128 |

- **Rays help, then stop helping.** From 3 to 5 batches, frame-1 noise falls 38%. From 5 to 9 it falls only
  11% more, and frame 3 stays near 2.3%. Something other than the ray count sets a floor near 4% at frame 1.
- **The floor is the diffuse filter's width** (`screen_probe_filter.glsl`):
  - The radius is `clamp(512 / √sampleAge, 8, 192)` pixels. `sampleAge` is accumulated batches in
    mature-probe frames, which is 1.5 at 3 batches and 4.5 at 9 on frame 1.
  - Both therefore start at the 192-pixel cap, about 24 probe tiles. Extra rays lower the variance but leave
    the blur in place, and the test counts that blur as error against the settled image.
  - The same decay sets the settle time. The radius is still 75 px at 47 frames (one second), 45 px at
    128 and 32 px at 256; only past 4,096 frames does it reach the 8-pixel floor.
- **Cost.** At today's 3 batches the turn frames already take 27.5–33.8 ms, against 20 ms in the hold.
  5 batches adds 1.5–3.4 ms to those frames and 9 batches adds 9–11 ms; hold frames are unchanged. 5 batches
  is a cheap partial gain.
- **Consequence for the plan.** T4 moves ahead of T3. The filter width has to follow each probe's measured
  variance rather than a fixed `512 / √N`. Only then do more samples, whether from rays or reused segments,
  narrow the blur, and only then can the image settle within a second. T3 remains the way to make those
  samples cheap.

### T3. Reuse path segments stored in world space

- **Store.** Each world-cache slot gets one segment reservoir, keyed like the cell (level, position and
  normal bin through `CacheSpatialKey`). It holds:
  - the hit position and the packed hit normal;
  - the radiance leaving the hit point (FP16);
  - the weight sum, candidate count M and the unbiased contribution weight W.

  That is 32 bytes per slot, 32 MB at 1,048,576 slots.
- **Producers.**
  - Every screen-probe ray offers its segment to the reservoir of its receiver's cell, by weighted
    reservoir sampling with target `p̂ = luminance(L_hit) · cos θ_receiver`.
  - So do T1's off-screen paths and the world-cache path vertices.
  - Conflicts use one stochastic replacement per slot per frame (a 64-bit atomic on a hashed key), or a
    collect-then-resolve pass if contention costs too much.
- **Consumers.** Probes with a low effective sample count (fresh, or relocated without support) gather
  candidates:
  - from their own cell and the eight cells around it in the tangent plane;
  - from the current-frame segments of the adjacent probes.

  Each candidate is reconnected to the probe: direction, receiver cosine, and the reconnection Jacobian
  `|cos θ_hit| / d²` ratio. The probe resamples M of them (start at 4) and traces visibility only for those.
  An occluded segment contributes zero.
- **Combine.** Weight the reused segments against the probe's own 48 rays with the generalised balance
  heuristic over the two techniques (marginal MIS). Each reused segment adds to the diffuse SH (cosine) and to
  the radiance-atlas cell its direction falls in.
- **Invalidation.** A segment is dropped when its cell fails `WorldHistoryUsable`, or when the light edit log
  marks its hit region. This covers moving lights and geometry changes.
- **Cost.** After a 180° turn nearly every probe is new: 162,000 × 4 visibility rays is about 0.65 M rays,
  against about 7.8 M probe rays per frame today.
- **Exit:**
  - Frames 1–3 under 1% noisy on every leg, which is the first-frames gate.
  - The `--ball` run shows no stale reflections or lag beyond the baseline.
  - Cost at most +1.0 ms in total with T1.

### T4 calibration (2026-11-28, `build/turn-noise-tool/t4`)

Each run changes one thing at 48 rays. The tool now also reports how many noisy pixels are darker than
settled and the mean signed error.

| Run | First visit, frames 1 / 3 / 8 / 32 | Return, frame 1 | Revisit, frames 1 / 8 / 32 | Revisit mean error at frame 8 | Settled at |
|---|---|---|---|---|---|
| Baseline | 7.66 / 3.79 / 2.24 / 0.99 | 3.18 | 7.29 / 2.02 / 0.75 | −0.16 levels | 256 / 128 / 128 |
| Radius constant 128 (frame-1 radius 104 px) | 7.10 / 3.40 / – / 0.83 | 3.13 | 7.05 / – / 0.63 | – | 256 / 128 / 128 |
| Radius constant 64 (52 px) | 7.57 / 3.69 / – / 0.83 | 3.28 | 7.48 / – / 0.60 | – | 256 / 64 / 128 |
| Young clamps off (outlier scale, isolated ceiling) | 7.49 / 3.61 / 1.92 / 0.85 | 3.15 | 7.43 / 1.74 / 0.64 | −0.01 levels | 256 / 128 / 128 |
| Cache cells kept (`--gi-world-cache-unused-frames=4096`) | 7.47 / 3.66 / 2.18 / 0.95 | 3.14 | 7.21 / 1.58 / 0.54 | −0.03 levels | 256 / 128 / 128 |

- **The filter radius is not the floor.** Shrinking the frame-1 radius from 192 to 52 px leaves frames 1–3
  unchanged; it helps only frames 32–64. The T3a explanation (blur at the 192-px cap) is therefore wrong.
- **The young clamps cause a dark bias, not the noise.** Without them the mean error at frame 8 goes from
  −0.16 to −0.01 levels, but the noisy count barely moves. Bright specks return (182 and 207 at frame 1 on the
  return and revisit, against 51 and 9), which is why the clamps exist.
- **A retired cache causes the rest of the dark bias and part of the tail.**
  - `worldCacheUnusedFrames` defaults to 240, so in each 1,024-frame hold the cells for the view behind the
    camera are retired. Every visit therefore lands on a cold cache, which is why the revisit looked like the
    first visit. The seed's "warm" test (at least 4 updates) was too weak to show this.
  - Keeping the cells removes the revisit's dark bias and cuts frames 8–32 by about a quarter. Frames 1–3 do
    not change.
  - Keeping them costs no steady-state memory: every run settles at 1,048,576 cells (`worldCacheCapacity` in
    the profile summary). The 4,194,304 cells in ZenApp's log is the first allocation, before the cache
    sizes itself to the scene.
- **Frame-1 noise responds only to the young probes' estimate,** and only partly to more of the same rays:
  48 → 80 → 144 rays gives 7.5 → 4.7 → 4.1%.
  - In the landing error map it is soft blotches across all surfaces, tens of pixels wide, plus brighter error
    along carved edges.
  - Adding rays drawn from the same distribution saturates, which points to a heavy-tailed estimate rather
    than plain variance. A new probe has no learned or CDF guide lanes, so it seldom draws the rare bright
    paths. Its estimate then swings between low and occasionally high.
  - Resampling candidates by their contribution, which T3 does, targets exactly those rare paths, whereas
    more rays do not. T3a therefore does not rule T3 out.
- **Settling** stays at 128–256 frames across every run except two. That matches the 128-update cache window
  and the young windows (age 32–192) together, rather than any single one.
- **Frame-time caveat.** The clamps-off run measured 64–68 ms hold frames against the usual 20 ms, probably
  from another GPU load. Its noise numbers count frames, so they are valid; its seconds are not.

### T3 first version (2026-11-28, `build/turn-noise-tool/t3`)

**Built, measured and reverted.** None of this code is in the tree: the gain was too small for its cost, and
the measurements below are its only record. The first version reused current-frame samples between screen
probes:

- **Hit records** grow from 6 to 8 words. Shade-hits also stores each surface hit's distance and its normal
  facing the probe (`screen_probe_hits.glsl`, `ZEN_SCREEN_PROBE_HIT_WORDS`).
- **New pass `ScreenProbeReuseHits`** (`screen_probe_reuse_hits{32,64}.comp`), between shade-hits and
  Accumulate. In probes no older than 4 frames, each of the 16 lanes resamples the probe's coplanar
  neighbourhood:
  - It streams the samples of its own ray index across all batches and keeps two reservoirs: one sky
    direction, reused as the same direction, and one surface hit, reconnected with its Jacobian. Jacobians
    outside [0.1, 10] are rejected.
  - Weights are the unshadowed contribution at this probe over the density the source probe drew the
    sample with, under uniform `1/M` MIS. Only coplanar neighbours count, so their hemispheres cover this
    probe's.
  - Each kept sample traces one visibility ray (`HardwareOccluded`) from this probe.
- **Accumulate blends** each signal's lane moments as `(1 − s) · own + s · f · W`.
- Validation launches are clean.

**A source split came first.** With the reuse pass on, frame-1 noise for each light source:

| Run | First visit | Return | Revisit | Noisy pixels darker than settled |
|---|---|---|---|---|
| Everything on (bounce reuse only) | 7.15 | 3.09 | 7.02 | about 45% |
| Environment (sky) light off in GI | 2.90 | 2.56 | 2.62 | 86% |
| Analytic (lamp and sun) light off in GI | 6.18 | 2.09 | 6.13 | 33% |

- **Sky light through openings carries about 60% of the first-visit and revisit frame-1 noise.** What is
  left without it is mostly the dark bias from the young clamps and the retired cache.
- **This also explains the T4 radius result.** The diffuse filter deliberately keeps the sky footprint narrow
  (`skyFalloff`), so that sky visibility is not blurred across edges. Sky noise therefore reaches the screen
  almost unfiltered.

**Results** (noisy %, frame 1 and frames 2 / 3 of the first visit):

| Variant | First visit | Return | Revisit | First visit, frames 2 / 3 | Turn ms |
|---|---|---|---|---|---|
| Baseline (T3a, 3 batches) | 7.46 | 3.18 | 7.32 | 4.79 / 3.55 | 27.5–33.8 |
| Bounce only, share 0.5, 3 × 3 | 7.15 | 3.09 | 7.02 | 4.64 / 3.50 | 34.2–36.4 |
| Sky and bounce, share 0.5, 3 × 3 | 6.77 | 2.97 | 6.62 | 4.42 / 3.35 | 34.8–35.1 |
| Sky and bounce, share 0.75, 5 × 5 | 6.11 | 2.88 | 6.20 | 3.96 / 3.02 | 28.9–36.0 |

- **Gains are real but small:** up to −18% at frames 1–3, for 1–3 ms on turn frames. Settling is
  unchanged at 128–256 frames. The still-camera floor stays at 0.000%.
- **Why so small.**
  - Every candidate is a sample traced this frame by a probe in the neighbourhood. Resampling picks the bright
    ones well, but cannot add information that no nearby probe traced.
  - The diffuse filter already averages bounce over a wider area.
  - For sky, reuse with per-probe visibility is the right tool. It pools the sky directions that neighbours
    found while keeping this probe's own visibility, which the filter cannot do. But a 3 × 3 to 5 × 5
    neighbourhood holds only 27–75 candidates per lane.
- **What the frame-1 noise leaves.** About 45% of the remaining noisy pixels are darker than settled. That is
  the dark bias from the young clamps and the retired cache, and reuse does not address it.
- **Reaching under 1% in frames 1–3** of a view never seen before needs information from outside the
  current frame's neighbourhood, a fix for the dark bias, or both. Shipped renderers (Lumen with TSR) show
  disocclusion noise for several frames after a fast turn too.

### Noise or blur? (2026-11-28, `build/turn-noise-tool/split/main`, main's image)

The turn tool now splits the error of the three consecutive landing frames into two parts:

- **Flicker:** pixels whose error changes by more than the noisy threshold from one frame to the next. That
  is visible noise.
- **Stable error:** the error that survives averaging the three frames. That is blur, bias, or error held
  in place by temporal accumulation.

| Leg | Noisy, frame 1 | Flicker, frames 1–3 | Stable error | Of which darker | RMS stable / changing |
|---|---|---|---|---|---|
| First visit | 7.51% | 0.20% | 5.62% | 2.91% | 1.15 / 0.27 levels |
| Return | 3.19% | 0.13% | 2.53% | 1.48% | 0.82 / 0.21 levels |
| Revisit | 7.29% | 0.19% | 5.39% | 2.71% | 1.12 / 0.27 levels |

- **There is almost no frame-to-frame noise after a turn.** Under 0.2% of pixels flicker beyond the
  threshold.
- **The error is stable over the first frames and fades over seconds.** It is about four times the changing
  part.
  - About half of it is darker than settled: the bias from the young clamps and the retired cache.
  - The rest is soft, low-frequency mottling: the first frames' sampling error, spread by the spatial filter
    and held by temporal accumulation while it converges.
  - It is not loss of detail. Narrowing the filter radius by four times did not change it (T4 calibration).
- **The still camera is clean** (floor 0.000%). Slow pans have not been split yet.
- **Consequence.** What reads as "noise converging over several seconds" is a stable, slowly fading error:
  half dark bias, half soft mottling. Sampling-side fixes (rays, reuse) shrink only the initial error. The
  dark bias and the length of the history decide how long it lasts.

### Dark-bias and fade fixes (2026-11-28, `build/turn-noise-tool/fix`, main's image, reuse share 0)

Noisy % at frames 32 / 64 / 128 after landing, and the settle frame:

| Run | First visit | Return | Revisit | Settled at | Specks, frames 32–128 |
|---|---|---|---|---|---|
| Main | 0.88 / 0.53 / 0.150 | 0.35 / 0.21 / 0.027 | 0.76 / 0.44 / 0.079 | 256 / 128 / 128 | 2–4 |
| Cache history 32 (instead of 128) | 0.90 / 0.53 / 0.078 | 0.37 / 0.21 / 0.024 | 0.79 / 0.43 / 0.077 | 128 / 128 / 128 | 1–4 |
| Cache cells kept 1,024 frames (instead of 240) | 1.02 / 0.61 / 0.135 | 0.36 / 0.22 / 0.030 | 0.64 / 0.40 / 0.084 | 256 / 128 / 128 | 0–4 |
| **A: edge stopping full at sample-age 32 (instead of 128)** | **0.71 / 0.34 / 0.082** | **0.25 / 0.09 / 0.018** | **0.62 / 0.26 / 0.050** | **128 / 64 / 128** | **0–2** |
| B: A plus young clamps fading at ages 8–48 (instead of 32–192) | 0.84 / 0.35 / 0.055 | 0.27 / 0.09 / 0.016 | 0.65 / 0.22 / 0.028 | 128 / 64 / 128 | 31–44 |
| A + cache history 32 + retention 1,024 | 0.80 / 0.37 / 0.066 | 0.25 / 0.09 / 0.018 | 0.66 / 0.30 / 0.061 | 128 / 64 / 128 | 0–3 |
| Same, with a looser outlier limit (4× instead of 2×) | 0.79 / 0.37 / 0.058 | 0.25 / 0.09 / 0.018 | 0.58 / 0.26 / 0.049 | 128 / 64 / 128 | 1–5 |
| A + young clamps released by measured uncertainty (5–15%) | 0.84 / 0.41 / 0.066 | 0.26 / 0.09 / 0.018 | 0.65 / 0.25 / 0.038 | 128 / 64 / 128 | 0–3 |

- **Kept: A.** The diffuse filter's luminance edge stopping now reaches full strength at 32 sample frames
  instead of 128 (`screen_probe_filter.glsl`). Before that, newly exposed probes blurred across lighting edges
  for more than two seconds.
  - The tail falls 20–57% at frames 32–64, and the first visit settles at 128 frames instead of 256, the
    return at 64 instead of 128.
  - The still-camera floor stays at 0.000% with RMS 0.033, and specks do not rise.
- **The fade is faster but not yet within one second.** At 64 frames the first visit and revisit still show
  0.26–0.34% noisy pixels.
- **The cache settings add nothing measurable on top of A,** so the defaults stay. Keeping cells longer costs
  no steady-state memory (see the T4 calibration).
- **The dark bias is not fixed.** The young clamps cause it: with them off it disappears, but bright specks
  return. Releasing them earlier by age (B) or by measured uncertainty brings the specks back or changes
  nothing, and a looser limit barely helps. A fix has to keep the clipped energy, for example by spreading it
  over the neighbouring probes, instead of discarding it.
- **Frames 1–3 are unchanged in every run** (7.1–7.5%, flicker 0.13–0.20%).

### Energy-preserving young-probe limit (2026-11-28, `build/turn-noise-tool/clamp`, with A, reuse share 0)

**Built.**

- **Only the vertical filter pass limits young estimates.** The horizontal pass's limit is off: limiting both
  passes also lost the light twice.
- **The vertical pass records the clipped light** (`screen_probe_filter.glsl`). That is the output
  coefficients at full scale minus the limited ones. It also records the total weight with which the coplanar
  base probes within 2 tiles will receive it.
- **New pass `ScreenProbeSpread`** (`screen_probe_spread.comp`). Each base probe adds its share,
  `clip_j · w_ij / W_j`, of every neighbour's clipped light. Both sides use one weight,
  `ProbeSpreadWeight` in `screen_probe.glsl`: a Gaussian over the tile offset times the plane weight on the
  same layer. Every clipped amount is therefore handed out exactly once.
- **A probe whose receivers weigh less than 1.5 keeps the old, lossy limit.** That is about three adjacent
  coplanar probes; with fewer, each receiver took a large share of a spike and became a speck itself.
- Validation launches are clean.

| Frame 8 after landing, unless noted | A (old limit) | Vertical limit only | Spread to all receivers | Spread, receivers ≥ 1.5 (kept) |
|---|---|---|---|---|
| Mean signed error, first visit / revisit (levels) | −0.143 / −0.148 | −0.072 / −0.065 | −0.011 / −0.009 | −0.036 / −0.032 |
| Noisy %, first / return / revisit | 1.99 / 0.94 / 1.93 | 1.94 / 0.93 / 1.83 | 1.90 / 0.94 / 1.76 | 1.99 / 0.91 / 1.84 |
| Noisy % at frame 64 | 0.34 / 0.09 / 0.26 | 0.35 / 0.08 / 0.27 | 0.37 / 0.09 / 0.24 | 0.40 / 0.09 / 0.26 |
| Bright specks at frame 1 | 41 / 40 / 8 | 21 / 111 / 17 | 44 / 202 / 181 | 42 / 77 / 17 |
| Settled at | 128 / 64 / 128 | 128 / 64 / 128 | 128 / 64 / 128 | 128 / 64 / 128 |

- **The dark bias is largely gone.** The mean error at frame 8 falls from −0.14 to −0.03 levels, and newly
  exposed surfaces no longer start dark and brighten over seconds. Frames 1–3 now run slightly bright
  (+0.12 levels at frame 1).
- **The noisy count does not fall.** The errors become symmetric instead of dark, so as many pixels stay more
  than 10% off. Settling is unchanged.
- **Specks are close to A's.** The return leg rises from 40 to 77 bright pixels at frame 1, out of
  8.3 million.
- **Hold frames are 0.5–1 ms slower than A** (20.6–21.4 ms against 19.7–20.4 ms). That is within run-to-run
  spread, but plausible for the extra pass.

### T4. Settle by evidence, not by age

- **Youth from sample count.** Drive probe youth (the 32–192 frame window) and the pixel-history youth from
  the effective sample count: own rays, plus the prior's weight, plus the reused segments' effective sample
  count. A well-seeded probe gets the 3 × 3 filter at once and stops changing.
- **Exit:** settled within 1.0 s on every leg, which is the settle gate. The still-camera floor and the specks
  guard are unchanged.

## Order and decisions

1. T0 is done: diffuse GI is the noise, and the cache is warm at 99.4% of new probes.
2. T2 is done and rejected: a biased world-space prior raises frame-1 error.
3. T3a is done. Rays help up to 5 batches, then the diffuse filter's fixed `512 / √N` radius sets the floor.
4. T4 comes next: a filter radius driven by variance, first calibrated by scaling the radius constant at 3
   and 5 batches.
5. T3, the segment reuse, follows once more samples actually narrow the filter.
6. T1 waits. A cold cache is not what limits the first frames: the cache is already warm at 99.4% of new
   probes. It only affects the first visit's tail.

## Risks

- **Bias from the cache prior** (T2): cell resolution, thin walls and missing contact shadows. It shows as
  early-frame error against the settled image, so the gate catches it. Keep the prior's weight variance-based
  and fading.
- **Stale or wrong reuse** (T3): moving lights and geometry. Invalidate through the existing edit log and
  `WorldHistoryUsable`, and check with `--ball`. Visibility tests on the selected segments stop light
  leaking through walls.
- **Contention and memory** (T1, T3): about 48 MB extra at 1,048,576 cells. The world cache sizes itself from
  free VRAM, so log its capacity before and after.
- **Launch variance.** Large-angle numbers vary up to 20% per launch. Repeat a run whose result decides a
  stage.
- **Unmeasured cases.** Slow pans and the light ball on a still camera are no longer per-change tests. Run
  the old three-case check before committing filter changes.

## Sources

- [PhD defence: Light Transport Beyond Connected Paths (Wenyou Wang, Waterloo)](https://cs.uwaterloo.ca/events/phd-defence-computer-graphics-light-transport-beyond-connected-paths-from-path-segments-to-latent-probes)
- [Segment-based Light Transport Simulation (Wang, West, Hachisuka, TOG 2025)](https://cs.uwaterloo.ca/~thachisu/seglt.pdf)
- [Marginal Multiple Importance Sampling (West, Georgiev, Hachisuka, SIGGRAPH Asia 2022)](https://cs.uwaterloo.ca/~thachisu/mmis.pdf)
- [World-Space Spatiotemporal Reservoir Reuse (Boissé, SIGGRAPH Asia 2021)](https://gpuopen.com/download/SA2021_WorldSpace_ReSTIR.pdf)
- [World-Space Spatiotemporal Path Resampling (Pacific Graphics 2023)](https://wangningbei.github.io/2023/ReSTIR_files/paper_ReSTIRGI.pdf)
- [GI-1.0: two-level radiance caching](https://ar5iv.labs.arxiv.org/html/2310.19855)
