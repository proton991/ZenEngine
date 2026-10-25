# Compute GI implementation plan

2026-10-22. Implements the direction accepted in the [algorithm survey](AlgorithmSurvey.md): a screen-probe final gather over a sparse voxel-brick scene, reusing the existing voxel radiance cache for hit lighting. The quality bar is relaxed: results need not match HW RT or the Mitsuba references exactly.

## Scope decisions

- **Integration.** The new path writes the same three textures the hybrid composition already reads (`hybridSky`, `hybridBounce`, `hybridSpecular`). Every deferred, forward and shadow lighting variant stays unchanged. The hybrid receiver prepass (position, normals, motion, identity) is reused as-is.
- **Scene extent.** Bricks cover the existing voxel GI bounds rather than a camera clipmap. The radiance cache that lights hits covers the same bounds, so a tracing clipmap alone would not extend GI to open worlds. Clipmaps are a follow-up when open-world content is in scope (survey open question 1).
- **Hit lighting.** Hits read the existing voxel radiance cache and light-visibility cache (`HitRadiance`). The cache is single-bounce; feeding gathered irradiance back into it for multi-bounce is a follow-up.
- **Direct shadows and sky cache** keep their current providers by default. Bricks replace the GI gather rays (sky, specular occlusion, bounce and reflections) and, opt-in, Compute direct shadow rays.

## Defaults

Since 2026-10-25 the presets (default `voxel_gi_preset=medium`) trace every GI pass with HW RT when
the GPU reports usable ray queries, and otherwise with Compute, whose GI gather rays (sky, specular
occlusion, bounce and reflections) trace bricks. The sky cache and Compute direct shadows trace
triangles either way. A rejected configuration falls back to the same preset. The editor starts in
PBR + Global illumination (`--mode=pbr` selects PBR); the demo already started in Voxel GI.

## Settings

| Key | Values | Default | Meaning |
| --- | --- | --- | --- |
| `voxel_gi_compute_traversal` | `triangles`, `bricks` | `bricks` | What the Compute provider traces for GI gather rays |
| `voxel_gi_brick_resolution` | `64`, `128`, `256` | `128` | Brick cells per axis; each cell holds 8³ bits, so 128 resolves 1/1024 of the grid |
| `voxel_gi_gather` | `pixel`, `probes` | `pixel` | Per-pixel stochastic gather (existing) or screen probes |
| `voxel_gi_probe_spacing` | `8`, `16` | `16` | Pixels per screen-probe tile side |

