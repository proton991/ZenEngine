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

## Large-angle specks (2026-11-27)

`build/gi-denoiser-final-fix/replay-large-angle.py` turns the 4K New Sponza camera 30° per frame out to
180° and back, then snaps 180° in one frame. Each turned frame is compared with settled frames at the same
pose. About 40% of each sweep frame was outside the previous view. The five settled frames per pose
differ from their mean in only 0–4 pixels by more than 24 levels, so every speck below comes from motion.
The specks fell into two kinds:

- **Sun shadow edges.** Shadowed pixels along hard sun edges turned nearly fully lit. The gather adds the
  exact per-pixel sun (`GatherProbeSources`) to the sky before the pixel history. While the camera moves,
  the history resamples the previous frame between pixels and blurs the edge. The 5×5 variance clamp
  does not catch it, because the edge itself makes that variance large. This was added with the pixel
  history in 4ee3dbf9.
- **Warm glints on curved rims.** These are mostly on newly revealed pixels. A reflection-only capture
  (screen-probe debug face 1, `glint-diagnostic`) at frame 1164 put them in the rough probe specular.
  Glint pixels were 6–20× their settled value in linear light. The revealed area as a whole was close to
  settled (median ratio 0.96, 0.24% of pixels above 3×). The same rims glowed at frames 1164 and 1933,
  which points to a systematic error in new probes rather than noise. Scene roughness is above 0.4
  almost everywhere, so the glossy pass is not involved.

**Fix A (kept): the sun stays out of the pixel history.** `screen_probe_history_sources.comp` subtracts
the sources from the gathered sky, blends only the residual, and stores it as the history. A new pass,
`ScreenProbeSkySources` (`screen_probe_sky_sources.comp`), adds the current sources back. It writes the
result into the history's previous-side sky texture, which nothing reads until the next frame's history
pass overwrites it, and that texture becomes the sky output. The raw gathered sky keeps the sun for
captures and debug views. A change between the two history kinds restarts the history.

A first version wrote the lit sky to a new 4K RGBA16F frame texture instead (66 MB). With the light ball
on, this scene leaves the world cache planner with about 1.1–1.5 GB of headroom. The planner grants a
quarter of the headroom below 90% of the VRAM budget, so those 66 MB dropped the cache from 1,048,576
to 524,288 cells in two of four ball launches. Earlier builds received 1,048,576 cells in every launch.
Reusing the previous side costs no memory.

**Fix B (tried, reverted): limit fresh probe specular.** For a probe's first three frames, each specular
texel was limited to twice the median of its own and its adjacent probes' estimates on any surface
(`screen_probe_specular_filter.comp`, at least seven estimates).

Sweep columns average the 12 turned frames. "+24" and "−24" count pixels per frame more than 24 levels
brighter or darker than the settled image. Every launch below had 1,048,576 world cache cells.

| Case | Build | Sweep clusters (edge / new glint / seen glint) | Bright px | +24 | −24 | > 24 levels | RMS | Snap clusters |
|---|---|---|---|---|---|---|---|---|
| Static lights | 4ee3dbf9 | 316 (177 / 85 / 45) | 72.9 | 242 | 1182 | 0.017% | 1.23 | 19 |
| Static lights | A | 148 (23 / 78 / 39) | 37.8 | 56 | 1142 | 0.015% | 1.20 | 29 |
| Static lights | A + B | 71 (19 / 33 / 14) | 19.6 | 30 | 2317 | 0.028% | 1.36 | 3 |
| Light ball | 4ee3dbf9 | 290 (105 / 114 / 69) | 77.3 | 181 | 986 | 0.014% | 1.32 | 48 |
| Light ball | A | 203 (23 / 98 / 76) | 55.8 | 82 | 968 | 0.013% | 1.31 | 58 |
| Light ball | A + B | 73 (17 / 34 / 17) | 23.8 | 46 | 2302 | 0.028% | 1.51 | 7 |

Fix A removed 78–87% of the edge specks and halved the bright pixels with static lights, without making
anything darker. RMS and the share of pixels more than 24 levels off fell slightly. The glints are
unchanged, as expected. The snap reveals the whole frame, which has no history for fix A to act on;
its differences are launch variation. Fix B cut the glints by about 60%, but it doubled the pixels more
than 24 levels too dark, for example 1663 → 3581 at frame 769. Lamp-lit ledges and beams
seen at a grazing angle turn dark in revealed areas. 89% of those pixels are in revealed areas, where the
pixel history has nothing to blend, so fix A cannot cause them. A median over every surface is the wrong
reference for a directional quantity: a neighbour facing elsewhere sees different light in that
direction. Fix B was reverted.

### Stage 1: new-probe reflection estimate (2026-11-27)

