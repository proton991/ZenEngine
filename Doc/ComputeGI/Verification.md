# G1 verification — 2026-10-22

The sparse brick scene is usable through `voxel_gi_compute_traversal=bricks` and
`voxel_gi_brick_resolution=64|128|256`, with per-pixel gather and the existing hit-radiance cache.
Defaults remain triangles and 128 brick cells per axis. The results below predate the G3 screen-probe
implementation and do not validate its shaders, integration, or changed settings expectations.

## Brick traversal and lamp bounce — 2026-10-25

New Sponza lit by its own 24 lights looked right in direct light but was too dark in bounce on the
compute bricks path. Against the CUDA one-bounce reference (`tools/ground_truth_bounce.py`, 4096
samples, uniform 0.5-albedo copy of the scene, `PhysCamera001`, 480×270; the engine captured 64
frames), direct sunlight matched the analytic value on every path (median ratio 0.999), while
bounce was:

| Bounce bias against CUDA, all / floor | All 24 lights | Sun only | Lamps only |
| --- | ---: | ---: | ---: |
| Hardware, per-pixel rays | −6.1% / −9.3% | −3.8% / −2.0% | −10.7% / −17.8% |
| Compute triangles, probes | −11.6% / −2.2% | −7.7% / −1.1% | −17.4% / −3.1% |
| Compute bricks, per-pixel rays | −18.6% / −44.3% | −4.1% / +3.9% | −41.8% / −85.0% |
| Compute bricks, probes | −26.4% / −41.4% | −8.1% / +6.4% | −55.1% / −80.4% |

The sun/lamp reference split is the stored `bounce-fix/cuda/split.npz` (1024 samples); the
engine captures were sun-only and lamps-only scene copies. Only lamp light was lost, and less of it
at finer bricks (lamps, per-pixel rays, floor: −93% at 64 cells, −85% at 128, −58% at 256). The
CUDA analysis from the hardware fix found 91% of the floor's lamp bounce on secondary hits within
25 cm of a lamp: lantern interiors seen through openings centimetres wide, which 7 cm fine voxels
close.

Experiments that ruled out other causes, lamps only, bricks with per-pixel rays:

- Stopping near-lamp visibility rays two fine voxels short of the lamp (the bulb's voxels blocking
  them): floor −84.5%, unchanged.
- Accepting any surface class present in the hit's cell when the brick face's class is absent: the
  rejected lookups fell from 3.7% to 1.9% of bounce rays, the floor stayed at −84.9%.
- Re-tracing brick hits within 1.5 class cells of a lamp against triangles: floor −29%; with
  triangle visibility rays as well, −21% (probes: −14%, then −6%).

**Fix.** A diffuse bounce ray whose brick hit lies within one class cell of a cached point or spot
light is re-traced against triangles, starting one fine voxel before the hit (bricks are
conservative, so no triangle lies closer), and visibility rays from re-traced hits to near lights
trace triangles. Both ignore alpha masks. The re-trace follows triangles for two class cells, then
bricks again from mid-air. Brick trace passes queue these rays and a separate pass re-traces them
(below). Reflection rays are not re-traced. Re-trace radius, bricks with probes, all lights, with
the first, inline version: 0.5 cells −26.2% / −42.6%, 0.75 −18.6% / −24.9%, 1 −11.4% / −5.1%, 1.5
−11.1% / −3.5%.

| Bounce bias against CUDA, all / floor | Before | After |
| --- | ---: | ---: |
| Bricks, probes, all lights | −26.4% / −41.4% | −11.4% / −5.1% |
| Bricks, probes, lamps | −55.1% / −80.4% | −16.4% / −14.4% |
| Bricks, probes, sun | −8.1% / +6.4% | −8.1% / +6.4% |
| Bricks, per-pixel rays, all lights | −18.6% / −44.3% | −6.8% / −6.3% |
| Bricks, per-pixel rays, lamps | −41.8% / −85.0% | −12.2% / −17.1% |
| Bricks, probes, all lights, 64 / 256 cells | — | −12.6% / −5.5%, −11.8% / −0.9% |

Bricks now match the triangle paths, which the change does not touch (triangles with probes and
hardware per-pixel rays measured the same before and after). The "After" column held unchanged
through the move to a separate pass: per-pixel all lights −6.8% / −6.3% and lamps −12.2% / −17.1%,
probes −11.5% / −5.1% and −16.5% / −14.4%.

**Cost** (New Sponza, textured, 1600×900, full-resolution per-pixel trace, RTX 5080, still camera,
two runs each):

| | Before | Inline re-trace | Separate pass |
| --- | ---: | ---: | ---: |
| Bricks + probes: `ProbeTrace` | 0.98 ms | 1.30 ms | 1.04 ms |
| Bricks + probes: frame | 17.5 ms | 18.4 ms | 17.9 ms |
| Bricks, per-pixel rays: `HybridTrace` | 13.0 ms | 16.6 ms | 13.4 ms |
| Bricks, per-pixel rays: `HybridRetrace` | — | — | 0.33 ms |
| Bricks, per-pixel rays: frame | 31.7 ms | 35.5 ms | 32.6 ms |

The first, inline re-trace ran inside the brick trace shaders. Dropping the alpha test from its
triangle rays cut the probe trace's increase from 0.82 to 0.31 ms; it did not help the per-pixel
trace. Starting the re-trace near the brick hit instead of at the ray's origin saved 1.7 ms there,
and bounding its triangle segment to two class cells another 0.8 ms (16.6 → 15.8 ms): a ray through
a lantern's openings had crossed the room against every triangle. With the re-trace compiled in but
never taken, `HybridTrace` took 14.0 ms: about 1 ms came with compiling it in, and the remaining
1.8 ms was the re-traced rays themselves. The separate pass re-traces them in 0.33 ms.

Brick trace passes now queue those rays (`hybrid_retrace.glsl`, `probe_trace.glsl`) and carry no
triangle traversal; their near-light visibility rays trace bricks. Per-pixel, each thread chains its
receiver's queued rays and lists the receiver once; `hybrid_retrace_bricks.comp` re-traces them,
one receiver per thread, and adds them to the raw images, or at half resolution to the half images
before the upsample, with a second queue after the fallback pass. Probes queue rays per slot;
`probe_retrace_bricks.comp` re-traces one ray per thread and the probe temporal pass adds them to
their probes (its pass time overlaps the temporal pass, so the frame time is the measure). Queues
hold one ray per two traced receivers, one per eight pixels for the fallback pass, and eight per
probe slot; further rays are shaded from their brick hit. At half resolution
(`voxel_gi_diffuse_resolution=half`) the frame took 25.9 ms, `HybridRetrace` 0.47 ms and
`HybridFallbackRetrace` 0.002 ms; that path was not measured before the change.

**Reference accumulation and ray counts.** Reference accumulation traces 256, 1024 or 4096
diffuse rays per receiver and frame, more than the queue holds and, at first, more than the
re-trace pass walked per receiver (64): the trace left every queued ray out, and those the pass
dropped or the queue refused were lost or shaded from bricks. Its brick trace variants
(`hybrid_trace_reference*_bricks.comp`) now re-trace inline, with no queue, and the re-trace pass of
other frames walks every queued ray. New Sponza lamps only, bricks, per-pixel reference at 256
samples, against CUDA: −47.3% / −82.6% before, −7.4% / −10.8% after. Ray-count captures re-trace
with a counting variant (`hybrid_retrace_counts_bricks.comp`) that adds its rays to the bounce
count. PhysCamera001 at 960×540, Compute with bricks: 0.055 bounce rays per pixel, of which 0.036
come from the re-trace pass and were missing before; the other counts did not change.

