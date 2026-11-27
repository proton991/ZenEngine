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