The rough-specular prefilter divided each texel's samples by their actual lobe weight, the ratio
estimator of stochastic screen-space reflections. That estimator assumes the samples were drawn for the
lobe. Probe rays are drawn for the hemisphere and the lights, so a grazing rim lobe can hold only a few
lamp-guided samples, and their mean became the lobe's value. Probes with history do not show this: history
is counted at the expected weight, and the running value converges to the ratio of sums over many frames,
which is consistent. A new probe had no history, so one frame's biased ratio was its whole estimate.

Two corrections, measured with the reflection-only diagnostic at frame 1164:

| Estimator for a new probe | Revealed-area mean / settled | Pixels > 3× settled | Glint pixels × settled |
|---|---|---|---|
| Ratio (4ee3dbf9) | 0.894 | 0.240% | 5.1–18.5 |
| Divide by at least the expected weight | 0.736 | 0.055% | 2.5–5.2 |
| Fill missing weight with the probe's mean radiance (kept) | 0.898 | 0.078% | 2.5–5.4 |

The settled reflection is unchanged by both (ratio 0.999 to the baseline). Dividing by the expected weight
alone caps the rare peak-hit frames that carry a narrow lobe's mean, so new probes read 18% dark. The kept
version fills only the weight missing from one expected frame. It uses the probe's cosine-weighted mean
radiance from the same samples, about its irradiance over π. A lobe sampled at or above its expected weight
keeps its ratio, bounded by the brightest sample. A probe with at least one expected frame of weight,
history included, is unchanged. No pass was added.

| Case | Build | Sweep clusters (edge / new glint / seen glint) | Bright px | +24 | −24 | RMS | Snap clusters / bright px |
|---|---|---|---|---|---|---|---|
| Static lights | A | 148 (23 / 78 / 39) | 37.8 | 56 | 1142 | 1.20 | 29 / 42.0 |
| Static lights | A + stage 1 | 133 (35 / 60 / 31) | 34.7 | 54 | 1123 | 1.19 | 9 / 18.0 |
| Light ball | A | 203 (23 / 98 / 76) | 55.8 | 82 | 968 | 1.31 | 58 / 133.0 |
| Light ball | A + stage 1 | 157 (31 / 64 / 58) | 43.8 | 73 | 981 | 1.30 | 31 / 73.5 |

New-area glints fell by 23–35% on the sweeps, and snap clusters by 47–69%, with no added darkening. The edge
counts moved within launch variation. The remaining glint pixels are 2.5–5× settled with either correction,
so they come from frames where a lamp-guided sample lands near the lobe peak (weight above expected). One
frame of guided rays cannot fix a narrow lobe's mean: the remaining error is too little information in a
new probe, the same cause as the dark revealed areas (stage 2).

### Stage 2: newly revealed areas (2026-11-27)

Sky-only and bounce-only diagnostics (debug faces 0 and 2) at frame 1164 do not show revealed areas
starting dark on average:

| Signal | Revealed mean / settled | Median | 10th–90th percentile | < 0.5× | > 2× |
|---|---|---|---|---|---|
| Sky (with exact sun) | 0.999 | 1.000 | 0.75–1.22 | 3.50% | 0.23% |
| Bounce | 0.977 | 0.984 | 0.85–1.10 | 0.76% | 0.18% |
| Reflection (stage 1) | 0.910 | 0.978 | 0.67–1.32 | 3.32% | 0.79% |

The distribution is skewed: most new pixels sit somewhat low and a few far high. This is the
first-frame noise of 48 rays per new probe (`ZEN_SCREEN_PROBE_MAX_BATCHES` × 16). Re-weighting the same
rays cannot remove it. Options that add information are more first-frame rays, whose cost grows with how
much a turn reveals, or a world-space record that a revisited area starts from. The existing cache seed
(`worldCacheSeed`, at most one batch of weight) lowered young-probe RMS by 3% in the earlier plan, so it was
not rerun here.

### Stage 3: normal camera motion (2026-11-27)

The three-case check (`tools/gi_noise_fps.py`, 1° per frame pan) with fix A and stage 1:

| Case | Build | Frame ms | RMS | > 4 levels | > 16 levels | > 10% off |
|---|---|---|---|---|---|---|
| Still | 4ee3dbf9 | 18.88 | 0.025 | 0.000% | 0.0000% | 0.000% |
| Still | A + stage 1 | 19.08 | 0.025 | 0.000% | 0.0000% | 0.000% |
| Moving camera | 4ee3dbf9 | 26.33 | 0.842 | 0.405% | 0.0154% | 1.297% |
| Moving camera | A + stage 1 | 28.82 | 0.805 | 0.403% | 0.0123% | 1.297% |
| Light ball | 4ee3dbf9 | 38.92 | 0.788 | 0.363% | 0.0132% | 0.971% |
| Light ball | A + stage 1 | 38.58 | 0.762 | 0.375% | 0.0117% | 0.977% |