| Check | Result | Local evidence |
| --- | --- | --- |
| ConeVoxelGI integration, including the re-trace chain walk past 64 rays (64 of 200 with the old bound) | 76 passed; 4 FBX asset tests fail outside rendering | `build/compute-gi/retrace-fix-tests2.log` |
| Full RenderCoreTest, including brick reference frames (inline, no queue) and ray-count frames (counting re-trace) | 726 passed, no leaks | `build/compute-gi/retrace-fix-tests2.log` |
| RayQuery integration, including the software query oracle | 20 passed | `build/compute-gi/retrace-fix-tests2.log` |
| Shader reflection | 23 passed | `build/compute-gi/retrace-fix-tests2.log` |
| UIDrawPacket | 15 passed | `build/compute-gi/retrace-fix-tests2.log` |
| Editor rendering, preview, UI rendering, runtime UI | 53, 16, 5, 10 passed | `build/compute-gi/retrace-fix-tests2.log` |

Evidence in `build/compute-gi/newsponza-gt/`: `bounce_split.py` (captures and compares one
configuration), `compare.py`, `show_reference.py`, `reference-vs-captures.png`, and the captures
in `split/`. The timings are the `nearlamp-*` entries of `build/compute-gi/sponza/report.json`.
Each capture of a scene copy imports a 5.2 GB cache into `ZenEngine-gi/Cache/SceneAssets`; delete
those afterwards.

## Screen probes while the camera moves — 2026-10-24

Moving the camera made screen-probe lighting shimmer in tile-sized, water-like patches and lose
occlusion contrast. Each uniform probe sat at a fixed pixel of its tile, so while the camera moved
it traced from a different surface point every frame. Its history, blended from the 2×2 previous
probes as if they sat at tile centres, averaged light along that trail.

**Fix.** Probes stay at their world positions while they remain visible (AMD GI-1.0 also reuses
probes this way):

- `probe_place.comp` keeps last frame's uniform probe that now projects into the tile onto the same
  surface (receiver identity, plane, normal). When several land in one tile, the longest history
  wins, then the one nearest the tile centre. Other tiles take a new probe from the fixed candidates.
- `probe_adaptive.comp` gives priority to receivers under last frame's adaptive probes that still
  project onto their surfaces.
- `probe_temporal.comp` gives a kept probe its own record alone with four times the sky history,
  moving or still. A new probe blends the previous probes around its reprojection, weighted by
  where they actually were.

The first build reprojected world positions with the hybrid reconstruction matrix, which maps
positions relative to `worldOrigin` (the near-plane centre). Probes landed a median 775 pixels from
their own tile, and only about 2% were kept, even with a still camera. With the origin subtracted, a
still camera keeps 99.9% of uniform probes and reprojects them within 0.5 pixel (0.7 after turning).
While moving, about 60% of tiles keep last frame's probe per frame when strafing and 76% when
turning.
The placement test now uses a non-zero origin.

**Method.** `scene_renderer_demo --capture-camera-motion=<prefix>`, glTF Sponza at 2870×1579,
environment light only, Brick + Probe, RTX 5080. From a view settled for 63 frames the camera
strafes by 0.001 of the scene bounds per frame or turns by 0.2° per frame for 30 frames, two
consecutive moving frames are captured, and each of those views is captured again after 63 still
frames. Frame-to-frame change matches pixels through their world positions. A tile is 8×8 sampled
pixels (one 16-pixel probe tile at stride 2) and averages out per-pixel noise. The reference is the
per-pixel gather settled at the same pose.

Sky light, the signal probes carry, before (probes at fixed pixels), after (kept probes) and with
the per-pixel gather:

| Sky light | Strafe: before | after | per-pixel | Turn: before | after | per-pixel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tiles changing by over 1% per frame | 41.0% | 37.6% | 35.0% | 38.6% | 28.7% | 29.0% |
| Tiles changing by over 2% per frame | 25.3% | 23.5% | 21.3% | 21.0% | 14.6% | 15.3% |
| Moving frame against its settled view, mean tile difference | 16.6% | 9.3% | 3.1% | 20.3% | 6.9% | 3.0% |
| Moving tiles over 10% off the reference | 51.7% | 40.9% | 5.3% | 55.8% | 36.1% | 5.9% |
| Settled tiles over 10% off the reference | 33.1% | 33.1% | — | 33.6% | 31.8% | — |

Final diffuse lighting (sky and bounce through the material):

| Diffuse | Strafe: before | after | per-pixel | Turn: before | after | per-pixel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Tiles changing by over 1% per frame | 39.3% | 37.2% | 36.8% | 39.1% | 34.0% | 34.5% |
| Moving frame against its settled view, mean tile difference | 8.2% | 4.7% | 1.8% | 10.0% | 3.3% | 1.2% |
| Moving tiles over 10% off the reference | 31.9% | 23.2% | 2.8% | 38.5% | 18.7% | 1.0% |
| Settled tiles over 10% off the reference | 14.0% | 13.8% | — | 15.6% | 14.3% | — |

Frame-to-frame shimmer while moving is now at the per-pixel gather's level, and the gap between a
moving frame and its settled view shrank by a half (strafe) to two thirds (turn). Converged quality
is unchanged. While turning, moving frames were about 2% darker on average than settled ones
(sky ×0.978, diffuse ×0.981); now ×0.997 and ×1.000. Bounce follows: the mean tile difference
against the settled view went from 5.9% to 3.6% (strafe) and 7.8% to 2.8% (turn).

The per-pixel gather still tracks the settled view more closely while moving. World-locked probes
drift unevenly across tiles, leaving some tiles two and others none, and each extra probe ended
while the empty tile started a new one. The next section halves that churn.

| Check | Result | Local evidence |
| --- | --- | --- |
| ConeVoxelGI integration, with kept-probe placement and history tests | 72 passed, no leaks | `build/compute-gi/p13-cone.log` |
| Full RenderCoreTest | 700 passed, no leaks | `build/compute-gi/p13-tests.log` |
| RayQuery integration | 20 passed | `build/compute-gi/p13-tests.log` |
| Shader reflection | 22 passed | `build/compute-gi/p13-tests.log` |
| UIDrawPacket GI tracing and lighting backend | 7 passed | `build/compute-gi/p13-tests.log` |

The SH reconstruction test compared a new probe's history length exactly; that length is now a
weighted blend of several neighbours (3.99999976 instead of 4), so it allows float rounding. The
first ConeVoxelGI run in `p13-tests.log` shows those four failures; `p13-cone.log` is the rerun.

Evidence: `build/compute-gi/motion/` holds `report.txt` (frame-to-frame change and lag),
`truth.txt` (against the settled per-pixel gather), `bias.txt`, `sky-error-yaw.png` (sky tile error
against the reference while turning: before, after, after settled; red brighter, blue darker,
full colour at 25%), the capture logs and the scripts. The per-frame extracts (about 40 MB each)
were not kept; `motion_flicker.py capture <dir> <label> voxel_gi_gather=probes|pixel` recreates them.

## Probe churn while the camera moves — 2026-10-24

With kept probes, about one uniform tile in five still started a new probe every frame while the
camera moved, strafing or turning. A temporary capture diagnostic (removed afterwards) traced why
last frame's probes ended: almost all reprojected validly onto their own surface but lost their
tile to a probe with a longer history, while a neighbouring tile had none and started fresh.

**Change.** Placement runs in two passes. `probe_claim.comp` records per tile the previous probe it
keeps (the earlier rule), or whether it has a receiver. `probe_place.comp` then gives each free tile
a displaced probe: one that now projects into a neighbouring tile which kept another, whose nearest
free neighbouring tile (by centre distance) is this one, and which lies on the surface a new probe
here would take. Every tile derives a displaced probe's destination from the same claims, so no
probe is kept twice. About 10% of tiles then hold a probe just outside their bounds, which the 3×3
interpolation still reaches.

Same capture and reference as the previous section. "New" counts tiles whose probe matches none of
last frame's probes; "young" counts probes with at most 32 frames of history.

