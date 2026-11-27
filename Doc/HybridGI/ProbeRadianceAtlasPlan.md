# Screen-probe radiance atlas (plan, 2026-11-27)

The next step to close the remaining noise gap in [GIDenoiserFinalFix.md](GIDenoiserFinalFix.md).

## Remaining gap

Measured on 4K New Sponza with the full world cache, against the proposed no-visible-noise gate:

| Test | Now | Gate |
|---|---|---|
| Still camera, pixels > 4 levels | 0.000% | 0.000% |
| First shown frame (128-frame warm-up), pixels > 10% off | 0.04% | — |
| 1° per frame pan, pixels > 10% off, moving camera / light ball | 1.256% / 0.928% | ≤ 1.0% |
| Same, pixels > 16 levels | 0.0123% / 0.0116% | ≤ 0.005% |
| Same, 16 × 16 block-mean error std | 0.40 levels | ≤ 0.2 levels |
| 30° per frame sweep, speck clusters per frame on surfaces seen last frame, static / light ball | 6.0 / 7.0 | 0 |
| Sweep and 180° snap, speck clusters per frame on revealed surfaces | 4.5–13.5 | ≤ 1 |

Two causes remain:

1. **Rough reflections are ratio estimates over random rays.** A probe's 48 rays per frame are drawn for its
   hemisphere and its lights, not for each reflection lobe. A narrow lobe receives zero to two rays, with
   weights tens of times their expectation. About 80% of the large-angle specks are these glints, mostly
   on rims. Re-weighting the rays has been tried twice: filling missing weight with the probe's mean
   (stage 1, kept) and an effective-sample prior (reverted). Neither can remove the heavy tail without bias.
2. **A new probe starts from one frame of rays.** Surfaces revealed in the last ~16 frames change by
   0.21–0.31 levels per frame during a pan. Mature surfaces change by 0.085. These measures did not move
   much: a world-space table of probe irradiance (W1), more first-frame rays, and longer-lived relocated
   probes each gained 10% or less.

## How shipped engines do it