Large errors fell 11–20% and RMS 3–4%. The share more than 10% off did not change. The moving-camera frame
time rose 2.5 ms in this launch, while the light-ball and still cases, which run the same passes,
changed by under 0.4 ms. That matches the known per-launch pass-speed variation rather than the added
pass.

Where the moving-camera error is:

- Only 11–15% of the pixels more than 10% off lie in the 140-pixel strip at the leading edge, and about 10%
  on luminance edges. The rest are on surfaces that stayed in view.
- 82% of them are within 4 levels of the settled image, at a settled level near 12, with mixed sign.
- The signed error's standard deviation is 0.73 levels per pixel and 0.40 levels for 16 × 16 block
  means: much of it is at probe scale. The worst block, lantern glass, shows smooth 30–60 pixel blotches
  in both the settled and the moving frame, in a different pattern.

This error is against a settled image whose own probe pattern is fixed, so it cannot separate crawl
from that pattern. `build/gi-denoiser-final-fix/crawl.py` measures crawl directly: consecutive frames of a
1° per frame pan, the earlier reprojected exactly into the later view (the camera only rotates), with the
16 × 16 block mean of the difference as the probe-scale change. Frames held still in the same launch
measure 0.001. Binned by the pixel history's bounce age (debug face 3, same camera path):

| History age | Share of frame | Block-mean change per frame |
|---|---|---|
| 0–4 frames | 4.6% | 0.228 levels |
| 4–8 | 7.1% | 0.191 |
| 8–16 | 11.4% | 0.159 |
| 16–31 (mostly geometry edges) | 6.1% | 0.431 |
| 32, the cap | 68.8% | 0.085 |

On surfaces seen since the pan began, the history stays at its cap except along geometry edges, and the
probe-scale change is under a tenth of a level per frame. The visible motion noise is the convergence of
surfaces revealed in the last ~16 frames, about a third of the screen during this ±30° pan. The
settled-image error on mature surfaces is the difference between two stable probe patterns, not crawl.
Items 1–3 therefore share one cause: a newly revealed surface starts with one frame of 48 rays per probe.

### Proposed no-visible-noise gate

All on 4K New Sponza with the full world cache, from the two existing tools:

| Test | Measure | Now (A + stage 1) | Gate |
|---|---|---|---|
| Three-case, still | Pixels > 4 levels | 0.000% | 0.000% |
| Three-case, moving camera and light ball | Pixels > 10% off | 1.297% / 0.977% | ≤ 1.0% |
| Three-case, moving camera and light ball | 16 × 16 block-mean error, std | 0.40 levels (moving) | ≤ 0.2 levels |
| Three-case, moving camera and light ball | Pixels > 16 levels | 0.0123% / 0.0117% | ≤ 0.005% |
| Large-angle sweep | Speck clusters on surfaces in view last frame (edge + seen glint) per frame | 5.5 / 7.4 | 0 |
| Large-angle sweep and snap | Speck clusters on newly revealed surfaces per frame | 5.0 / 5.3 | ≤ 1 |
| Editor, by eye | Light ball, slow pans and fast turns | — | No visible speckle or crawl |

The block-mean row targets the probe-scale crawl that the per-pixel measures understate. Revealed
surfaces get a nonzero allowance because one frame of rays cannot be noise-free.

Still open:

- New probes start from one frame of rays: the first-frame noise of revealed surfaces, their ~16-frame
  convergence during pans, and the remaining new-probe glints. [ProbeMemoryPlan.md](ProbeMemoryPlan.md) plans
  the fix for surfaces seen before.
- A single pixel at (114, 808) reads (164, 127, 71) at frames 772 and 1160, the same 120° pose, and is
  black when settled. It vanished with the expected-weight estimator and returned with the kept one, so it
  is a new-probe reflection like the other glints, deterministic because each frame's rays are.

[Before](../../build/gi-denoiser-final-fix/large-angle-sparkles/summary.json) ·
[Fix A](../../build/gi-denoiser-final-fix/large-angle-fix-a/summary.json) ·
[A + B](../../build/gi-denoiser-final-fix/large-angle-after-fix/summary.json) ·
[A + stage 1](../../build/gi-denoiser-final-fix/large-angle-stage1b/summary.json) ·
[Reflection diagnostic](../../build/gi-denoiser-final-fix/glint-diagnostic)

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
- Large-angle specks: `screen_probe_history.glsl` (body moved from `screen_probe_history.comp`),
  `screen_probe_history.comp`, `screen_probe_history_sources.comp`, `screen_probe_sky_sources.comp`,
  `ScreenProbeGI.cpp/.h`, `ShaderProgram.cpp`, and the pass-list pins in `HybridGIPassListTests.inl`.
