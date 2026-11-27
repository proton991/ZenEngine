# Screen-probe memory in world space (plan, 2026-11-27)

## Problem

Measured in [GIDenoiserFinalFix.md](GIDenoiserFinalFix.md) ("Large-angle specks", stages 1–3 and item 1):

- During a 1° per frame pan, surfaces seen since the pan began have a pixel history at its 32-frame cap. Their
  16 × 16 block mean changes by 0.085 display levels per frame, which is not visible.
- Surfaces revealed in the last ~16 frames change by 0.16–0.23 levels per frame. During the ±30° pan
  they cover about a third of the screen. They are the visible motion noise.
- Fast-turn specks and rim glints are on the same young surfaces.

All of it starts the same way: a screen probe born on a revealed surface has one frame of 48 rays and no
history. Re-weighting those rays (stage 1) removes bias, not noise. Filtering more would blur. The fix
must give a new probe information it does not have now.

## How shipped engines handle it

- [AMD GI-1.0](https://ar5iv.labs.arxiv.org/html/2310.19855) and its shipped simplification
  [FidelityFX Brixelizer GI](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/brixelizer-gi/) pair
  screen probes with world-space caches. Brixelizer GI projects each screen probe's incoming radiance
  onto spherical harmonics and stores them per world brick. Its world cache is "stable and persistent",
  while screen probes "can be disoccluded with motion".
- [Lumen](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)
  resolves distant lighting through world-space radiance-cache probes that trace more rays, so screen
  probes inherit their low noise.
- Neither source documents how a newly disoccluded screen probe is filled.

## What ZenEngine already has

- The world radiance cache (`world_cache.glsl`) hashes surface cells by distance-adaptive position, level and
  normal bin. Each cell stores six directional records of incoming irradiance, filled by cache paths, also
  off screen. Probe rays read it at their hits. The records include direct light from scene lights,
  which screen probes leave to per-pixel lighting, so they cannot seed a probe directly.
- The optional `worldCacheSeed` reads a scalar, indirect-only seed record along the shading normal. It seeds
  only the bounce signal, at most one batch, and lowered young-probe RMS by 3% (rejected).

## Design

The screen probes' own converged state is written into a world-space table. A probe without history adopts
the matching entry as its history. The representation is the probe's own, so the semantics match exactly:
sky and settled bounce, each as RGB L1 spherical harmonics plus irradiance and batch weight.

### Table

- A separate open-addressed hash table, sized independently of the world cache: 2^18 entries of 80 bytes,
  21 MB. Its bytes count as fixed bytes in the world cache planner, so the planner sees them.
- Key: the world cache cell key at the probe, `CacheCellKey(position, geometric normal, WorldLevel(position))`.
  An entry then corresponds to the world cache cell whose change evidence tracks light edits at that spot.
- Entry: the full key, the frame of its last write, the last-global-change frame it was written after, its
  accumulated weight in batches, a representative position (plane check), and for sky and bounce the RGB L1
  SH and irradiance in FP16.
- Probing: four slots, as the world cache. A full neighbourhood replaces its oldest entry.

### Write (existing pass, `ScreenProbeSpatialHorizontal`)

- After integration, each probe with enough weight (at least eight frames of batches) and no held change
  evidence writes its sky and bounce state into its entry.
- One writer per entry per frame: the first probe that swaps the entry's frame stamp writes, so there are no
  torn entries. Over frames, different probes in the cell write, so the entry averages them.
- Blend: an exponential average over at most 16 writes, so the entry follows slow changes and forgets an
  outlier probe.
- Moving-light signals are not stored. They have their own history.

### Read (existing pass, `ScreenProbeTemporal`)

A probe with no predecessor reads its entry before the temporal blend and treats it as history. So does
a relocated probe, for the share its footprint does not support. It is accepted when:

- the key matches and the representative position lies within half a cell of the probe's plane;
- no global lighting change, environment or geometry revision happened since it was written;
- the world cache cell at the key holds no change evidence (the same test as the old seed).

Its weight is its accumulated batches, capped at eight frames' worth, so the probe's own rays take over
within a few frames if the entry was wrong (a leak or a missed edit).

### Reflections (stage W2)

A new probe's specular texels fill their missing weight (stage 1) from the entry's SH, evaluated in the
texel's direction, instead of the probe's hemisphere mean.

## Limits

- Only surfaces seen before benefit. A surface revealed for the first time still starts from one frame
  of rays. Lumen-style world radiance probes would cover that case and are a possible later stage.
- One entry per cell and normal bin can mix lighting across an occluder inside a cell. The plane check,
  the eight-frame weight cap and the probe's own rays bound that.

## Passes, memory and cost

- No new pass: the write goes into the horizontal spatial filter and the read into the temporal pass.
- 21 MB, counted by the world cache planner.
- Per probe, one hash lookup and one 80-byte write when mature; reads only for probes without history.

## Stages and measurements

| Stage | Change | Measured with |
|---|---|---|
| W1 | Table, write, diffuse read for new and relocated probes | `crawl.py` young-band change, three-case, large-angle return frames |
| W2 | Specular fill from the entry | Reflection diagnostic at frame 1164, large-angle glints |
| W3 | Remove the superseded `worldCacheSeed` path | Settings tests, pass list |

Gates: the young-band block change under 0.1 level per frame for surfaces seen before. No darkening in
the large-angle −24 counts. Mature surfaces unchanged (0.085 levels per frame). The world cache keeps
1,048,576 cells at 4K with the light ball.

## W1 results (2026-11-27)

W1 was built as planned, except that the read and the write both live in the temporal pass. A per-entry
frame stamp guards them: one writer claims an entry, and a reader skips an entry that changed while it was
read. No pass was added, and the world cache kept 1,048,576 cells in every launch. The pass-list test passes.
Debug colouring confirmed that probes on surfaces revealed by a return pan adopt entries.

Three-case check (1° per frame pan, pixels more than 10% off):

| Variant | Moving camera | Light ball | Still camera |
|---|---|---|---|
| Stage 1 (no table) | 1.297% | 0.977% | clean (RMS 0.025) |
| Entry trusted as 8 frames, 3-spacing cells | 1.172% | 0.853% | pops (RMS 0.039) |
| Entry rejected beyond 3 standard errors of the probe's own frame | 1.276% | 0.946% | clean |
| Entry weighted by inverse variance (cell spread against frame noise) | 1.280% | 0.945% | clean |
| Same, 1-spacing cells | 1.280% | 0.956% | clean |

Fixes found on the way, kept in every variant after they appeared:

- Cells near local lights. The first variant leaked lantern light: inverse-square falloff varies across a
  cell next to a lamp, and the large-angle static case went from 133 to 1,383 speck clusters. Probes within
  eight cells of a local light now neither read nor write.
- Normals. A cell's normal bin admits normals 55° apart, while an entry's irradiance holds at its writers'
  normal. Entries now keep their writers' mean normal and are read only within about 25°.

Outcome: trusting an entry as eight frames gains about 10% but makes a still camera pop. A new probe now and
then adopts the mean of a cell whose spots differ, and the pixel jumps several-fold before its own rays take
over. Weighting the entry by how much its cell's spots differ removes the pops and the gain. An entry then
counts for about one frame, because neighbouring spots differ about as much as one frame's sampling noise.
Smaller cells keep entries precise, but a new probe rarely lands in a cell an earlier probe wrote, since
placement shifts with the camera.

This matches why shipped engines keep world-space caches for distant lighting, which varies slowly across
space, and trace near-field lighting fresh. W1 does not meet its gate, and W2 would read the same entries.

## More first-frame rays (2026-11-27)

As an experiment, brand-new probes (no predecessor) traced six batches instead of three. The world cache kept
1,048,576 cells. Measured with `crawl.py` (1° per frame pan out to 30° and back), 16 × 16 block change per
frame by pixel history age:

| Pan | Build | Age 0–4 | Age 4–8 | Age 8–16 | Blocks > 1 level, age 0–4 |
|---|---|---|---|---|---|
| Out, first visit | 3 batches, stage 1 | 0.221 | 0.178 | 0.147 | 0.64% |
| Out, first visit | 3 batches, W1 table (inverse-variance) | 0.231 | 0.186 | 0.157 | 0.75% |
| Out, first visit | 6 batches, stage 1 | 0.208 | 0.188 | 0.156 | 0.60% |
| Back, revisit | 3 batches, stage 1 | 0.315 | 0.203 | 0.160 | 1.73% |
| Back, revisit | 3 batches, W1 table (inverse-variance) | 0.283 | 0.197 | 0.161 | 1.33% |
| Back, revisit | 6 batches, stage 1 | 0.295 | 0.197 | 0.158 | 1.50% |

The experiment was first compared with the W1 build, the last one measured on the return pan. The plain
stage 1 rows were measured afterwards, with the 128-frame start-up warm-up of
[GIDenoiserFinalFix.md](GIDenoiserFinalFix.md). A cold start is already settled by frame 256 (0.03% of blocks
more than 5% off), so the warm-up does not change a pan that starts at frame 600. Against them,
doubling the first frame's rays lowered the youngest band's change by about 6% on both pans. If first-frame
sampling noise dominated, it would have fallen by about 30%. On revisits, the W1 table gained about 10% in
this band, as on the three-case check. The change on
young surfaces therefore comes mostly from elsewhere: probes keep being placed and relocated as the camera
moves, and those pixels' history is still short. On mature surfaces the 32-frame pixel history hides the
same re-placement (0.085 levels per frame). A pool of extra rays for new probes was not built, and the
experiment was reverted.