- **Lumen** (Wright, SIGGRAPH 2021 and 2022) stores screen-probe radiance in an octahedral texel layout.
  Trace directions importance-sample the BRDF of the pixels that use the probe and the incoming lighting
  from the last frame's filtered probe radiance, or from the world radiance cache. Rays whose
  normalized BRDF PDF falls below `r.Lumen.ScreenProbeGather.ImportanceSample.MinPDFToTrace` (0.1) are given
  to more important directions ([cvar reference](https://indxzero.github.io/ue544cvarwiki/articles/r.lumen.screenprobegather.importancesample.minpdftotrace)).
  Pixels with roughness of 0.4 and above take their reflections from the screen probes instead of tracing
  ([Lumen performance guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine)).
- **AMD GI-1.0** ([report](https://ar5iv.labs.arxiv.org/html/2310.19855), section 2.1) follows the same
  encoding: an 8 × 8 octahedral hemisphere atlas per probe.
  - Rays: each picks an atlas cell from a CDF of the reprojected radiance and a jittered direction
    inside it. Cells that got no ray take the average of those that did.
  - New probes: a probe without a predecessor scatters the radiance of the 3 × 3 neighbouring previous
    probes into its own atlas, correcting each texel's parallax with the stored hit distance. An LRU cache
    of evicted probes covers thin geometry where reprojection keeps failing.
  - Filtering: a separable 7 × 7 probe-space blur per texel rejects neighbours by plane distance, normal
    and angle error.

Both store radiance per direction cell. Every direction then has a value. A ray only informs its own cell,
and filtering and reconstruction work per direction rather than on an integrated quantity.

## What ZenEngine has

- Each batch draws 16 rays from a mixture: uniform, lamp-aimed, learned lobe, environment and a learned
  64-bin octahedral CDF lane. Each ray carries the mixture PDF, i.e. one-sample MIS. An active probe
  traces up to 3 batches a frame (48 rays), a mature one 2 (`screen_probe_trace.glsl`, `ScreenProbeGI.h`).
- Diffuse sky and bounce are integrated into eight lighting vectors per probe, plus statistics.
- Rough reflections live in 112 texels per probe: an 8 × 8 full-sphere octahedral level at roughness 0.25,
  and 4 × 4 levels at 0.5, 0.75 and 1.0. Each frame the prefilter (`screen_probe_prefilter.glsl`) estimates
  them as GGX-weighted ratios over all of the frame's rays. A 3 × 3 spatial filter follows
  (`screen_probe_specular_filter.comp`). Storage is 113 × 8 bytes per probe on each of the four history
  sides.
- The learned CDF already bins directions into the same 8 × 8 full-sphere octahedral layout
  (`ProbeOctBin`).

## Design

### 1. A radiance atlas per probe

- Each probe keeps an 8 × 8 octahedral atlas over its hemisphere (GI-1.0's layout). A full-sphere layout
  would spend half its cells below the horizon. Each texel holds RGB radiance and hit distance in FP16,
  8 bytes.
- The atlas replaces the 64 texels of the 0.25 roughness level, which also hold 8 bytes each. Probe
  memory stays the same, and the world cache planner sees no new bytes.

### 2. Rays assigned to cells

- Each ray belongs to one atlas cell: a stratified base, where every cell receives a ray within two
  frames (48 rays per frame for 64 cells), plus cells chosen from the learned CDF and the light guides.
  This follows Lumen's reassignment of low-PDF rays and GI-1.0's CDF cell pick.
- A ray updates only its own cell. Its weight inside the cell is the cell's solid angle over its PDF there,
  so it is bounded for the stratified and CDF rays. Lamp-aimed rays keep the mixture PDF.
- A cell with no ray this frame keeps its history. A new probe's empty cells take the average of its sampled
  cells, as stage 1 and GI-1.0 already do.

### 3. Diffuse and reflections from the atlas

- The sky and bounce lighting vectors are projected from the atlas. Moving-light signals keep their
  current path.
- Every reflection texel convolves the atlas with its GGX lobe: 64 cells against 112 texels per probe,
  a third more work than today's 48 rays against 112 texels. Every lobe then sees every direction, and its
  estimate is a bounded weighted mean of cells rather than a ratio over whichever rays fell near it. This
  removes the cause of the rim glints.
- The cost is blur. A lamp seen in a roughness 0.25 reflection spreads over one cell, about 18° across,
  instead of a ray's precision. Lumen accepts the same from roughness 0.4.

### 4. New probes rebuilt from their neighbours

- A probe without a predecessor fills its atlas from the 3 × 3 previous probes around it, as in GI-1.0.
  Each source texel's hit point (direction × hit distance) is re-aimed from the new probe's position.
  A source is rejected by plane distance and normal; a texel is rejected when its hit lies behind the
  new probe's plane.
- This differs from what was rejected before. W1 and the earlier neighbour-history borrowing reused
  integrated irradiance from other positions, and neighbouring spots differed about as much as one frame's
  noise. Re-aiming each direction's hit point corrects the parallax that makes them differ.
- The reconstruction is the probe's history with a weight of one frame, so its own rays take over as before.

## Stages and gates

| Stage | Change | Gate |
|---|---|---|
| R0 | Memory and pass budget: atlas in place of level 0, no new pass | World cache keeps 1,048,576 cells at 4K with the light ball |
| R1 | Atlas, rays assigned to cells, diffuse projected from the atlas | Three-case still 0.000% > 4 levels; moving and ball no worse than now |
| R2 | Reflections convolved from the atlas; remove the ratio estimator and the stage 1 fill | Large-angle glint clusters (new and seen) down at least 80%; no rise in the −24 counts; reflection diagnostic at frame 1164 within 10% of settled |
| R3 | New probes rebuilt from neighbours | `crawl.py` age 0–4 band below 0.15 levels per frame on both pans; three-case moving ≤ 1.0%; still camera without pops |
| R4 | Remove superseded paths (`ProbeRelocatedRoughHistory`, the prefilter ratio code) | Pass-list pins, settings tests |

Every stage is measured with the existing tools: `tools/gi_noise_fps.py` (three-case),
`build/gi-denoiser-final-fix/crawl.py`, `replay-large-angle.py` with `compare_specks.py`, and `startup.py`.
Each launch is repeated at least once, since launches vary as much as some effects.

## Risks

- **Coverage.** 48 rays cannot cover 64 cells in one frame. If R1 loses diffuse quality on new probes, new
  probes get one extra batch (64 rays). The more-rays experiment found extra first-frame rays lower the
  young-band change by only about 6%, so this is a coverage fix, not a noise fix.
- **Light guiding.** Lamp-aimed rays concentrate in a few cells. A lamp smaller than a cell must still
  dominate its cell without making the cell's estimate a single sample. R1 must show the large-angle
  edge and seen-glint counts do not rise.
- **Rough reflection sharpness.** The 0.25 level becomes cell-limited. If that is visibly blurrier on
  lamps, the glossy pass's roughness limit can rise to cover it, at the glossy pass's cost.
- **R3 may not reach its gate.** If the young band stays above 0.15, the remaining pan noise is probe
  placement itself, and the next lever is placement that keeps anchors through motion.

## R0–R2 results (2026-11-27)

### What was built

- **Layout.** The atlas uses the existing world-space 8 × 8 octahedral layout of the 0.25 roughness level and of
  the learned CDF bins (`ProbeOctBin`), not a hemisphere layout per probe. Cells then line up across probes,
  so relocation and the spatial filter work per cell. About 32 cells cover a probe's hemisphere (about 25°
  each). R0 is met by construction: the atlas takes level 0's storage, and no pass or memory is added.
- **Diffuse** keeps its current path. Only reflections read the atlas.
- **Prefilter** (`screen_probe_prefilter.glsl`). Each ray updates only its own cell, at its inverse PDF against
  the cell's expected weight (sample count × cell solid angle). Rays keep their current generation. The uniform
  lanes bound every cell's weight, so no reflection lobe multiplies a ray's weight. Each cell blends with its
  history at the probe's age, as the texels did. Missing weight in a young cell takes the probe's mean radiance.
  Relocated probes average the footprint's cells over the taps that see the cell above their horizon.
- **Specular spatial filter** (`screen_probe_specular_filter.comp`). It averages neighbouring probes' cells
  (aligned, so a cell means the same in every probe), from 3 × 3 probes for a mature probe to 7 × 7 for a new
  one, as the diffuse filter widens. It then integrates every reflection texel from the filtered cells with
  its GGX lobe. The weights are deterministic.

### Measurements

Baseline is `main` (f397b8aa); large-angle cases are two launches each.

| Measure | main | Atlas |
|---|---|---|
| Reflection-only view at frame 1164 (settled pose right after the sweep), pixels > 3× settled | 2,337 | 1,249 |
| Same, error against settled (RMS) | 4.92 | 3.86 |
| Settled reflection, ratio to main | — | 1.000 (80% of pixels within 0.94–1.07) |
| Three-case still camera, RMS / pixels > 4 levels | 0.025 / 0.000% | 0.025 / 0.000% |
| Three-case pixels > 10% off, moving camera / light ball | 1.256% / 0.928% | 1.224% / 0.887% |
| 180° snap, bright pixels per frame, static | 9.0, 9.0 | 3.0, 2.5 |
| 180° snap, bright pixels per frame, light ball | 23.5, 40.0 | 10.5, 7.5 |
| 180° snap, speck clusters, static / light ball | 9, 10 / 27, 28 | 6, 5 / 13, 10 |
| 30° sweep, speck clusters, static | 181, 186 | 184, 200 |
| 30° sweep, bright pixels per frame, static / light ball | 46.0, 45.0 / 45.2, 49.2 | 69.2, 69.6 / 59.1, 63.7 |
| 30° sweep, error against settled (RMS), static / light ball | 1.20 / 1.31 | 1.16 / 1.27 |

The R2 gate (glint clusters down 80%) is not met. Snaps improved by half to two thirds; sweeps have the same
cluster count, lower overall error, and more pixels above +24 levels. The wider young filter spreads a bright cell
over more probes at lower contrast.

### Tried and not kept

- **Cells without the cell filter**, levels integrated per probe in the prefilter. Snaps improved, but sweeps did
  not, and reflection glints at frame 1164 rose slightly (2,535). The glints were never the lobe estimator's heavy
  tail: they are a new probe's one frame of a few rays per direction, which the reflection lookup's
  per-pixel normal variation turns into specks.
- **Directional fill.** Young cells' missing weight took nearby samples (cos^8 about the cell's axis) instead of
  the probe's mean. Static sweeps came back to main's bright-pixel count, but light-ball sweeps got worse
  (85–88), and the still camera was no longer clean (RMS 0.040, 0.001% > 4 levels): probes that stay young use
  the fill every frame, and a local fill is noisier than the mean.

### What this means for R3

R3 rebuilds a probe without a predecessor from the previous frame's probes around it. The measurements above
narrow what it can add:

- The remaining young-surface noise is on surfaces revealed by the motion: the leading strip of a pan and
  disocclusions. In both, the previous frame had no probes on that surface, so there is nothing to rebuild from.
- Where previous probes on the same surface exist, probes are relocated and already take an 18-tap footprint
  history. Since R2, reflections also average neighbouring probes' cells up to 7 × 7.
- What R3 could still add is the footprint history for probes that have no predecessor but do have coplanar
  previous probes nearby. That is a small change to the existing relocation path, not a new reconstruction.

## R3 and R4 results (2026-11-27)

**R3, tried and not kept.** Probe placement only gives a probe a predecessor when a previous probe within 1.5
spacings has a normal within about 11° (cosine above 0.98). Otherwise the probe starts fresh. R3 let such a probe
adopt the best coplanar previous probe at any orientation as a relocation source, never as an anchor, so the
existing relocation path (normal-corrected diffuse, measured retention, world-direction reflection cells) set
how much history it kept. `crawl.py` separated R2 from R3:

| Build | Age 0–4, out / back | Age 4–8, out / back |
|---|---|---|
| main | 0.222 / 0.321 | 0.183 / 0.208 |
| R2 (atlas) | 0.166 / 0.307 | 0.117 / 0.181 |
| R2 + R3 | 0.165 / 0.304 | 0.119 / 0.180 |

R3 adds nothing measurable, as expected from surfaces revealed by motion having no previous probes. It was
reverted. Its gate (age 0–4 below 0.15 on both pans) is not met. The atlas, though, cut the youngest band's change
on the outward pan by a quarter and the 4–8 band by a third: a large share of the shimmer on newly revealed
surfaces was their reflections. A relocated probe now also takes a cell's history only if its source saw that cell
above its own horizon.

**R4.** The reflection history keeps only the atlas: `ZEN_SCREEN_PROBE_ATLAS_CELLS` (64) cells and the age word per
probe, instead of 112 texels and the age word. The three broad levels are integrated each frame in the specular
spatial filter and live only in its transient output. At 4K (162,000 probes, four history sides) this frees about
250 MB. The world cache still gets 1,048,576 cells. The crawl matches R2 (0.165 / 0.306), start-up is unchanged,
and a validation launch is clean.

**Review fix, not yet measured.** The predicted moving-light change reached only cells that drew a ray in that
frame, so parts of a moving light's reflection could trail by a frame. A cell without samples now takes the change
its nearby directions saw (inverse PDF times cos^8 about the cell's axis). It is zero without moving lights.