| | Strafe: kept | with adoption | Turn: kept | with adoption |
| --- | ---: | ---: | ---: | ---: |
| Tiles with a new probe per frame | 20.4% | 11.3% | 21.4% | 10.2% |
| Tiles with a young probe | 41.5% | 35.9% | 24.1% | 13.7% |
| Sky tiles changing by over 1% per frame | 37.6% | 37.5% | 28.7% | 29.5% |
| Sky, moving frame against its settled view, mean tile difference | 9.3% | 9.0% | 6.9% | 6.6% |
| Sky tiles over 10% off the reference, moving | 40.9% | 40.2% | 36.1% | 35.3% |
| Diffuse tiles over 10% off the reference, moving | 23.2% | 22.7% | 18.7% | 18.3% |
| Sky tiles over 10% off the reference, settled | 33.1% | 32.8% | 31.8% | 31.9% |

Halving the churn moved quality by under a point, because churn no longer limits it. Splitting the
moving frame's sky tiles by their probe's history:

- **Strafe.** Tiles with a long history (70% of the view) are as accurate as settled ones: 31.8%
  are more than 10% off the reference, against 32.8% settled. Young tiles (30%) are 60% off. Most
  of them are the near wall entering the view, whose probes are kept but only a few frames old.
- **Turn.** Long-history tiles are 34.6% off, young tiles (10.5%) 41.5%. Removing the remaining
  churn entirely would gain about one point.

So the gap to the settled view now comes from content entering the view, where a probe gathers 64
rays per tile per frame and the per-pixel gather about 1,000. With adoption, the previous probes
that still end are: 7.6% (strafe) and 9.2% (turn) that lost their tile and were not adopted (no
free neighbour, another displaced probe took it, or its surface differed; the diagnostic did not
separate these); 3.2% (strafe) occluded, failing the plane or normal test; under 1% off screen.
Placing new probes at tile centres, so that probes cross tile boundaries together, did not reduce
churn (10.6% and 10.8%) and was not kept.

The claim pass costs 0.026 ms and placement 0.006 ms (New Sponza, 1600×900, Brick + Probe, still
camera, RTX 5080); a free tile's search for a displaced probe only runs while the camera moves.

| Check | Result | Local evidence |
| --- | --- | --- |
| ConeVoxelGI integration, with displaced-probe placement test | 72 passed, no leaks | `build/compute-gi/p14-tests.log` |
| Full RenderCoreTest, with the claim pass in the dispatch list | 700 passed, no leaks | `build/compute-gi/p14-tests.log` |
| RayQuery integration | 20 passed | `build/compute-gi/p14-tests.log` |
| Shader reflection | 22 passed | `build/compute-gi/p14-tests.log` |
| UIDrawPacket GI tracing and lighting backend | 7 passed | `build/compute-gi/p14-tests.log` |

