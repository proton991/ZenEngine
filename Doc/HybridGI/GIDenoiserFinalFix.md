# GI denoiser final fix (2026-11-26)

This plan combines two code-only reviews of the uncommitted local-history implementation:
the session review of [GIDenoiserPlan.md](GIDenoiserPlan.md) and the independent audit in
[GIDenoiserCodeAudit.md](GIDenoiserCodeAudit.md). Both found that the new path never ran by
default, and that with it enabled the light ball's own bounce light was still estimated from a
few frames of 16 rays. The ten fixes are implemented, with measurements below.

## Fixes

| # | Problem | Audit item | Fix |
|---:|---|---|---|
| 1 | Local history response was off by default and had no editor control. Setting it in `engine.cfg` also switched the GI preset to custom. | 1 | `voxel_gi_history_response` defaults to `local` and is a content option, so the optimal preset keeps it. The GI panel has a checkbox for it under the probe denoiser. `global` remains available for comparison. |
| 2 | The light ball and movable lights were hashed into the settled light signature. Every camera move reset the settled stable-frame count and cache confidence, disabled parent seeding for new cache cells, and filled the 32-entry light-edit log. A settled edit was then evicted within 32 frames, which invalidated sparse cache cells. | 5 | Moving lights are excluded from the settled signature, policy and moved bounds. Their edits go to a separate 32-entry moving log, recorded every frame in local mode. Settled edits can no longer be evicted by moving lights. Probes refresh their light selection when a moving light moved. |
| 3 | The moving signal had a hard 16-frame limit, about 4 frames whenever the ball moved more than 4% per frame, and no history after an anchor moved. Its change value was unbounded when the probe saw no current moving light. | 2, 3 | Each frame weights the predicted history against the new observation by inverse variance, using per-lane sample variances of the observation (`V_O`) and of the predicted change (`V_D`): `age = 1 + 1 / (1 / frames + V_D / V_O)`. During steady motion this settles at `sqrt(V_O / V_D)` frames; when the movers stop, `V_D` is zero and history grows by one frame per frame, up to 256. Only the cache part of the moving light, which has no prediction, limits history through the drift rule, one frame late. Change is bounded to [0, 1]. Relocated probes reproject their moving history from the same footprint as the settled signals. |
| 4 | The moving signal reported zero noise at age 1. The spatial filter limited its radius to two tiles whenever the centre probe's current rays missed the lit patch. | 4 | The moving record stores its per-frame estimate variance, so the filter and gather see its real uncertainty. The two-tile limit applies only to a probe whose own history has been dark for 16 frames. |
| 5 | The world-cache moving record was limited to 16 frames even when every light was still. | 3 | The limit grows by one frame per frame since the last moving-light edit, up to the cache history. |
| 6 | Rough reflections took the maximum change over all rays, so one changed ray restarted all 112 texels. Moving light changes had no prediction. | 6 | Change is the energy-weighted mean over rays, with each ray counted at no less than the mean radiance. Moving-light hits add their predicted change to the texel history and report only their cache part as change. |
| 7 | The local glossy clamp compared slow history with neighbour fast history using only an absolute tolerance of 0.005. Bright pixels and isolated curved-surface pixels were clamped almost every frame. The traced change used the maximum of three rays. | 7 | The clamp tolerance is at least 20% of the pixel's fast-history luminance, the same relative floor as the global lag test. Glossy change is the mean over traced rays. |
| 8 | A changed ray counted as at least 1/16 of the hemisphere regardless of its weight, and a single low-density ray could saturate the probe's evidence. | 8 | Each changed ray counts by its estimator weight `cos / (pi * pdf)`, raised to its current energy share when brighter. One ray can claim at most a quarter of the hemisphere. |
| 9 | Light-edit evidence decayed with a 16-frame half-life, holding affected probes at 4 frames for about 64 frames. When the cap shortened history, older observations kept their full weight while new ones were scaled by a small confidence, freezing the probe. | 9 | Evidence decays with a two-frame half-life for every source; later cache solves report their own changes on the rays that see them. When the cap shortens history, the retained history is expressed in units of the current observation, so the new observation's weight is exactly `1 / age`. |
| 10 | Sky rotation added a 16-ray importance-sampled delta to mature history and clamped it at zero before blending, which biased energy upward. The predicted change was also counted again as evidence on the next frame. | 10 | On sky-delta frames the history and the observation are weighted by inverse variance, using the lane variance of the delta, the same rule as moving lights. The predicted change no longer counts as evidence. Probe and cache values are clamped after blending, not before. |