Compute direct shadow rays always trace triangles. Brick-traced shadows (`voxel_gi_shadow_traversal`)
were removed on 2026-10-24 for their quality; configs that still name the key load and ignore it
([Verification.md](Verification.md#direct-shadow-cost-on-the-compute-path--2026-10-23)).

Defaults keep today's qualified behaviour. A preset change follows measurement.

## Progress (2026-10-22)

G1 is implemented and has its first RTX 5080 verification results in [Verification.md](Verification.md):
the analytic GPU oracle passes in all four submission modes, and 128-cell bricks reduce the measured
1080p `HybridTrace` pass by 1.99–3.27× on the two Sponza/Papermill views. Floor sky bias is −3.74%/−4.42%
against the archived triangle references. These are whole-pass measurements; they do not establish
the survey's proposed 5× identical-ray traversal target or the final GI budget.

G3 code is now implemented: `ScreenProbeGather` owns diffuse SH probe histories, a reduced-resolution
specular trace, and compact per-pixel reconstruction histories. All four config keys, editor controls
and requested/effective capture metadata are connected. Defaults were triangle traversal and pixel
gather until 2026-10-25 (see Defaults below). A subsequent correctness-only verification built the shaders, renderer/UI and selected test
targets under `build/` with one low-priority worker. All 21 focused CPU cases and 12 tiny native GPU
reconstruction cases passed. The GPU cases use at most 17×9 receivers and six probes, create no windows,
and cover the four submission modes. See [Verification.md](Verification.md) for the exact scope.

A headless curved-geometry check (editor test `ScreenProbeGatherTracksPixelGatherOnCurvedGeometry`)
then exposed black thin and curved receivers. Adaptive probes, normal-aligned irradiance anchors,
contact occlusion and the review fixes below followed; the test now passes with 0% holes and under
1% mean bias against the per-pixel gather, for triangles, bricks and HW RT. See
[Verification.md](Verification.md#review-fixes-and-curved-geometry-verification--2026-10-22).

New Sponza (1600×900, RTX 5080): Brick + Probe runs the GI in 3.2–3.5 ms, against 51 ms for the
previous compute path; the frame drops from about 62 ms to 16 ms. Probe gather costs about 3% of
bounce energy, and bricks about 3% more. See
[Verification.md](Verification.md#new-sponza-profile--2026-10-23).

The adaptive pool now follows demand and allocation runs in parallel (about 0.01 ms instead of
0.23 ms). Serving all 15,551 requests on New Sponza costs about 0.5 ms more trace, with no measured
quality gain on that view. See
[Verification.md](Verification.md#demand-sized-adaptive-pool-and-parallel-allocation--2026-10-23).

A sweep of placement rules then showed that most of those requests were wasted. A subtile now takes
at most two adaptive probes, and only while at least 16 of its receivers are uncovered. See
[Verification.md](Verification.md#adaptive-probe-demand-sweep--2026-10-23) and
[Verification.md](Verification.md#stricter-adaptive-placement--2026-10-23).
Remaining work:

- Reduce probe mid-frequency error and contact leaks.
- Cut the Compute direct shadows, about 12.6 ms of the compute frame, of which about 8.5 ms is triangle
  traversal. Brick shadows saved 1.4 ms but were removed for their quality; the bigger lever is fewer shadow rays with trusted
  visibility (reused receiver visibility masks, or stochastic light selection with denoising). See
  [Verification.md](Verification.md#direct-shadow-cost-on-the-compute-path--2026-10-23).
- Moving frames remain further from the converged result than settled ones (strafing on Sponza,
  sky tiles off by over 10%: 36.6% against 32.7%), mostly on content entering the view. Young
  probes now trace up to four ray batches; more would cost more than the 0.5 ms they take while
  moving. About 10% of probes still restart each frame because they lost their tile and no free
  neighbour took them. A large camera jump that stays under the cut threshold keeps history but
  starts many new uniform probes at four batches: about twice the trace for two frames (2.0–2.4
  against 1.1 ms on glTF Sponza at 1600×900). A per-frame budget of new-probe batches would bound
  it. See [Verification.md](Verification.md#extra-rays-for-young-probes--2026-10-24).
- Measure other GPUs before changing presets.

## G1: brick scene and tracer

**Data** (`BrickScene`, owned by `VoxelGIRenderer`):

- `BrickCells`: one `uint` per cell. `0` empty during construction; `1..N` brick index + 1; `0x7FFFFFFF` solid (no brick, fully blocking). After distance construction, an empty cell sets `0x80000000` and stores its Chebyshev distance to the nearest non-empty cell in the low bits (capped at 15), so one fetch both tests occupancy and sizes a skip.
- `BrickBits`: 16 `uint` per brick, 8³ occupancy bits.
- `BrickCounters`: allocation count, overflow flag and the large-triangle indirect arguments.

**Build** (GPU, on geometry revision or resolution change), from the software ray query's world-space triangles:

1. Clear cells and counters.
2. Mark: one thread per triangle tests the cells its bounds overlap (triangle/box SAT). Triangles spanning more than 64 cells are appended to a list and marked by one workgroup each.
3. Allocate: one thread per marked cell takes a brick from the pool; when the pool is full the cell becomes solid (conservative, logged).
4. Rasterize: the same triangle iteration sets each overlapped fine voxel's bit. Alpha-masked triangles test coverage at the fine voxel's closest triangle point.
5. Prune empty bricks left by alpha coverage; three separable passes write each empty cell's Chebyshev distance.

**Trace** (`brick_trace.glsl`, `BrickTraceRay`, returning `HybridRayResult`): cell DDA with distance skips; inside a brick a fine DDA over the bit mask. A ray that starts inside occupied voxels passes that starting run, up to `ZEN_BRICK_START_RUN_VOXELS` (4) fine voxels, and hits from the first empty voxel on; a start inside a solid cell still hits at once. Counting the starting voxel as a hit blocked every direction for receivers in concave corners, whose biased origin lies in the other surface's voxels: per-pixel gather showed black, voxel-stepped strips along mesh edges (Sponza, 2870×1579, looking down the hall: 4.1% of pixels got under a quarter of the triangle sky light, 0.44% after; pixels over 1.5× it went from 1.09% to 1.20%; mean sky light 95.4% → 99.4% of triangles). Probes hid it, tracing from one pixel per tile and spreading the result. The hit normal is the entered face; the hit cell indexes the radiance cache. A diffuse bounce ray whose brick hit lies within one class cell of a cached point or spot light is queued and re-traced by a separate pass against opaque triangles from one fine voxel before the hit, for two class cells before bricks take over again, and near-light visibility from re-traced hits traces opaque triangles: lamp light reaches a room through lantern openings centimetres wide, which fine voxels close. Brick trace shaders carry no triangle traversal (`hybrid_retrace.glsl`, `hybrid_retrace_bricks.comp`, `probe_retrace_bricks.comp`); reflection rays are not re-traced. Reference accumulation, with hundreds of rays per receiver and frame, re-traces inline in its own brick trace variants instead of queueing; ray-count captures re-trace with a counting variant. Bricks lost 80–85% of New Sponza's floor lamp bounce against the CUDA reference before this (see [Verification.md](Verification.md#brick-traversal-and-lamp-bounce--2026-10-25)).

**Per-pixel use.** New `HybridTrace*Bricks` variants trace Compute features with bricks, so G1 can be measured inside today's pipeline before probes exist.

**Exit.** A GPU oracle test against analytic geometry (hit/miss agreement and distance within two fine voxels); Sponza floor bias against the archived references, compared with the rejected near/far result (about −30%); Compute trace time against the software BVH.

## G3: screen-probe gather (`ScreenProbeGather`, owned by `HybridGIRenderer`)

Per frame at 1080p and 16-pixel spacing: about 8k probes × 64 rays, plus about 0.5 M half-resolution specular rays.

1. **Place** (`probe_claim.comp`, then `probe_place.comp`): each tile claims last frame's uniform probe that now projects into it onto the same surface (receiver identity, plane and normal), at its exact world position; when several do, the longest history wins, then the one nearest the tile centre. A probe fixed to its screen pixel slid across the scene while the camera moved, and its history averaged light along that trail: moving frames lost occlusion contrast and drifted from the settled view by 17–20% per probe tile on sky light, 7–9% after the fix (see [Verification.md](Verification.md#screen-probes-while-the-camera-moves--2026-10-24)). Kept probes drift unevenly across tiles, leaving some tiles two and others none. A probe that lost its tile therefore goes to the free neighbouring tile whose centre is nearest it, if it lies on the surface a new probe there would take; every tile derives that destination from the claims, so no probe is kept twice. This halved the probes started each frame while the camera moves (see [Verification.md](Verification.md#probe-churn-while-the-camera-moves--2026-10-24)). A tile that keeps or takes no probe chooses a receiver from up to eight candidates, fixed per tile. Candidates that changed every frame moved every receiver's probe interpolation every frame: on Sponza at 2870×1579, 6.5% of pixels changed sky light by more than 2% per frame (per-pixel rays: 0.7%), against 0.5% with fixed candidates, which were also slightly closer to per-pixel rays. Probe rays still rotate per frame and accumulate in the probe history.
2. **Adaptive placement** (`probe_adaptive.comp`, one workgroup per 8×8 subtile): while at least 16 of the subtile's receivers have no compatible probe (thin or curved geometry, silhouettes), propose an adaptive probe on one of them, up to two per subtile, each round counting the earlier rounds' probes. Smaller gaps (edges, carvings, surface detail) take the integration fallback instead. A receiver under one of last frame's adaptive probes (from the 3×3 subtiles around the subtile's reprojection) that still projects onto its surface goes first, so adaptive probes also stay put while the camera moves. Otherwise the choice is hashed from the pixel, so it is stable while the view holds still. The subtile maps hold one `uvec2` per subtile.
3. **Allocate** (`probe_allocate_count.comp`, then `probe_allocate.comp`, one 256-subtile block per workgroup): the first pass counts each block's candidates per round. The second sums the counts of earlier blocks and of all blocks, scans its own block, and assigns the adaptive pool round by round: every subtile's first probe precedes any subtile's second, and subtiles go in index order within a round. The result does not depend on workgroup scheduling when the pool runs short. It writes the subtile map, the counters and the trace's indirect dispatch.
4. **Trace** (`probe_trace.comp`, one 64-thread workgroup per uniform probe and allocated adaptive row; empty slots exit before tracing): trace 8×8 stratified uniform-hemisphere directions around the probe's geometric normal, in up to four batches with fresh jitter that the sample averages. A kept probe with under 8 frames of history traces four batches, under 24 two, otherwise one; a new uniform probe traces four, since its starting history is a blend of its neighbours' at other positions; a new adaptive probe traces one, since adaptive probes are numerous and re-chosen often while the camera moves. Only a probe clearly younger than the view takes extra batches: its history must be more than two frames shorter than the frames since the probe history last restarted (`ScreenProbeGather` counts submitted frames into `probe.parameters.w`). Right after a restart (startup, a camera cut) every probe is as young as the view and traces one batch, so the trace does not spike; without valid history every probe traces one as well. The placement carries the count in `screen.w`. While the camera moves this brings content entering the view to its own light sooner, for about 0.5 ms on glTF Sponza at 1600×900 (see [Verification.md](Verification.md#extra-rays-for-young-probes--2026-10-24)). Misses read the unfiltered environment, as the pixel gather does: the prefiltered GGX mips are a specular filter, not an energy-preserving cone average, and reading them brightened open sky by 10-30%. Hits read `HitRadiance`. A shared-memory reduction projects sky and bounce into L1 spherical harmonics and also sums the exact cosine-weighted irradiance along the probe normal, the anchor that SH corrects only toward other normals.
5. **Temporal** (`probe_temporal.comp`): a kept probe, moving or still, takes its own previous record alone and keeps four times the history limit for sky light; blending neighbours' histories every frame would diffuse light between probes over that long history. A new probe starts from the previous uniform probes of the 3×3 tiles and the adaptive probes of the 3×3 subtiles around its reprojection, weighted by plane distance, normal and screen distance to where each of them actually was, and accumulates up to the history limit. History counts ray batches: a sample of four batches adds four frames and weighs four times one of a single batch. The longer history matters because a probe's 64 rays change direction every frame, and whole probe tiles shifting by about 1% per frame read as water-like shimmer on still frames (Sponza, 2870×1579, looking down the hall: 3.9% of pixels changed sky light by more than 1% per frame, 0.3% with the longer history; per-pixel rays 1.9%). Bounce keeps its history across a one-step radiance-cache change (at most four frames) instead of restarting, so moving lights converge with a few frames of lag.
6. **Spatial** (`probe_filter.comp`): neighbouring uniform tiles and adaptive subtiles with plane and normal weights.
7. **Integrate** (`probe_integrate.comp`, full resolution): each receiver interpolates nearby uniform and adaptive probes by screen distance, plane distance and normal similarity and evaluates sky and bounce irradiance at its shading normal. A receiver no compatible probe covers takes the best-facing nearby probe instead of staying black. Contact occlusion from G-buffer positions closer than one probe tile multiplies the result. A short per-pixel temporal pass smooths the remaining per-frame variation.
8. **Specular** (`probe_specular_trace.comp`, `probe_specular_filter.comp`, `probe_specular_resolve.comp`): one GGX ray per 2×2 block (rotating leader); with the spatial filter enabled, four edge-aware 3×3 à-trous passes over the half-resolution samples (block steps 1, 2, 4, 8; same surface, plane, lobe and roughness, read from a per-block receiver record the trace writes), with visibility reaching about 30 pixels on rough surfaces and reflected radiance keeping the per-pixel filter's roughness-limited footprint; then a full-resolution resolve that reuses neighbouring block samples by depth, normal and roughness, followed by temporal accumulation. Without the sample filter, one binary visibility ray per block flickered visibly on still frames: on Sponza at 2870×1579, 41% of pixels changed specular by more than 2% per frame, against 7% for per-pixel rays; with it, 4.4%, for about 0.3–0.5 ms on an RTX 5080. A receiver without a compatible sample is marked (alpha −1) and keeps its history; without history it takes its diffuse sky visibility rather than the unoccluded environment. Output matches the existing contract: RGB reflected radiance, alpha environment visibility.

The tracer is selected per provider: bricks or triangles for Compute, `rayQuery` for HW RT.

Implementation details and diagnostic limits:

- The persistent probe record is 208 bytes (three receiver vectors, eight L1 SH coefficients, two
  irradiance anchors). Diffuse trace shared storage is about 10.4 KiB per workgroup. Spatial filtering
  reads the temporal result into a separate buffer; filtered SH is never fed back into the next
  frame's probe history.
- The placement passes mark a kept probe by a negative `normal.w` naming its history slot; the
  temporal pass reads it and restores the field. Reprojection uses the inverse of the hybrid
  reconstruction matrix, which maps positions relative to `worldOrigin`, so probe world positions
  subtract that origin first. Without it every probe reprojected hundreds of pixels away and almost
  none were kept.
- The plane test accepts a quarter of the probe/receiver distance, capped at two probe tiles of the
  receiver's pixel footprint, so smooth curved surfaces share probes but parallel surfaces separated
  mostly in depth (a table top in front of the floor) do not.
- The adaptive pool starts at one slot per uniform probe and follows demand. Every frame copies the
  requested count to a host buffer, with up to four copies in flight. When a copy has completed and
  the request exceeds the pool, the pool grows to the request plus a quarter, up to every subtile's
  two candidates. After 240 completed frames in a row that needed at most half the pool, it shrinks
  to the largest of those needs. A resize replaces both probe histories. The next submitted frame
  copies the previous probes into the slots both sizes share, and the temporal pass ignores history
  slots past that count. A count above the possible maximum is ignored.
- Lighting captures record the uniform probe count, adaptive capacity, allocated and requested
  adaptive probes.
- Pixel sky/bounce history is at most four frames (two during responsive lighting changes). Specular
  history uses the configured limit, shortened for smooth surfaces and changing radiance. Six pixel
  textures store 56 bytes per receiver per history side. Only accepted submissions advance histories;
  first-use clearing remains pending after a declined frame. Switching gather modes releases the
  inactive histories and invalidates reuse even if that transition frame is declined. Probe-buffer
  allocation checks the device storage-buffer range before creating resources.
- `filter=false` skips the probe spatial filter; full-resolution integration and specular resolve
  still reconstruct the sparse samples. `temporal=false` disables both temporal accumulations.
- Static reference accumulation and instrumented ray-count captures explicitly select the existing
  pixel gather. Metadata records both requested and effective gather so those runs cannot be mistaken
  for probe measurements. Ordinary lighting captures and profiling use the selected probe path.
- Probe captures expose integrated sky/bounce and resolved specular before pixel temporal accumulation,
  plus reconstructed outputs and actual pixel history lengths. Pixel variance, rejection reason, and
  a single reflected hit distance are unavailable and identified as such in metadata. The rejection
  debug view is unavailable for probes; raw/reconstructed sky, history length and specular visibility
  views are connected.

## G4/G5: settings, tiers, verification

- Config, editor and runtime controls for the keys above; capture metadata records them. The editor's
  algorithm is named PBR + Global illumination. The shared **GI tracing** panel
  (`LightingBackendControls.cpp`) has two levels:
  - **Ray tracing** (Compute or HW RT) sets every pass's provider. It shows Per pass when the passes
    differ, and keeps each pass's enables and Compute tracer.
  - A **Passes** table gives each pass one tracer. Diffuse GI (sky and bounce rays) and Specular GI
    (occlusion and reflection rays) choose HW RT, Compute: Bricks or Compute: Software BVH, and share
    one Compute traversal. Either can be switched Off to tell the two contributions apart: Off lights
    that term from the unoccluded environment, as without GI (`indirectDiffuse`, config
    `voxel_gi_indirect_diffuse`; specular Off clears `specularOcclusion`). Diffuse GI keeps tracing
    while off. Sky cache chooses HW RT or Compute: Software BVH. Direct shadows chooses HW RT, Compute: Software BVH or Shadow maps; it has no brick tracer.

  Brick cells appear while a pass uses bricks. Gather and probe spacing are GI attributes, alongside
  indirect intensity, contributions, and the voxel cone numbers, which light translucent surfaces and
  the optional cone bounce. `GetGIPassTracer` and `SetGIPassTracer` hold the mapping onto
  `VoxelGISettings`, and `ZenUI/Tests/LightingBackendControlsTests.cpp` covers it.
- Native tests: settings parsing, brick build/trace oracle, probe path smoke test in all four submission modes.
- Measurements on Sponza/Papermill views: per-pass GPU time and composed-diffuse error against the archived references, for triangles and bricks, pixel and probe gather.
- Portability: shared memory under 16 KB per workgroup, no 64-bit integers, atomics only on 32-bit integers.

## Follow-ups (not in this change)

Camera-centred brick clipmaps; multi-bounce feedback into the radiance cache; HZB screen traces before brick traversal; GPU triangle transform for animated scenes (bricks currently rebuild from the CPU-built triangle list); Compute-lite world probe grid.