Evidence in `build/compute-gi/motion/`: `churn.txt` (the diagnostic's counts),
`report-adopt.txt`, `truth-adopt.txt`, `bias-adopt.txt`, `adopt.log` and `motion_diag.py`.
The timing is `adopt-probe16` in `build/compute-gi/sponza/report.json`.

## Extra rays for young probes — 2026-10-24

The churn measurements showed that, while the camera moves, the remaining gap to the settled view
is young probes: on content entering the view a probe gathers 64 rays per tile per frame, where
the per-pixel gather takes about 1,000. Young probes now trace more ray batches, and history counts
batches, so they reach a long history in fewer frames:

- `probe_trace.glsl` traces the placement's batch count (`screen.w`), up to four batches of 64
  stratified rays with fresh jitter, and averages them.
- `probe_temporal.comp` adds that many frames of history and weighs the sample by it.
- The count (`ProbeRayBatches`): a kept probe traces four batches under 8 frames of history, two
  under 24, otherwise one. A new uniform probe traces four, a new adaptive probe one. Without
  valid history every probe traces one, so geometry changing every frame, which resets history
  every frame, does not quadruple the trace.

Four variants, measured on the moving-camera capture of the previous sections. Trace time is the
median `ProbeTrace` while moving minus while settled within the same run (glTF Sponza, 1600×900,
RTX 5080; the demo profiled the capture's frames for this measurement only), because the settled
median itself varied by 0.2 ms between runs:

| Extra batches for | Strafe sky | Strafe diffuse | Turn sky | Turn diffuse | Extra trace while moving |
| --- | ---: | ---: | ---: | ---: | ---: |
| None (previous section) | 40.2% | 22.7% | 35.3% | 18.3% | about 0 |
| Young kept probes | 38.7% | 20.9% | 34.9% | 18.0% | +0.3 ms |
| **Young kept and new uniform probes (shipped)** | **36.6%** | **18.7%** | **34.3%** | **17.2%** | **+0.5 ms** |
| Young kept and all new probes | 36.4% | 18.6% | 34.0% | 16.9% | +1.0 to +1.4 ms |

The columns are the share of tiles more than 10% off the settled per-pixel gather on the moving
frame; settled frames stay at 32.7% (strafe) and 31.8% (turn) for sky in every variant. Four
batches for every new adaptive probe doubled the trace while moving for almost no gain, because
most new probes inherit a long history from their neighbours.

With the shipped variant, against no extra rays:

| | Strafe: before | after | per-pixel | Turn: before | after | per-pixel |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Sky tiles changing by over 1% per frame | 37.5% | 34.2% | 35.0% | 29.5% | 30.0% | 29.0% |
| Sky tiles changing by over 2% per frame | 23.1% | 18.2% | 21.3% | 15.1% | 15.2% | 15.3% |
| Sky, moving frame against its settled view, mean tile difference | 9.0% | 6.8% | 3.1% | 6.6% | 6.2% | 3.0% |
| Diffuse, the same | 4.5% | 3.4% | 1.8% | 3.2% | 2.9% | 1.2% |
| Sky tiles over 10% off the reference, moving | 40.2% | 36.6% | 5.3% | 35.3% | 34.3% | 5.9% |
| Diffuse tiles over 10% off the reference, moving | 22.7% | 18.7% | 2.8% | 18.3% | 17.2% | 1.0% |

Strafing benefits most, since most of its young probes are on the near wall entering the view;
while strafing, probe sky light now changes less from frame to frame than the per-pixel gather's.
Frames grow by about 0.4–0.5 ms while moving (5.2 to 5.6–5.8 ms) and not at all when still.

**After a history restart.** As first shipped, every probe was young right after a restart
(startup, a camera cut), so the trace took 4.0–4.4 ms for two frames and about 2.1 ms for eight
more, against 1.1 ms settled, and those two frames took 8.2 ms against 5.2 ms settled. Now only a probe clearly younger
than the view traces extra batches: its history must be more than two frames shorter than the
frames since the probe history restarted, which `ScreenProbeGather` counts per submitted frame and
passes in `probe.parameters.w`. After a restart every probe is as old as the view and traces one
batch, so it converges at the single-batch rate as before this change; content that enters the
view later is younger than the view and still traces more.

| `ProbeTrace`, glTF Sponza 1600×900 | Before the cap | After (two runs) |
| --- | ---: | ---: |
| First two frames after startup | 4.02, 4.43 ms | 1.37, 1.08 / 1.36, 1.08 ms |
| Next eight frames | about 2.1 ms | 1.1–1.6 ms |
| Moving, minus settled, strafe / turn | +0.50 / +0.52 ms | +0.61 / +0.33, +0.52 / +0.35 ms |

Moving quality is unchanged: strafe sky tiles over 10% off 36.8% (36.6% before the cap), diffuse
18.7% (18.7%); turn 34.5% (34.3%) and 17.2% (17.2%). A large camera jump that stays under the
cut threshold keeps history, so it is not capped: the capture's jump back to the start pose
before turning starts many new uniform probes at four batches, and the trace took 2.0–2.4 ms for
two frames, before and after the cap.

| Check | Result | Local evidence |
| --- | --- | --- |
| ConeVoxelGI integration, with batch-weighted history and placement batches at two view ages | 76 passed, no leaks | `build/compute-gi/p16-tests.log` |
| Full RenderCoreTest | 700 passed, no leaks | `build/compute-gi/p16-tests.log` |
| RayQuery integration | 20 passed | `build/compute-gi/p16-tests.log` |
| Shader reflection | 22 passed | `build/compute-gi/p16-tests.log` |
| UIDrawPacket GI tracing and lighting backend | 7 passed | `build/compute-gi/p16-tests.log` |

`p16-tests.log` holds the final run; an earlier run in the same session tested a stale
ConeVoxelGI binary after a transient editor link lock (`p16-all-build.log`) and is superseded.

Evidence in `build/compute-gi/motion/`: `report-young.txt`, `truth-young.txt` (the variants
against the reference), `bias-young.txt`, `young.log`; for the cap `report-capped.txt`,
`truth-capped.txt`, `bias-capped.txt`, `capped.log`, `profile-capped.txt` and
`profile-startup.txt`; `profile.txt` with the per-run `profile/*.csv`, and `motion_profile.py` /
`motion_profile_report.py`. The profiled runs used a temporary demo change that keeps profiling
through `--capture-camera-motion`; it was removed.

## Direct shadow cost on the compute path — 2026-10-23

On New Sponza with Brick + Probe (1600×900, RTX 5080), direct lighting with Compute shadow rays costs
about 12.6 ms (`SceneLighting` 9.3 + `ForwardTranslucent` 3.3) of a 15.8 ms frame. With HW shadow
rays the same passes take 4.1 ms, so software triangle traversal accounts for about 8.5 ms. Every
receiver traces one ray to each of the 23 shadowed lights: the sun and 22 lamps, most of them behind
walls. Two config probes did not change anything. `voxel_gi_shadow_enabled=false` repeats a key the
base config already sets, and `ConfigLoader` keeps the first occurrence. `voxel_gi_rt_shadows=false`
loaded, but New Sponza stays on ray shadows.

**Brick-traced shadows** (`voxel_gi_shadow_traversal=bricks`, default `triangles`). **Removed on
2026-10-24.** In the editor the voxel-scale shadow edges, leaks along moldings and over-occlusion by
alpha-masked foliage read as poor shadow quality, and on glTF Sponza Software BVH shadows showed no
noticeable frame-rate drop. The 1.4 ms saving on New Sponza did not justify keeping a second shadow
tracer: Compute direct shadows trace triangles, and the shaders, setting and panel choice are gone.
The record below is kept for reference.

- **Shaders.** Eleven new variants mirror the software-shadow ones: deferred plain, visibility,
  capture and counts, and forward material and scatter. They trace `BrickTraceRay` from the receiver
  with the GI rays' origin offset. The brick scene sits at set 4 bindings 5–8, past the lighting
  capture (2) and receiver inputs (3, 4).
- **Brick scene.** Shared with GI. It is built when either GI gather or Compute shadows request it, and
  `UsesBrickTraversal` and `UsesBrickShadows` report the two uses separately. The voxel radiance
  injection keeps triangles.
- **Settings and diagnostics.** Presets pin triangles. There is an editor and runtime combo, and
  captures record the requested and effective shadow traversal.

Lamp fixtures decided the design. Voxelized, a lantern's frame encloses its lamp: with plain brick
rays several lamps went dark, and 24% of covered pixels changed their direct light by more than
half. Skipping a larger region near the light leaked through the walls the sconces hang on (+155% direct
energy). The shipped variant traces bricks up to the last 1/64 of the grid extent before a local
light (16 fine voxels at 128 cells) and triangles through it.

Measured in one session, Brick + Probe. Direct-light quality is the `analytic_direct` capture
component against HW shadow rays, which match software triangles exactly. "Flips" are covered pixels
whose direct light differs by more than half (and is at least 1% of the mean).

| Shadow rays | GPU frame | `SceneLighting` | `ForwardTranslucent` | Direct energy | Direct L1 | Flips | Final frame mean \|d\| |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Software triangles | 15.71–15.79 ms | 9.26–9.28 | 3.32–3.35 | 0 | 0 | 0 | 0 |
| Bricks 128, plain | 12.17–12.41 ms | 7.16 | 2.21 | −4.2% | 11.7% | 23.6% | 0.68/255 |
| Bricks 256, plain | 15.89 ms | 9.56 | 2.70 | −2.5% | 6.8% | 16.2% | 0.49/255 |
| Bricks 128, fixture region skipped (4×, 8× offset) | 11.68–11.83 ms | — | — | +155%, +160% | — | 85% | 9.0/255 |
| **Bricks 128, triangles through the fixture (shipped)** | **14.29–14.35 ms** | 8.97 | 2.30 | −0.06% | 6.7% | 3.5% | 0.33/255 |
| HW RT shadow rays | 7.11 ms | 2.87 | 1.21 | 0 | 0 | 0 | 0 |

The shipped brick shadows save 1.4 ms (9% of the frame), mostly in the forward translucent pass. The
triangle segment through each lamp fixture costs nearly what bricks save in the deferred pass: plain
brick rays were 1.6× faster than the software BVH, and 256 cells cost as much as triangles. The
remaining differences are voxel-scale shadow edges, small leaks along moldings and over-occlusion by
alpha-masked foliage. On the editor shadow fixture the directional result is pixel-identical; the
point light's brick shadow removes 7% less light than the triangle shadow, and 86 of 65,536 pixels change.

**Fewer rays instead (experiment, not shipped).** Most of the cost is the ray count, not the
traversal. A shader experiment traced shadow rays only for lights carrying at least a fraction of
the receiver's unshadowed direct light. The rest counted as visible or as shadowed:

| Skipped lights count as | Threshold | GPU frame | Direct energy | Flips | Final frame mean \|d\| |
| --- | ---: | ---: | ---: | ---: | ---: |
| Visible | 1% / 3% / 10% | 12.34 / 10.40 / 8.61 ms | +36% / +69% / +96% | 76–91% | 3.0–7.5/255 |
| Shadowed | 1% / 3% | 12.32 / 10.48 ms | −0.5% / −2.5% | 9.0% / 15.4% | 0.05 / 0.25/255 |
| Shadowed, with brick shadows | 1% | 10.74 ms | −0.6% | 10.9% | 0.37/255 |

The selection uses unshadowed contributions, and in New Sponza most lamps are occluded. Counting the
skipped lights as visible floods the image. Counting them as shadowed is close on average but
removes the faint direct light from regions where a distant lamp was the only source. Neither
estimate is shippable as is. A ray-count reduction needs visibility it can trust: reused receiver
visibility (half-resolution or temporally reprojected per-light masks, which
`RECEIVER_LIGHT_VISIBILITY` already writes) or stochastic light selection with denoising.

| Check | Result | Local evidence |
| --- | --- | --- |
| Focused RenderCore, with shadow-traversal settings tests | 24 passed, no leaks | `build/compute-gi/p5-cpu.log` |
| Full RenderCoreTest | 699 passed, no leaks | `build/compute-gi/p5-rendercore.log` |
| Shader reflection, with brick-shadow binding test | 23 passed | `build/compute-gi/p5-reflection.log` |
| Editor: brick vs triangle shadows, mesh shadows, provider switching, preflight, curved probes; inline and threaded | 10 passed, no validation errors | `build/compute-gi/p5-editor.log` |

Evidence: `build/compute-gi/sponza/report.json` (`*-sb`, `*-s1`, `bs-*`, `lc-*`, `*-final`) with
`runs/` and `captures/`. The experiment drivers are `shadow_sweep.py` and `light_cull_sweep.py`; the
culling macro was removed from the shader afterwards. The comparison script is `shadow_quality.py`.

## Stricter adaptive placement — 2026-10-23

The sweep below picked the shipping rule. A subtile now places at most two adaptive probes
(`ZEN_PROBE_ADAPTIVE_PER_SUBTILE` 4 → 2), and only while at least 16 of its 64 receivers are
uncovered (`ZEN_PROBE_ADAPTIVE_MIN_UNCOVERED`). The subtile maps and the allocation block counts
shrink from `uvec4` to `uvec2`. Per subtile, that halves the map (8 bytes per history side instead
of 16) and the transient candidates (96 bytes instead of 192).

| Check | Result | Local evidence |
| --- | --- | --- |
| Focused RenderCore, pool resize test now with at most two probes per subtile | 23 passed, no leaks | `build/compute-gi/p4-cpu.log` |
| Full RenderCoreTest (fake RHI) | 698 passed, no leaks | `build/compute-gi/p4-rendercore.log` |
| Native probe fixtures, four submission modes | 24 passed, no validation errors | `build/compute-gi/p4-gpu-probe.log` |
| Curved-geometry editor test, inline and threaded | 2 passed | `build/compute-gi/p4-curved.log` |

The placement fixture now checks both sides of the minimum. A column of 16 uncovered receivers gets
an adaptive probe; a column of 8 does not. The allocation fixture offers zero to two rounds per
subtile, and its 450-slot pool runs out during the second round, inside the second workgroup.

**Curved fixture**, against the per-pixel gather (thresholds |bias| < 4%, difference < 6%, holes < 1%):

| Tracer | Adaptive probes | Bias | Difference | Holes |
| --- | ---: | ---: | ---: | ---: |
| Triangles | 11 (was 32–38) | −0.96% | 3.47% | 0% |
| Bricks | 9 | −1.18% | 3.54% | 0% |
| HW RT | 12 | −0.36% | 3.03% | 0% |

**New Sponza** (1600×900, as in the sweep). The shipped build's captures are byte-identical to the
sweep variant's, with both triangles and bricks. The sweep's same-session comparison therefore
applies: the frame fell from 16.08 to 15.08 ms with unchanged direct lighting.

This session's runs, against the demand-sized pool before the change. "Diffuse probe passes" are
every `Probe*` pass except the two specular ones:

| Configuration | Adaptive requested | Diffuse probe passes, ms | Probe trace, ms |
| --- | ---: | ---: | ---: |
| Probes 16 px, bricks 128 | 15,551 → 4,332 | 3.14 → 2.03 (2.03 again) | 1.71 → 0.89 |
| Probes 16 px, triangles | 15,551 → 4,332 | 9.75 → 5.39 | 8.32 → 4.25 |
| Probes 8 px, bricks 128 | 13,513 → 2,588 | 4.45 → 3.34 | 2.97 → 2.19 |
| Probes 16 px, bricks 128, HW shadows | 15,551 → 4,332 | 3.14 → 2.06 | 1.68 → 0.89 |
| HW RT, probes 16 px | 15,551 → 4,332 | 2.07 → 1.47 | 0.61 → 0.31 |

Bounced diffuse against HW RT pixel gather:

| Configuration | Energy | L1, per pixel | L1, 64 px blocks | Holes |
| --- | ---: | ---: | ---: | ---: |
| Probes 16 px, bricks 128 | −7.99% → −5.93% | 17.40% → 17.25% | 10.95% → 9.58% | 0.28% → 0.28% |
| Probes 16 px, triangles (and HW RT) | −2.91% → −2.60% | 15.41% → 15.30% | 7.37% → 6.16% | 0.18% → 0.20% |
| Probes 8 px, bricks 128 | −6.44% → −5.30% | 14.07% → 14.17% | 8.86% → 8.14% | 0.21% → 0.15% |

Whole-frame medians from this session do not isolate the change, for two reasons:

- **Direct lighting varies between builds.** `SceneLighting` measured 8.76–9.32 ms across runs.
- **The specular trace timing is bimodal.** `ProbeSpecularTrace` reads about 0.43 or about 0.72 ms
  per frame. The slow share was 25% in the sweep and 65–80% here, yet frames with a slow reading
  are no slower overall (15.76 against 15.80 ms). The pass ends beside the `ForwardBackground`
  graphics pass, and its SPIR-V is byte-identical before and after this change.

With hardware shadows, which avoids the software-shadow variance, the frame went from 7.59 to 7.15 ms.

## Adaptive probe demand sweep — 2026-10-23

The question: does the coverage test request more adaptive probes than the image needs? Variants
of `probe_adaptive.comp` were built from a scratch copy with macro overrides and compiled into the
worktree's SPIR-V. Nothing else changed, and the shipping shader was restored afterwards. The
default variant reproduced the shipping shader byte-for-byte on New Sponza and exactly on the curved
fixture. Three knobs:

- **Coverage threshold** (`ZEN_PROBE_COVERAGE`, 0.05): a receiver counts as uncovered below this
  summed uniform-probe weight.
- **Rounds:** at most this many adaptive probes per 8×8 subtile (`ZEN_PROBE_ADAPTIVE_PER_SUBTILE`,
  4).
- **Minimum uncovered receivers:** a subtile places its next probe only while at least this many of
  its 64 receivers are uncovered (1 in the shipping shader).

**New Sponza.** 16 px spacing and 1600×900, with the demand-sized pool, so every request is traced.
Quality is bounced diffuse from 96-frame captures, compared with the HW RT pixel-gather capture as
before. Cost comes from a bricks profile (20 + 60 frames). GI passes include the 0.16 ms
allocation-timestamp overlap described in the next section. Frame times also carry the session's ~5% direct-lighting
noise; the GI and trace columns are the reliable comparison.

| Variant | Adaptive requested | GI passes ms | Probe trace ms | Frame ms | Tri energy | Tri L1 1/16/64 px | Tri holes | Bricks energy | Bricks L1 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| No adaptive probes | 0 | 2.27 | 0.53 | 14.64 | -4.0% | 17.4% / 13.2% / 7.2% | 0.23% | -6.9% | 18.8% |
| Current: coverage 0.05, 4 rounds | 15,551 | 3.76 | 1.73 | 16.08 | -2.9% | 15.4% / 11.9% / 7.4% | 0.18% | -8.0% | 17.4% |
| Coverage 0.005 | 13,824 | 3.57 | 1.57 | 15.87 | -3.4% | 15.2% / 11.6% / 7.0% | 0.19% | -7.9% | 17.9% |
| Coverage 0.02 | 14,670 | 3.66 | 1.65 | 15.99 | -3.0% | 15.2% / 11.7% / 6.6% | 0.18% | -7.6% | 17.6% |
| 1 round | 6,910 | 2.95 | 1.09 | 15.29 | -2.5% | 15.3% / 11.4% / 6.5% | 0.19% | -6.5% | 17.3% |
| 2 rounds | 11,289 | 3.34 | 1.43 | 15.65 | -2.8% | 15.2% / 11.5% / 6.6% | 0.19% | -7.2% | 17.4% |
| At least 4 uncovered receivers | 11,850 | 3.55 | 1.44 | 15.68 | -2.4% | 15.3% / 11.7% / 6.9% | 0.18% | -7.3% | 17.1% |
| At least 8 uncovered receivers | 8,893 | 3.13 | 1.22 | 15.45 | -2.2% | 15.3% / 11.6% / 6.4% | 0.18% | -6.4% | 17.2% |
| At least 16 uncovered receivers | 5,062 | 2.80 | 0.94 | 15.20 | -2.3% | 15.4% / 11.5% / 6.1% | 0.20% | -5.9% | 17.1% |
| 2 rounds, at least 4 uncovered | 9,151 | 3.14 | 1.26 | 15.47 | -2.5% | 15.3% / 11.5% / 6.4% | 0.18% | -6.8% | 17.3% |
| 2 rounds, at least 8 uncovered | 7,182 | 2.97 | 1.11 | 15.31 | -2.6% | 15.2% / 11.3% / 6.2% | 0.18% | -6.4% | 17.2% |
| At least 32 uncovered receivers | 1,757 | 2.51 | 0.69 | 14.94 | -2.2% | 15.8% / 11.8% / 6.3% | 0.22% | -5.5% | 17.5% |
| 2 rounds, at least 16 uncovered | 4,332 | 2.71 | 0.89 | 15.08 | -2.6% | 15.3% / 11.4% / 6.2% | 0.20% | -5.9% | 17.2% |

**Curved fixture.** `ScreenProbeGatherTracksPixelGatherOnCurvedGeometry`: 160×120, reconstructed
sky against the pixel gather, sRGB, 80 uniform probes. Thresholds: |bias| < 4%, difference < 6%,
holes < 1%. HW RT behaved the same way; its run without adaptive probes was the only one with holes
(0.52%).

| Variant | Adaptive requested (tri) | Tri bias / difference / holes | Bricks bias / difference / holes |
| --- | ---: | ---: | ---: |
| No adaptive probes | 0 | -1.31% / 3.81% / 0.07% | -1.00% / 3.59% / 0.00% |
| Current: coverage 0.05, 4 rounds | 38 | -0.88% / 3.32% / 0.00% | -0.88% / 3.29% / 0.00% |
| Coverage 0.005 | 38 | -0.91% / 3.33% / 0.00% | -1.00% / 3.32% / 0.00% |
| Coverage 0.02 | 38 | -0.91% / 3.33% / 0.00% | -0.95% / 3.29% / 0.00% |
| 1 round | 12 | -1.15% / 3.60% / 0.00% | -1.19% / 3.60% / 0.00% |
| 2 rounds | 24 | -0.71% / 3.37% / 0.00% | -0.93% / 3.36% / 0.00% |
| At least 4 uncovered receivers | 31 | -0.98% / 3.35% / 0.00% | -1.03% / 3.38% / 0.00% |
| At least 8 uncovered receivers | 27 | -0.98% / 3.42% / 0.00% | -1.04% / 3.43% / 0.00% |
| At least 16 uncovered receivers | 19 | -1.00% / 3.47% / 0.00% | -1.19% / 3.55% / 0.00% |
| 2 rounds, at least 4 uncovered | 22 | -0.97% / 3.40% / 0.00% | -1.04% / 3.44% / 0.00% |
| 2 rounds, at least 8 uncovered | 19 | -1.09% / 3.51% / 0.00% | -1.10% / 3.48% / 0.00% |
| At least 32 uncovered receivers | 7 | -0.80% / 3.54% / 0.00% | -1.11% / 3.65% / 0.00% |
| 2 rounds, at least 16 uncovered | 16 | -0.96% / 3.47% / 0.00% | -1.18% / 3.54% / 0.00% |

**Findings.**

- **The threshold is not the problem.** Receivers are either clearly covered or not covered at all:
  a tenfold lower threshold removes 11% of New Sponza's requests and none on the curved fixture.
- **Adaptive probes earn their keep, but only the first few.** Without any, New Sponza loses 2 points
  of per-pixel error and 1 point of energy (triangles), and the curved fixture's difference rises from
  3.3% to 3.8%. Holes appear only with HW RT, at 0.5%. Since the review's integrate fallback, uniform
  probes alone already pass the curved thresholds.
- **The waste is the trigger.** A subtile takes a probe for a single uncovered receiver, and up to
  four rounds of them. On New Sponza those are mostly carvings, edges and normal-mapped detail,
  which the fallback already handles.
- **Recommended setting, now shipped:** at most two rounds, and a probe only while at least 16
  receivers (a quarter of the subtile) are uncovered.
  - New Sponza requests 4,332 probes instead of 15,551 (−72%). GI drops from 3.76 to 2.71 ms and the
    probe trace from 1.73 to 0.89 ms.
  - Quality is unchanged or better: per-pixel error 15.3%, block errors and brick energy improve.
  - The curved fixture stays well inside its thresholds (difference 3.5% against 3.3%, no holes).
- **Where it starts to hurt:** at a minimum of 32, New Sponza's per-pixel error rises to 15.8%.
- **The pool rarely needs to grow.** At the recommended setting, demand (4,332) fits the initial pool
  (5,700) on New Sponza.

**Gaps.** One static view per scene and one GPU. Thin features that cover fewer than 16 receivers of
a subtile now rely on the integrate fallback; New Sponza's holes rose from 0.18% to 0.20%. Motion and
disocclusion, where adaptive probes also catch newly visible surfaces, are unmeasured.

Evidence: `build/compute-gi/sponza/sweep.json` and `sweep.txt`, `runs/ad-*` (profiles and curved-test
logs) and `captures/ad-*`. Driver: `sweep.py`; variant source: `probe_adaptive_variant.comp`, both
in that folder.

## Demand-sized adaptive pool and parallel allocation — 2026-10-23

Follow-up to the New Sponza profile below. Two changes:

- **Pool sized to demand.** The adaptive pool now follows the requested count, read back without
  waiting from earlier frames: it grows to the request plus a quarter and shrinks after 240 frames of
  low demand. The probe history is copied across a resize.
- **Parallel allocation.** A per-block count pass plus a per-block scan replace the single-workgroup
  allocator. Slot assignment is unchanged.

Same worktree setup, GPU, view and settings as the New Sponza profile.

| Check | Result | Local evidence |
| --- | --- | --- |
| Focused RenderCore, with a new pool grow/decline/shrink test | 23 passed, no leaks | `build/compute-gi/p2-cpu.log` |
| Full RenderCoreTest (fake RHI) | 698 passed, no leaks | `build/compute-gi/p2-rendercore.log` |
| Native probe fixtures, four submission modes, with a 600-subtile, three-block allocation test against a CPU model of the round order | 24 passed, no validation errors | `build/compute-gi/p2-gpu-probe.log` |

At 8 px spacing the pool never ran short, and the new build's capture is bit-identical to the old
one: the parallel allocator assigns the same slots.

At 16 px the pool grew from 5,700 to 20,000 slots and now serves all 15,551 requested adaptive
probes (previously 5,700). GPU times in ms, frame / GI passes / probe trace:

| Configuration | Before | After |
| --- | --- | --- |
| Probes 16 px, bricks 128 | 16.5, 16.0 / 3.5, 3.2 / 1.47, 1.20 | 15.64, 15.68 / 3.73 / 1.71 |
| Probes 16 px, triangles | 21.2 / 8.0 / 5.08 | 23.1 / 11.2 / 8.32 |
| Probes 8 px, bricks 128 | 18.7 / 5.8 / 3.51 | 17.0 / 5.0 / 2.97 |
| Probes 16 px, bricks 128, HW shadows | 7.62 / 3.40 / 1.34 | 7.59 / 3.73 / 1.68 |
| HW RT, probes 16 px | 6.61 / 2.29 / 0.56 | 6.21 / 2.35 / 0.62 |

Direct lighting measured 12.0–12.1 ms this session against 12.4–12.9 ms before. Treat differences
of about 5% between the two sessions as noise; the 8 px row changed no work.

**Allocation.** The single-workgroup allocator measured 0.386 ms. The new allocation pass measures
0.009 ms. The count pass reads 0.159 ms, but that timestamp overlaps the specular resolve
(0.156 ms) running beside it in the queue, and the old figure carried the same overlap. The
allocation work therefore fell from about 0.23 ms to about 0.01 ms.

**Quality.** Serving every request did not bring this view closer to the reference. Bounced diffuse
against HW RT pixel gather, as in the New Sponza tables:

| Configuration | Bounce energy | L1, per pixel | L1, 64 px blocks | Holes |
| --- | ---: | ---: | ---: | ---: |
| Probes 16 px, triangles: before → after | −2.97% → −2.91% | 15.28% → 15.41% | 6.36% → 7.37% | 0.19% → 0.18% |
| Probes 16 px, bricks 128: before → after | −6.45% → −7.99% | 17.34% → 17.40% | 9.91% → 10.95% | 0.28% → 0.28% |

The extra probes change the columns and arch edges (5% L1 against the earlier capture) and leave
floors and walls alone. The dominant errors are uniform-probe blotches and brightening at contacts,
and neither is affected. With bricks, the added probes on curved and contact receivers come out
darker than the uniform-probe fallback they replace (−1.5 points of energy).

In this view the extra 9,851 probes cost about 0.5 ms of trace with no measurable gain. The 15,551
requests are 2.7 adaptive probes per uniform probe, which suggests the coverage test
(`ZEN_PROBE_COVERAGE`) requests many probes the fallback already serves well. Thin geometry, where
unserved requests leave holes, is not covered by this view.

Evidence: `build/compute-gi/sponza/report.json` (`*-pool` entries), `runs/*-pool.*` and
`captures/*-pool.*`, including `captures/probes-tri-pool-vs-probes-tri.diff.png`.

## New Sponza profile — 2026-10-23

Release build of the compute-GI changes alone: a detached worktree at `309ebb2a` with only the
compute-GI files copied in, so the concurrent asset-processor edits are excluded. RTX 5080, New
Sponza main glTF (imported sun and 22 lamps replace the environment), 1600×900, voxel GI view
(`--mode=3`), VSync off, async compute off, `voxel_gi_preset=medium` otherwise. Each timing run:
20 warm-up and 60 measured frames, median native GPU timestamps, RTSS-safe runner, implicit layers
disabled. Compute rows use `voxel_gi_backend=compute`; HW rows use `voxel_gi_backend=hardware`.

| Configuration | GPU frame | GI passes | Direct lighting |
| --- | ---: | ---: | ---: |
| Pixel gather, triangles (previous compute path) | 62.5 / 61.3 ms | 51.8 ms | 12.4 ms |
| Pixel gather, bricks 128 | 31.0 ms | 20.1 ms | 12.5 ms |
| Probes 16 px, triangles | 21.2 ms | 8.0 ms | 12.7 ms |
| **Probes 16 px, bricks 128** | **16.5 / 16.0 ms** | **3.5 / 3.2 ms** | 12.9 ms |
| Probes 8 px, bricks 128 | 18.7 ms | 5.8 ms | 12.8 ms |
| Probes 16 px, bricks 256 | 17.5 ms | 3.9 ms | 13.5 ms |
| Probes 16 px, bricks 128, HW shadows | 7.6 ms | 3.4 ms | 4.3 ms |
| HW RT, pixel gather | 11.5 ms | 9.5 ms | 4.0 ms |
| HW RT, probes 16 px | 6.6 ms | 2.3 ms | 4.4 ms |

GI passes sum the `Hybrid*` or `Probe*` passes; direct lighting is `SceneLighting` plus
`ForwardTranslucent`, whose shadow rays use the shadows provider (software triangles on compute).
Pass medians overlap slightly, so they do not add exactly to the frame. Two values are two runs.
The previous compute path measured 59.3 ms in `build/sponza-gi-investigation/perf/report.json`.

Brick + Probe cuts the GI work from about 51 ms to 3.2–3.5 ms, the frame from about 62 ms to 16 ms.
On compute, software-triangle direct shadows (12.9 ms) now cost almost four times the GI.
Probe passes, 16 px, bricks 128 (first run): trace 1.47, pixel temporal 0.57, specular trace 0.44,
allocate 0.38, integrate 0.36, specular resolve 0.16, adaptive 0.07, filter 0.03, temporal 0.02,
place 0.01 ms. The allocation pass runs as a single 256-thread workgroup; it costs more than the
adaptive search it serves. The 64 rays per probe come to about 0.73 M rays per frame, against about
5.8 M for four samples per pixel.

The brick scene is built once on load: 22.3 ms GPU at 128 cells (114,747 of 524,288 bricks, 599
large triangles, no overflow, 56.3 MiB) and 51.2 ms at 256 cells (146.3 MiB).

**Adaptive pool saturation.** At 16 px spacing the frame requests 15,551 adaptive probes against a
pool of 5,700 (`ZEN_PROBE_ADAPTIVE_ROW_RATIO 1u`), so 63% of the requests go unserved and those
receivers fall back to the uniform probes. At 8 px all 13,513 requests fit in the 22,600-probe pool.

Quality, from 96 fixed-step frames per configuration with `--capture-lighting`, compared with HW RT
pixel gather (which matches software-triangle pixel gather to 0.01% L1). Values are luminance of the
`bounced_diffuse` component over covered pixels. Holes are lit reference pixels that keep under 10%
of their value.

| Configuration | Bounce energy | L1, per pixel | L1, 16 px blocks | L1, 64 px blocks | Holes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pixel gather, bricks 128 | −3.2% | 10.2% | 7.7% | 6.1% | 0.31% |
| Probes 16 px, triangles (= HW RT probes) | −3.0% | 15.3% | 11.3% | 6.4% | 0.19% |
| Probes 16 px, bricks 128 | −6.5% | 17.3% | 13.9% | 9.9% | 0.28% |
| Probes 8 px, bricks 128 | −6.4% | 14.1% | 11.2% | 8.9% | 0.21% |

The final composed frame changes by 0.2/255 on average (combined energy −0.3%), because this view's
bounce light is small next to the direct lamps. The two error sources add up:

- The probe gather loses about 3% of bounce energy, whichever tracer it uses. Its error is
  mid-frequency rather than noise: block L1 stays at 11% for 16 px blocks. Difference maps show
  blotches a few probes wide on the floor and walls, and brightening at contacts such as column bases.
- Bricks lose about 3% more, mostly in the lamp-lit bounce pools next to the lanterns and on column
  sides, with light leaking along concave creases. Pixel gather with bricks shows the same pattern.

Local evidence is under `build/compute-gi/sponza/`: `report.json` (second runs), `report-pass1.json`,
`runs/`, `captures/` and the comparison text files. `captures/sponza-probes-bricks.png` holds the
reference bounce, the Brick + Probe bounce, the final frame and the signed difference map, with full
saturation at ±50%. Scripts: `profile.py`, `run_all.py`, `quality.py`, `diffmap.py`.

**Gaps.** One static view and one GPU. Motion, disocclusion, other scenes, other GPU vendors and
lower-tier hardware are unmeasured. The pixel-gather reference is itself a 4-sample filtered
estimate, not an independent ground truth.

## Review fixes and curved-geometry verification — 2026-10-22

A headless editor test on a ground/column/sphere fixture
(`ScreenProbeGatherTracksPixelGatherOnCurvedGeometry`, 160×120, Papermill, 48 frames per
configuration, inline and threaded execution) compares probe gather's reconstructed sky with the
per-pixel gather. Its first run showed thin and curved receivers black. Adaptive probes and
normal-aligned irradiance anchors followed, then the code-review fixes: up to four adaptive probes
per subtile with a deterministic, round-ordered allocation pass and an indirect trace dispatch;
the barrier between adaptive rounds; a footprint-capped plane tolerance; contact occlusion; an
integration fallback for uncovered receivers; bounce history across one-step cache changes; held
specular history for receivers without a sample; 4×4×4 empty-block skipping inside bricks;
once-per-streak brick fallback logging; probe counters in capture metadata.

Release build, RTX 5080, two build workers at below-normal priority; all tests headless with
implicit layers disabled.

| Check | Result | Local evidence |
| --- | --- | --- |
| Focused RenderCore (settings, probe graph/lifetime, brick fallback) | 22 passed | `build/compute-gi/g5-cpu.log` |
| Full RenderCoreTest (fake RHI) | 697 passed | `build/compute-gi/g5-rendercore.log` |
| Native probe fixtures, four submission modes | 20 passed, no validation errors | `build/compute-gi/g5-gpu-probe2.log` |
| Brick oracle with block skipping, four submission modes | 4 passed | `build/compute-gi/g5-gpu-brick.log` |
| Curved-scene editor test, inline and threaded | 2 passed | `build/compute-gi/g5-probe-final.log` |
| clang-format of the changed compute-GI files; no-exceptions check | Passed | — |

Curved-scene results against the per-pixel gather, sRGB over lit receivers (6,691 pixels). The first
run, with uniform probes only and a wider framing, left the column and sphere black:

| Stage | Tracer | Mean bias | Mean \|difference\| | Hole share |
| --- | --- | ---: | ---: | ---: |
| One adaptive probe per subtile, rotating each frame | triangles | +15.5% | 16.8% | 0.3% |
| Irradiance anchors, stable adaptive placement | triangles | +6.9% | 9.2% | 1.4% |
| Review fixes | triangles / bricks / HW RT | +7.9% / +7.8% / +8.0% | 8.2% / 8.1% / 8.3% | 0% |
| Unfiltered environment on misses | triangles / bricks / HW RT | −0.9% / −0.9% / −0.3% | 3.3% / 3.3% / 2.9% | 0% |

The remaining bias came from probe misses reading the prefiltered GGX environment at the mip
matching a ray's solid angle: on open ground the probe sky was 10–13% brighter in red/green and
25–30% in blue (linear). That specular split-sum filter is not an energy-preserving cone average;
probe misses now read the unfiltered environment, as the pixel gather does. Bright small sources
are no longer pre-blurred for probes; probe temporal and spatial accumulation absorb that variance,
and sun-heavy environments have not been examined.

**Gaps.** The last run here used SPIR-V built from a source variant behaviourally identical to the
final `probe_trace.glsl`. The tightened thresholds (|bias| < 4%, difference < 6%, holes < 1%) were
first compiled on 2026-10-23, in the compute-GI worktree build, because the main checkout's CMake
was blocked by an unrelated asset-processor edit. They pass there in both submission modes
(`build/compute-gi/p3-curved.log`). For New Sponza timing and quality see
[New Sponza profile](#new-sponza-profile--2026-10-23).

## G3 continuation status

Screen-probe gather, specular reconstruction, settings/UI and capture metadata are implemented.
After the initial coding-only continuation, the user authorized light correctness checks while
gaming on the primary display. Builds used one worker at below-normal process priority in the
existing `build/x64-windows-msvc-release/` folder. Tests ran headlessly: no window appeared on either
display. No benchmark, stress test, large scene capture or performance comparison was run.

| Check | Result | Local evidence |
| --- | --- | --- |
| C++ and all new probe shaders | Built successfully | `build/compute-gi/g3-correctness-build.log` |
| Renderer demo and UI | Built successfully; application not launched | `build/compute-gi/g3-demo-build.log` |
| Focused RenderCore correctness | 21 passed | `build/compute-gi/g3-cpu-correctness.xml` |
| Tiny native probe reconstruction | 12 passed, no Vulkan validation errors | `build/compute-gi/g3-gpu-correctness.xml`, `g3-gpu-correctness.log` |

The CPU cases cover settings parsing/rejection, both spacings, presets, allocation failure cleanup,
probe pass bindings/dispatch sizes, odd view extents, first-use clearing, declined/submitted frames,
resize, gather changes, diagnostic pixel fallback, and disabled temporal/filter controls. These use
the fake RHI and establish graph/lifetime behavior rather than rendered pixel correctness.

The native cases execute production probe clear, temporal, spatial filter, integration, specular
resolve and pixel temporal shaders on synthetic inputs. Each uses at most 17×9 receiver texels,
six SH probes and 9×5 specular samples. They check analytic constant-radiance normalization,
background/uncovered values, identity/plane/normal rejection and history/signal resets across
inline/threaded submission with async compute off/on. The twelve cases finished in 6.8 seconds
including device setup/teardown; this is a test duration, not a rendering performance measurement.

The native fixtures supply SH and raw specular samples directly. They do **not** execute the new
probe trace shaders or validate a complete scene gather using bricks, software triangles or HW RT.
Full-scene correctness smoke, image/motion quality and Sponza quality/timing comparisons at both
spacings remain pending. No preset was changed to select probes.

## Checks

Release build: Windows, MSVC 1951, NVIDIA GeForce RTX 5080. Debug builds and the initial native
oracle also passed. Final release results:

| Check | Result | Local evidence |
| --- | --- | --- |
| RenderCoreTest | 694 passed | `build/compute-gi/rendercore-release.xml` |
| RayQueryIntegrationTest | 20 passed | `build/compute-gi/ray-query-release.xml` |
| Vulkan upload integration | 44 passed, 4 existing packed-run cases skipped in fence modes | `build/compute-gi/vulkan-upload-release.xml` |
| Repository formatting and diff checks | Passed for 47 changed C++/GLSL files | `build/compute-gi/format.log` |

The brick oracle checks 1,208 analytic rays across five frames in each of four execution modes
(inline/threaded submission, async compute off/on): 24,160 ray checks. Cases cover hit/miss,
distance within two fine voxels, occupied starts, positive/negative/parallel directions, simultaneous
cell crossings, thin geometry, alpha coverage, unchanged-build reuse, resolution changes from 64
to 128, material edits and scene removal. Both small and large-triangle build paths execute.
No Vulkan validation errors appeared in the final ray-query run or the four quality captures.

The fake-backend regression verifies triangle fallback after allocation failure, recovery with
unchanged requested settings, discarded-frame history preservation and submitted-frame guide/history
reset. Settings tests check defaults, supported values, transactional rejection and preset behavior.

Counter readbacks retain their own buffer until the submission has retired. Overflow is logged once
per completed build without a new CPU wait. The native readback regression checks an explicit
transfer-write to host-read barrier on the copied range and the resulting bytes, with timeline and
fence completion in inline and threaded modes. This also fixes the existing copy path's reliance on
an optional breadcrumb barrier for host visibility.

## Sponza quality

960×540, Papermill environment, frozen top/hall cameras, 64 accumulated frames, normal maps enabled,
ray-hit bounce, temporal/filter enabled and the usual adaptive sampling. Reference inputs are the
archived 65,536-ray **triangle captures**, not independent Mitsuba renders. Source dependencies and
reference accumulation were verified before comparison. Values below are mean bias over floor pixels.

| View / tracer | Sky bias | Composed diffuse bias |
| --- | ---: | ---: |
| Top / triangles | −0.08% | −0.07% |
| Top / bricks 128 | −3.74% | −3.67% |
| Hall / triangles | −0.14% | −0.13% |
| Hall / bricks 128 | −4.42% | −4.37% |

The brick floor bias is substantially smaller than the rejected coarse-voxel near/far experiment's
roughly −30%. Both brick cases fall inside the survey's illustrative ±10% mean-bias screen; that is
a diagnostic threshold, not a new replacement for the existing triangle-path acceptance gates.
Side-by-side static images were inspected. Motion/flicker, independent-reference qualification and
other GPU vendors remain unmeasured.

The 128-cell Sponza build allocated 100,414 bricks from a 524,288-brick pool and classified 601 large
triangles, with no overflow. Persistent cells/bits/large-triangle storage is 42,992,108 bytes
(41.00 MiB); this excludes temporary distance buffers and diagnostic readbacks.

## GPU time

1920×1080, release renderer, 64 warmup frames followed by 120 measured frames, VSync and validation
off, RHI thread and async compute enabled. Native GPU timestamps; normal GPU clocks, without Nsight
clock locking. Both tracers use full-resolution diffuse and four fixed samples with adaptive sampling
disabled. Scene, lighting, cache and reconstruction settings match between each pair.

| View | Triangle `HybridTrace` median | Brick `HybridTrace` median | Pass speedup | GPU frame median, triangles → bricks |
| --- | ---: | ---: | ---: | ---: |
| Top | 41.32 ms | 12.63 ms | 3.27× | 47.09 → 19.09 ms |
| Hall | 34.97 ms | 17.60 ms | 1.99× | 42.46 → 25.21 ms |

`HybridTrace` includes gather, hit lighting and guide work. These are not isolated traversal timings
or an identical-ray-set rays/second measurement. The survey's proposed ≥5× traversal improvement and
final compute GI budget are not established. G3 must reduce the ray count and reconstruction cost.

## Reproduction

The driver reuses existing capture/benchmark tools, runs cases serially and restores `Data/engine.cfg`
after every run. Captures record requested/effective traversal, resolution, brick statistics, executable
and shader fingerprints. The timing runs record native per-pass/frame results and workload configuration.

```powershell
build/venv/Scripts/python.exe tools/validate_compute_gi.py `
  --exe build/x64-windows-msvc-release/bin/scene_renderer_demo.exe `
  --scene E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf `
  --reference-dir build/p8-nsight-optimizations/final-quality/voxel/static `
  --output build/compute-gi/g1-measurements --profile
```

Choose a fresh output directory for another run. `--resolutions 64 128 256` adds the other brick
resolutions; this recorded quality/timing run used 128. Full results, including sky/bounce/diffuse
region RMS and p99 error, are in `build/compute-gi/g1-measurements/report.json`. Static comparison
images and capture/profile logs live beside that report. Generated evidence is kept under ignored
`build/`; this document records the observed results for the repository.