## Not in this fix

The ten fixes address lighting changes. [Pixel history for camera motion](#pixel-history-for-camera-motion)
is implemented as a follow-up. Sampling a mixture
of the old and new sky distributions during rotation is also left for later. Swept-bounds
surface edits still mark every ray through the union of an object's old and new bounds.

## First measurements (2026-11-26)

All 20 native GPU integration tests passed in four submission modes, including both unannounced
directional light off/on fixtures. [Native results](../../build/gi-denoiser-final-fix/native.xml)

`tools/native_gi_fast_motion.py` replayed a quick turn (7.5° per frame), walk and strafe at 3840 × 2160
on New Sponza with the light ball following the camera. It used the setup of the earlier 180° turn
replays, with 768 warmup frames and 384-frame settled holds. Each mode was scored against its own
settled image of every pose. The table gives each mode's mean over the frames scored during motion,
with display levels of 255. These are single launches, and native launches are not bit-identical.

| Motion | RMS, global → local | Pixels above 4 levels | Pixels above 16 levels | Isolated sparkle |
|---|---:|---:|---:|---:|
| Turn | 1.599 → 0.982 | 3.472% → 1.303% | 0.0314% → 0.0172% | 0.0027% → 0.0006% |
| Walk | 1.112 → 0.625 | 1.103% → 0.307% | 0.0120% → 0.0082% | 0.0014% → 0.0006% |
| Strafe | 1.324 → 0.726 | 2.014% → 0.606% | 0.0213% → 0.0124% | 0.0024% → 0.0007% |

In the first eight frames after stopping, RMS was 0.58–0.67× global and pixels above four levels
0.37–0.60×. Pixels above 16 levels were within ±5% of global, and local sparkle and blotch
counts were higher after the turn stopped (0.0006% vs 0.0002% sparkle). Those counts are a few
dozen pixels in a frame and within launch-to-launch variation. Remaining local error during the
turn is mostly on newly revealed surfaces, which stage 8 addresses.
[Local report](../../build/gi-denoiser-final-fix/ball-local/report.json) ·
[Global report](../../build/gi-denoiser-final-fix/ball-global/report.json) ·
[Error masks](../../build/gi-denoiser-final-fix/ball-error-masks.png)

The local launches took longer in wall time (reference hold 211 s vs 173 s for 6,528 frames). The local
launch ran first after the shader rebuild, so it may include cold driver compilation. No GPU trace has
been taken.

## Three-case check (2026-11-26)

`tools/gi_noise_fps.py` measures one scene in three ZenApp launches: New Sponza, 3840 × 2160 fullscreen,
hardware RT, async compute off, local response. Frame rate is the median wall time over 600 frames without
captures (unlocked clocks). Noise compares captured frames with a settled image of the same pose. The motion
is a ±30° yaw at 1° per frame.

| Case | FPS | Frame ms (p95) | Noise RMS (levels) | Pixels > 4 levels | Pixels > 16 levels | Pixels > 10% off |
|---|---:|---:|---:|---:|---:|---:|
| Static camera, static lights | 44.9 | 22.30 (24.83) | 0.049 | 0.000% | 0.0000% | 0.000% |
| Moving camera, static lights | 29.8 | 33.61 (35.68) | 1.108 | 0.989% | 0.0158% | 3.933% |
| Moving camera, light ball | 22.0 | 45.37 (46.84) | 1.093 | 0.980% | 0.0166% | 3.502% |

The last column counts pixels whose error exceeds 10% of their settled value and two display levels.
The scene's mean display level is 16 of 255, so absolute levels understate the noise a viewer sees.
The settled image is almost noise-free. All measured noise comes from camera motion, and the ball adds none.
The ball costs 8.2 ms with the camera still (held poses, 22.6 → 30.8 ms) and 11.8 ms while panning. The RHI
thread waits on the GPU in both cases. Nsight measured only 2.2 ms for the ball on a different pan, so the
cost depends on the pose and has not been attributed to a pass. [Report](../../build/gi-denoiser-final-fix/three-case/report.json)

<a id="next-fix-pixel-history-for-camera-motion"></a>

## Pixel history for camera motion

The goal is for surfaces that stay in view during camera motion to approach the static image's noise.

### What the motion noise is

In the moving case, 3.93% of pixels are more than 10% off their settled value, against 0.000% for the
still camera. A full-resolution crop of the worst region (frame 1270, a wall, vault and column capitals)
shows two kinds of error:

- Probe-sized blotches, 8–30 pixels across, on smooth lit surfaces. The warm vault is visibly mottled
  in the moving frame and smooth in the settled one.
- Bright outlines along geometry edges: column capitals, cornices and arches.

About half the error energy is low-frequency. Over the six scored frames, the 15 × 15 box mean of the
difference has an RMS of 0.69 display levels, and the residual grain 0.64.

### Why it is noisy

The horizontal field of view is about 90°, so at 1° per frame the 4K image moves about 34 pixels per
frame at its centre. The probe spacing is 8 pixels. Every anchor therefore exceeds the fast-motion test
and is re-placed every frame ([screen_probe_place.comp:209-221](../../Data/Shaders/VoxelGI/screen_probe_place.comp)).
A re-placed probe takes history reprojected from neighbouring probes, discounted by footprint support and
spatial variance (`ProbeRelocatedBatches`, [screen_probe.glsl](../../Data/Shaders/VoxelGI/screen_probe.glsl)).
History therefore never builds up during a pan, while a still probe keeps up to 1,152 frames. Nothing
per-pixel carries history across camera motion: the gather writes transient textures, and there is no
final diffuse pixel history ([NativeGIPlan.md:2034](../NativeGIPlan.md)). Shipped renderers keep one after
the probe or cache stage: Lumen's screen-probe temporal filter followed by its temporal upscaler, and
RTXGI's REBLUR pass after cache queries.

### Design

With temporal GI enabled, `ScreenProbeHistory` runs after `ScreenProbeGather` and supplies the sky
and bounce textures used by composition. Rough probe reflections and glossy reflections keep their
own histories. The raw gather remains available to diagnostics.

For each pixel:

1. **Reproject.** Reconstruct the pixel's world position from its depth and project it with
   `hybrid.previousProjectionView`. This is exact for static geometry under camera motion. Reject
   previous taps with `DenoiseHistoryRejection` on position and normal, as `GlossySurfaceCandidate`
   does ([glossy_reflection_temporal.comp](../../Data/Shaders/VoxelGI/glossy_reflection_temporal.comp)).
   While the camera moves, history is read with the Catmull-Rom weights of the pixel denoiser's
   `CubicHistory` when all 12 taps of that footprint pass the test, and otherwise with the valid taps
   of a 2 × 2 bilinear footprint. Pixels without support, such as disocclusions and screen edges,
   restart at the current gather.
   The receiver motion vector also identifies moved geometry when it disagrees with the camera reprojection.
2. **Limit history by measured lighting change, not probe age.** Probe age falls on every
   re-placement, which is the case this pass must bridge. Lighting change does not.
   - The vertical spatial filter writes each probe's held change into the unused `w` of its gather
     statistics (`filteredProbeLighting[grid.w * coefficients + index]`). That value is the larger of
     the sky and bounce `nextResponse` changes and the moving signal's unexplained change.
   - The pass reads the four nearest probes and applies `WorldHistoryCap(change, sigma, 32)`. That is
     the probes' rule, one frame late as in the probes.
   - Where moving lights supply a significant share of bounce, history is also capped at the probe's
     moving-signal prediction age, because the pixel history has no same-ray prediction.
   - Maximum 32 frames.
3. **Clamp against the probe noise estimate.** Noise in the current gather is correlated at probe
   scale. A 3 × 3 pixel variance clamp would see almost no variance inside a blotch and pin the history
   to it. The tolerance is three standard deviations from the sum of the four nearest probes'
   gather noise variance (`filteredProbeLighting` `x` and `y`) and the 5 × 5 spatial variance of
   the current gather, with a 20% relative floor. Values beyond it are pulled back softly.
4. **Blend.** `age = min(previous age + 1, cap)` and `result = mix(history, current, 1 / age)`.

Storage at 3840 × 2160 is 443 MiB. Each side of the double buffer holds two RGBA16F lighting signals
with independent ages and a 12-byte guide per pixel. The guide stores depth, packed normals, and the
full receiver identity. Unchanged receivers can use a single history tap. The 5 × 5 spatial variance is
evaluated only for tiles that need it. Histories reset on camera cuts, resizes, GI setting changes, and
`InvalidateHistory`.

### How this differs from the rejected pixel history

[GIDenoiserPlan.md:755](GIDenoiserPlan.md) records a 1,012.5 MiB, 32-frame per-pixel history fused into
the gather that failed all nine motion comparisons and light recovery. This design differs in three ways:

- It is a separate pass that never feeds probes.
- Its length follows measured lighting change, so a lamp edit restarts it where the probes restart.
- Its clamp uses the probes' own noise estimate instead of pixel neighbours.

### Acceptance

Measured with `tools/gi_noise_fps.py` on the same New Sponza setup, three launches:

| Case | Today | Target |
|---|---:|---:|
| Moving camera, pixels > 10% off | 3.93% | ≤ 1.0% |
| Moving camera, RMS (levels) | 1.11 | ≤ 0.55 |
| Moving camera with ball | 3.50% | same as moving |
| Static camera | 0.000%, RMS 0.049 | unchanged |
| Frame time added by the pass | — | ≤ 1.0 ms in every case |

Also required:
- `NativeProbesRecoverLightingWithoutCPURevisionHint` passes in both modes. Light recovery is how
  the rejected history failed.
- The pass-list pins in `HybridGIPassListTests.inl` are updated.
- The world cache keeps 1,048,576 cells at 4K.

### Pixel history measurements (2026-11-27)

The same three-case replay ran before and after the change with the Release ZenApp executable, on an
RTX 5080 at 3840 × 2160 fullscreen, hardware RT, local response, and async compute disabled. Each row
is one launch per build, measured against its own settled images.

| Case | RMS (levels) | Pixels > 10% off | Median frame time (ms) |
|---|---:|---:|---:|
| Static camera, static lights | 0.049 → 0.035 | 0.000% → 0.001% | 18.24 → 19.15 |
| Moving camera, static lights | 1.111 → 0.893 | 3.948% → 1.557% | 26.93 → 27.81 |
| Moving camera, light ball | 1.098 → 0.856 | 3.537% → 1.135% | 32.06 → 28.53 |

The moving cases reduced RMS by 20–22% and reduced the fraction of pixels over 10% off by 61–68%.
The results still exceed the 0.55 RMS and 1.0% motion targets. In the static-light pan, the 15 × 15
box mean of the display error fell from 0.690 to 0.453 RMS levels. The residual stayed at approximately
0.64 RMS levels. The world cache retained 1,048,576 cells in every captured case. The whole-frame
timings are single-launch medians at unaltered clocks.
[Baseline report](../../build/gi-denoiser-final-fix/pixel-history-baseline/report.json) ·
[Pixel history report](../../build/gi-denoiser-final-fix/pixel-history-three-case/report.json)

In 30-frame 4K Nsight GPU traces at unaltered clocks, the median `ScreenProbeHistory` interval was
0.884 ms (static), 0.927 ms (moving), and 0.828 ms (moving with ball). Each median met the 1.0 ms
target. The longest individual interval across the three traces was 1.082 ms.
[Static trace](../../build/gi-denoiser-final-fix/pixel-history-trace/static/summary.json) ·
[Moving trace](../../build/gi-denoiser-final-fix/pixel-history-trace/moving/summary.json) ·
[Moving ball trace](../../build/gi-denoiser-final-fix/pixel-history-trace/moving-ball/summary.json)

All 20 native GPU cases passed across four submission modes, including both unannounced light
recovery fixtures, camera-cut history checks, and the pass-list pins. RenderCore passed 660 tests.
[Native results](../../build/gi-denoiser-final-fix/pixel-history-native.xml) ·
[RenderCore results](../../build/gi-denoiser-final-fix/pixel-history-render-core.xml)

### Bicubic history and effective probe age (2026-11-27)

A code review of the first implementation found two limits:

- **Bilinear resampling.** The history was resampled with a bilinear footprint every frame. The pass now
  uses the validated Catmull-Rom footprint described in the design.
- **Inflated ages for re-placed probes.** A re-placed probe recorded its footprint's age, which grows
  toward 256 frames during a pan, while `ProbeRelocatedBatches` kept only part of that history's weight.
  The spatial filter takes the larger of the two ages (`ProbeSampleAge`), so it gave re-placed probes a
  mature probe's narrow footprint. The gather noise estimate divided by the same inflated age.
  [screen_probe_integrate.glsl](../../Data/Shaders/VoxelGI/screen_probe_integrate.glsl) now records the
  age scaled by the share `ProbeRelocatedBatches` retains. The history weight is unchanged.

All 20 native GPU cases passed across four submission modes.
[Native results](../../build/gi-denoiser-final-fix/history-fixes-native.xml)

The three-case replay used the same setup and tool. Each row is one launch per build.

| Case | RMS (levels) | Pixels > 10% off | Pixels > 4 levels | Median frame time (ms) |
|---|---:|---:|---:|---:|
| Static camera | 0.035 → 0.025 | 0.001% → 0.000% | 0.001% → 0.000% | 19.15 → 18.92 |
| Moving camera | 0.893 → 0.839 | 1.557% → 1.297% | 0.355% → 0.402% | 27.81 → 26.42 |
| Moving camera, light ball | 0.856 → 0.792 | 1.135% → 0.968% | 0.351% → 0.368% | 28.53 → 35.91 |

In the moving case, pixels more than 10% off fell from 0.905% to 0.705% on surfaces away from
geometry edges and from 0.567% to 0.512% near edges. The two screen-edge strips stayed at 0.08%.
The 15 × 15 box mean of the error fell from 0.452 to 0.422 RMS levels and the residual from 0.641
to 0.598. Mean signed error moved from −0.179 to −0.057 levels. At frame 1270 the probe-scale
mottling on the vault is gone. Smooth differences remain along the arch, with a dark band beside the
right arch, the edges, and faint lines along the brick courses. The lines persist with bicubic history,
so resampling did not cause them. Pixels above four levels rose 13% in the moving case; native launches
are not bit-identical, and this is one launch per build.
[Report](../../build/gi-denoiser-final-fix/history-fixes-three-case/report.json)

Moving-camera RMS and pixels above 10% still miss their targets (0.55 and 1.0%). The light-ball
case reached 0.968%.

The light-ball frame time is not attributable from these launches. Its held-pose frames, where neither
change alters the work, measured 24.86, 23.83 and 26.82 ms in the three most recent launches; the case
without the ball stayed within 19.24–19.91 ms. Pan time minus held time with the ball was 7.20, 4.70
and 9.09 ms in the same launches. An interleaved GPU trace would separate a real cost from this
launch-to-launch variation.

Two 80-frame 4K Nsight traces at unaltered clocks then followed the same ±30° pan, with the light ball
on and off. GPU frame medians were 30.78 ms and 26.16 ms. The 4.62 ms difference lies entirely in the
traced passes; the part of each frame outside any pass was 0.06 ms in both traces.

| Pass | No ball (ms) | Ball (ms) | Difference (ms) |
|---|---:|---:|---:|
| `ScreenProbeMoving` | — | 1.603 | +1.603 |
| `ScreenProbePrefilter` | 1.581 | 2.623 | +1.041 |
| `ScreenProbeTemporal` | 1.230 | 2.107 | +0.877 |
| `ScreenProbeShadeHits` | 3.166 | 3.627 | +0.461 |
| `ScreenProbeSpatial` and `ScreenProbeSpatialHorizontal` | 0.711 | 1.079 | +0.368 |
| `ScreenProbeSpecularSpatial` | 1.084 | 1.226 | +0.142 |
| `ReceiverVisibility` (direct shadows) | 1.777 | 1.902 | +0.126 |

The ball's 4.6 ms on this pan is GI work for the moving-light signal. The replays measured 9.5 ms for
the same pan, and the case without the ball agreed with Nsight within 0.3 ms.
[Ball trace](../../build/gi-denoiser-final-fix/ball-trace/ball/summary.json) ·
[No-ball trace](../../build/gi-denoiser-final-fix/ball-trace/noball/summary.json)

#### Light-ball optimization and launch variation (2026-11-27)

`ScreenProbeMoving` now writes an empty, ageing record for a probe whose rays reached no moving-light
surface and whose history is dark. A re-placed probe reads its matched predecessor's moving history,
discounted by `ProbePlaneWeight`, instead of the 18-probe footprint. The rough prefilter runs the
moving-light prediction only for probes with a predicted change, and sums the energy-weighted change
across lanes. All 20 native GPU cases passed.
[Native results](../../build/gi-denoiser-final-fix/ball-opt-native.xml)

The same pan was traced again. `ScreenProbeMoving` fell from 1.603 to 0.818 ms. The ball's prefilter
measured 2.575 ms against 2.623 ms, so the prefilter changes did not measurably help. The three-case
noise was unchanged: 1.291% of moving pixels and 0.968% with the ball more than 10% off.
[Ball trace](../../build/gi-denoiser-final-fix/ball-trace-opt/ball/summary.json) ·
[No-ball trace](../../build/gi-denoiser-final-fix/ball-trace-opt/noball/summary.json) ·
[Three-case report](../../build/gi-denoiser-final-fix/ball-opt-three-case/report.json)

Whole-frame comparisons between launches are not reliable. Three passes run at one of two speeds per
launch, independently of the ball, while the other passes agree within 0.05 ms:

| Pass | Four launches (ms) |
|---|---|
| `ScreenProbeGather` | 3.437, 4.643, 3.501, 3.441 |
| `ScreenProbeTemporal` | 1.230, 2.273, 2.107, 2.250 |
| `ScreenProbeHistory` | 1.153, 1.667, 1.186, 1.741 |
| `ScreenProbeProposal` (control) | 1.990, 1.987, 2.012, 2.002 |

The gather, temporal and pixel-history SPIR-V files were the same in all four launches. Earlier traces found
identical SPIR-V receiving different register allocations between launches. The +0.877 ms first
attributed to the ball in `ScreenProbeTemporal` was this variation. Between the slow and fast speeds,
these three passes differ by about 2.8 ms per frame.

#### Review fixes before commit (2026-11-27)

A code review of the full change found four behaviour problems in the pixel history, now fixed:

- An environment intensity or rotation edit in local mode neither invalidated history nor produced
  change evidence, so the pixel history could keep the old sky for up to 32 frames. Any environment
  revision change now resets the pixel history. The probes still rescale or predict the edit.
- A failed pixel-history allocation disabled screen-probe GI. The pixel history is now optional: the
  probes gather without it and a warning is logged.
- In global mode the pixel history ignored the geometry-change window. It now keeps at most 16 frames
  while `sourceSignature.z` is below 64, as the global probes do.
- A pixel with no compatible probe received no change evidence and kept 32 frames after a light edit.
  It now takes the largest change and moving-light limit among the eight nearby probes.

The moving-light limit no longer switches from the moving age to 32 frames when the moving share falls
below 10%. It is the moving age multiplied by `share^(-2/3)`, capped at 32 frames, because the lag error
of an unpredicted contribution scales with its share. The changed C++ and GLSL files are formatted, and
the blue-noise generator writes LF.

With these fixes, all 20 native GPU cases and 32 GI settings tests passed. The three-case check measured
0.000%, 1.297% and 0.971% of pixels more than 10% off for the still, moving and light-ball cases,
unchanged from the previous build. Frame time with the light ball was 38.92 ms in this launch, against
31.92 and 35.91 ms in the two before it, the launch variation described above.
[Native results](../../build/gi-denoiser-final-fix/commit-native.xml) ·
[Three-case report](../../build/gi-denoiser-final-fix/commit-three-case/report.json)

### Expected limits

- Newly revealed surfaces start at the current gather and keep today's noise for their first frames.
  In this pan the leading edge reveals about a 34-pixel strip per frame, about 1% of the frame width.
- Moving receivers restart where the surface test or receiver motion rejects camera reprojection.
- Light from moving lights converges no faster than the moving probe signal.

### Steps

1. Filter: write each probe's held change into its gather statistics.
2. Pass and history textures in `ScreenProbeGI`, wired as the sky and bounce outputs in
   `HybridGIRenderer`, with the resets above.
3. Update the pass-list pins.
4. Run the three-case check and the native GPU tests, and record both here.

## What to measure

1. Light ball on, still camera and walking: whole-screen noise against `global`.
2. The scenario suite in `tools/gi_scene_replay.py` with both responses.
3. The 20 native GPU integration tests, especially the unannounced directional light off/on
   fixture. Its earlier overshoot was reduced by a longer evidence hold, which fix 9 removes.
4. Slow and fast pans, diffuse and reflections, against the retained-mode numbers in the plan.
5. The 4K frame-time trace. The moving pass now reduces lane variances and can reproject a
   footprint, the rough prefilter accumulates moving-light deltas, and the single-thread light-edit
   pass compares every light each frame in local mode.

## Changed files

- Activation: `VoxelGIRenderer.h`, `VoxelGISettings.cpp`, `LightingBackendControls.cpp`, `Data/engine.cfg` (comment).
  The optimal preset no longer keeps `worldCacheSeed` in local mode: it is an `engine.cfg` configuration key,
  so enabling it makes the settings custom and a saved configuration keeps it. `tools/gi_scene_replay.py`
  now treats a setup without `localHistoryResponse` as local, matching the engine default.
- Moving-light separation: `world_cache_lighting.glsl`, `world_cache_lighting.comp`, `moving_light.glsl`,
  `screen_probe_proposal.comp`, `Calibration/world_cache_check.glsl`, `WorldRadianceCache.h` (buffer size).
- Moving signal: `screen_probe_moving.comp`, `screen_probe.glsl`, `screen_probe_trace.glsl` (cache share bits 24-31),
  `screen_probe_filter.glsl`, `world_cache_resolve.glsl`, `ScreenProbeGI.cpp` (six moving vectors).
- Reflections: `screen_probe_prefilter.glsl`, `glossy_reflection_trace.glsl`, `glossy_reflection_clamp.comp`.
- Evidence, recovery and sky: `screen_probe_trace.glsl`, `screen_probe_integrate.glsl`, `world_cache_resolve.glsl`,
  `ScreenProbeGI.cpp` (ten sky-delta words per probe).
