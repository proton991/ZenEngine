# World cache: adopting Lumen's World Cache design (plan, 2026-11-20)

## Why

The native GI mode loses most of its frame time to motion and to cold world-cache cells, not
to the settled frame. From `fps-20261120/timeline.py` (pool 4, `tl-o4k`, frame_cpu_ms medians):

| Phase | Now | Unresolved samples not re-traced (diagnostic) |
|---|---:|---:|
| Settled | 24.7 ms | 24.5 ms |
| During the 12-frame walk | 49.2 ms | 32.3 ms |
| 32 frames after the walk | 65.9 ms | 32.9 ms |
| Frames 33-160 after the walk | 34.6 ms | 26.8 ms |

Causes:

- A probe ray whose hit lands in an unusable world-cache cell is traced again as a whole path:
  up to 16 bounces, with a shadow ray at every bounce.
- After a walk some cells stay unusable for more than 100 frames. The cause is not yet known.

The research note in `NativeGIOptimization.md` ("How shipped systems keep this cost flat") points
to Lumen. This plan reads Lumen's World Cache in the local UE source and adapts its techniques.
No Unreal code is copied: the Epic EULA does not allow it. Only the design is described here.

## What Lumen's World Cache does

Source: `E:\Dev\UE5`, branch `ue5-main`, `Engine/Build/Build.version` 5.9, read 2026-11-20.

- `Engine/Source/Runtime/Renderer/Private/Lumen/LumenWorldCache.cpp`
- `Engine/Shaders/Private/Lumen/LumenWorldCache.ush`
- `Engine/Shaders/Private/Lumen/LumenWorldCacheSampling.ush`
- `Engine/Shaders/Private/Lumen/LumenWorldCacheTracePaths.usf`
- `Engine/Shaders/Private/Lumen/LumenWorldCacheUpdate.usf`
- `Engine/Shaders/Private/Lumen/LumenHardwareRayTracingCommon.ush`

### Structure

- **Hash table.** Up to 2^20 entries (`r.Lumen.WorldCache.MaxEntriesLog2`).
  - Each entry has a 32-bit key: cell coordinates mod 256 on each axis, the clipmap index, and
    one of six faces (the dominant axis of the hit's geometric normal).
  - Every hit in a cell with the same face shares the entry. There is no anchor point and no
    plane test.
- **Clipmaps around the camera.**
  - The finest clipmap has a 512-unit radius and 80 cells across (`FirstClipmapRadius`,
    `ClipmapResolution`), so its cells are 12.8 units.
  - Each doubling of distance doubles the cell size.
- **Lookup.**
  - The key is hashed and up to 4 consecutive slots are probed.
  - The optional filter reads one tap jittered by up to half a cell in the surface's tangent
    plane. It falls back to the exact cell.
- **Per entry:**
  - the key;
  - 8 bits of frames unused and 8 bits of frames accumulated;
  - pre-exposed RGB lighting, quantized with dithering;
  - a fixed-point RGB accumulator plus a sample count, added with atomics.

### Fill: screen-seeded paths at a fixed budget

- **One path per 8×8 pixel tile** (`PathDownsampleFactor` 8), started from a blue-noise pixel
  in the tile. At 4K that is 129,600 paths per frame, whatever the camera does.
- **First direction.** It follows the pixel's material, as the screen-probe gather and
  reflections do. The GGX reflection lobe is picked with a probability given by its specular
  weight; otherwise the ray is cosine-diffuse.
- **Wavefront.** Each bounce is one ray-generation dispatch. The path state lives in three
  textures: ray, previous vertex's entry, and previous albedo plus throughput.
  - At most 3 bounces (`MaxBounces`).
  - Russian roulette: a path continues with probability √(throughput × albedo luminance).
    A stopped path still gets the last-bounce cache read described below.
- **At each valid hit:**
  - Find or insert the hit's entry, claiming the key with compare-and-swap.
  - Its lighting is emission plus albedo/π × one sampled light. The light is chosen by
    weighted reservoir sampling over up to 16 culled local lights plus the directional lights,
    and gets one shadow ray.
  - That lighting is accumulated into the entry with weight 1.
  - The previous vertex receives its albedo × the radiance arriving along the segment, with
    weight 0. That radiance is this entry's lighting from the last frame, or this hit's new
    lighting when the entry is still black.
  - A miss sends sky radiance back to the previous vertex instead.
- **Last bounce.** A compute pass with inline ray queries traces the continuation, reads the
  previous frame's cache at the hit (no material, no lights) and sends the result back. Further
  bounces arrive through frame-to-frame feedback.
- **Clamp.** Every sample is clamped (`MaxRayIntensity` 40).

### Update: one thread per entry

- This frame's accumulator is resolved to a mean.
- **A new entry takes its history from its parent**, the entry for the same face one clipmap
  coarser, together with the parent's accumulated frame count.
- The history weight is 1 − 1/min(n + 1, 16) (`MaxFramesAccumulated` 16).
- Eviction:
  - after 64 frames without samples (`NumFramesToKeepUnusedEntries`);
  - when the entry leaves the clipmap range;
  - a camera cut resets the cache.
- Only entries that received samples clear their accumulator, so there is no full clear.

### Consumers

- Lumen's hardware ray tracing reads the world cache at ray hits in place of the surface cache.
- A miss returns black, marked "radiance not completed".
- **The screen-probe gather writes the returned radiance as it is and retraces nothing.**
- Reflections retrace incomplete hits with hit lighting: material and direct light at that one
  hit.
- A cold cell therefore costs the probe gather nothing extra. The fill paths start from the same
  visible pixels, so they insert and fill that cell within a frame.

## How ZenEngine differs

| | Lumen | ZenEngine now |
|---|---|---|
| Cell identity | (cell, face); every hit in it shares the entry | (cell, normal bin) plus an anchor point. A hit more than 5% of the cell size off the anchor's plane, or whose normal deviates by more than about 18°, fails and tries a second, plane-split key. |
| Stored value | Outgoing diffuse radiance with the average albedo baked in | Irradiance/π in six directional records. The hit applies its own material. |
| Where samples come from | Path vertices, where camera-seeded rays land | A per-cell scheduler: 430,080 updates per frame from each cell's anchor, split among new, requested and mature cells |
| Creating a cell | The path vertex inserts it and adds a sample in the same frame | Miss → request queue → seed claim the next frame → scheduled update → resolve |
| Direct light per sample | One light by reservoir sampling, one shadow ray | Every light evaluated; the principal light and one sampled light get shadow rays |
| Path length | 3 bounces, then the previous frame's cache | Up to 16 bounces with Russian roulette, ending at the first usable cell |
| History of a new cell | The parent's value and count | Empty |
| History length | 16 frames | Up to 128 updates |
| Eviction | 64 frames without samples | 240 frames without requests |
| Lookup cost | 1 key, at most 4 probes | 2 keys × 8 probes; a miss adds a six-neighbour search (up to 96 probes) |
| Miss in the probe gather | Black sample, no retrace | The whole path is traced again |

**Adopt from Lumen:**

- A bounded miss: the parent cell, then the hit's direct light only, never a path.
- Parent-seeded history for new cells.
- Cells identified by (cell, face) alone, with cheaper lookups.
- Filling from path vertices with a fixed tile budget, in place of the per-cell scheduler.

**Keep from ZenEngine:**

- **Irradiance without albedo, in six directional records.** Hits keep their texture detail, and
  receivers with normal maps keep a direction-dependent value. Path vertices accumulate into the
  six records with the same projection the update uses today.
- **The 128-update cap.** It fixed the overbright patches that light edits exposed. Lumen's 16
  frames will be offered only as an option to test.
- **The response-shortened history for lighting edits.** Lumen has no edit handling beyond its
  short history.

## Phases

The phases are ordered by frame-time payoff per day of work. W1 and W2 are small and land on
their own. W3 is the Lumen World Cache itself.

### W0: count the misses (half a day)

- **Count probe-hit lookups per frame by outcome.** Extend the existing `requestCount` counters,
  sampled one path in 64 as today. Outcomes:
  - usable exact cell;
  - usable through the neighbour borrow;
  - key absent;
  - key present but the anchor is incompatible;
  - present but not yet resolved.
- **Measure time to first use.** For a sampled cell after the walk, count the frames from its
  first request to its first usable resolve.
- **Time the current passes.** Get each world-cache pass from Nsight: seed (3 passes), schedule,
  update and resolve. That total is the cost W3 must not exceed.
- **Run fast loops.** Use the walk segment of `timeline.py` only.

W0 explains the "unusable for 100+ frames" cells. If incompatible anchors dominate, W3's
identity change fixes them. If the cells are simply unresolved, they are a scheduling delay,
which W3 also removes.

### W1: bounded miss handling (1.5 days, the largest frame-time lever)

A probe hit whose cell is unusable is resolved in this order:

1. **The parent cell**: the same position one level coarser. This is Lumen's parent entry and
   GI-1.0's coarser-level read. One find replaces the six-neighbour search in `WorldLookup`.
2. **Endpoint shading**: emission plus reflectance × one sampled light with one shadow ray, with
   no continuation. This is Lumen's hit lighting for reflections and GI-1.0 §2.2.4.
3. **The request stays**, so the cell is still created.

Implementation:

- Retrace unresolved recorded samples in `ScreenProbeTrace` as a single segment:
  `NativePathRadiance` with a bounce limit of 1. The hit shader already honors the endpoint flag
  (value 8 in `rayShading.y`). This is the smallest change.
- A follow-up shades those hits in `ScreenProbeShadeHits` with an inline shadow-ray query, which
  saves re-tracing the first segment. Do this only if Nsight shows the retrace still costs
  noticeable time.
- Cache-update paths keep continuing through cold cells but stop at 3 bounces, as Lumen does,
  instead of 16. Feedback supplies the further bounces. Measure this as a separate step.

Expected result: close to the diagnostic column above.

- Walk: about 32 ms.
- 32 frames after the walk: about 33 ms.
- Frames 33-160 after: about 27 ms.
- The shadow ray on each miss keeps it a little above the diagnostic.

Quality cost: newly revealed surfaces lose their indirect bounce until their cell resolves, and
the parent cell covers most of it. Check with one A/B run of the fast-motion harness (RMS after
stop).

### W2: parent-seeded history (1 day)

- On its first resolve, a cell starts from its parent's resolved records instead of from empty.
  The parent is the same normal bin one level coarser, found from the cell's anchor position.
  The parent's age is capped so that the new cell's own samples take over within about 16
  updates.
- The parent's records have the same layout, so no conversion is needed.
- Effect: the first noisy updates after motion disappear, and the switch from W1's parent
  fallback to the fine cell does not pop.

### W3: Lumen-style fill and cell identity (6-9 days, the core)

W3 replaces the seed selection, the request queue, the compaction and schedule, and the
per-cell update with a path fill. W3's cell identity cannot land without the path fill: with
anchor-based updates, dropping the plane test would spread one anchor's value over other
surfaces in the cell. With path-vertex samples, a cell's value is the average over the points
where rays actually land, which are the points that read it.

1. **Path fill.** It reuses the split built for the probe trace: a record-only closest-hit pass
   (like `ScreenProbeTraceHits`) followed by compute shading (like `ScreenProbeShadeHits`).
   - One path per 8×8 tile from a blue-noise pixel. The first direction is cosine-diffuse, or
     GGX by specular weight when reflections are on.
   - The shading step, at each bounce:
     - find or insert the (cell, face) entry at the hit;
     - direct light: one light from the existing light-importance selection, with one shadow ray
       (inline ray query in compute, or a shadow-only ray tracing pass);
     - add emission;
     - accumulate into the six directional records;
     - send the incoming radiance on the previous segment back to the previous vertex's records;
     - apply Russian roulette;
     - write the path state.
   - After 3 bounces, a last bounce traces once, reads the previous frame's cache and sends the
     result back.
   - Accumulate only the records that a direction projects onto (at most three axes), to keep
     the number of atomics low. Reduce within a wave where lanes share an entry.
   - The seventh seed record, used only without screen probes, accumulates the same way along
     the vertex's shading normal.
2. **Resolve**, one thread per entry:
   - take the mean of this frame's samples;
   - blend it with the history (the parent's history for a new cell, as in W2);
   - clear the accumulators of entries that received samples;
   - evict entries left unused for N frames (start at 240 and measure VRAM against 64).
3. **Readers.**
   - `WorldLookup` uses one key with no anchor test and at most 4 probes, sized against the load
     factor from the capacity planner.
   - The per-axis bilinear feedback tap from O3 stays. It is Lumen's jittered tap.
4. **Removed:**
   - the anchor-point buffer (32 B per cell);
   - the per-cell lighting-source tracking;
   - the request queue (8 MB);
   - the active-slot list;
   - the seed, schedule and update passes and their shader programs;
   - the `WorldRequest` calls in the hit shaders.
5. **Lighting edits.**
   - The response-shortened history stays.
   - The per-cell correction for moved lights goes, because it needs an anchor.
   - Check with the lighting-edit captures before removing it.
6. **Field traversal** (devices without a ray tracing pipeline) runs the same passes, using
   `NativeTraceRay` in compute.

Cost:

- **Fixed.** The path count depends on resolution only. Motion and cold cells add no work.
- **Rays at 4K.** At most 129,600 × (3 material + 3 shadow + 1 last) ≈ 0.9 M rays per frame.
  Russian roulette lowers this.
- **Budget.** The fill plus the resolve must cost no more than today's seed, schedule, update and
  resolve, as timed in W0.

Work breakdown:

| Part | Days |
|---|---:|
| Shaders: fill passes, accumulation, resolve, readers | 3-4 |
| C++: passes, buffers, `ShaderProgram` registration, capacity planner | 1-2 |
| Removing the old paths; pass-list pins in `HybridGIPassListTests.inl` | 1 |
| Measurement and tuning: path budget, bounces, history, eviction | 1-2 |

Risks:

- **Atomic contention.** Large surfaces collect many path vertices per cell. Lumen accepts this,
  and wave reduction limits it.
- **Coverage.** Cells reached only by probe rays, and not by the 1-in-64-pixel paths, stay cold.
  Readers then use W1's fallback.
  - Coverage is comparable to today: about 390 K path vertices per frame against 430 K cell
    updates.
  - The path budget is a setting: one path per 6×6 tile instead of 8×8 gives 1.8 times the
    paths.
- **Lighting edits.** Without the moved-light correction, edits converge only through the
  shortened history.

## Measurement for every phase

- ZenApp only, with `--setup=` on `full.zenrender`. Close RTSS first. Use the venv Python.
- **Frame time.** Run the walk and turn segments of `timeline.py`. Report `frame_cpu_ms` per phase
  (start-up, turn, walk, 32 and 33-160 frames after).
- **Per pass.** Take Nsight GPU Trace through `tools/profile_hybrid_gi.py`. Report each world-cache
  pass and the probe-trace stages, plus RT-core utilization for the W3 fill.
- **Quality.**
  - Fast-motion harness, RMS after stop.
  - Settled 9-pose sums. Repeat the deciding runs, since native launches are not bit-identical.

| Phase | Walk | 32 frames after the walk | Frames 33-160 after | Settled |
|---|---:|---:|---:|---:|
| Now | 49.2 ms | 65.9 ms | 34.6 ms | 24.7 ms |
| W1 target | ≤ 34 ms | ≤ 35 ms | ≤ 28 ms | 24.7 ms |
| W3 target | ≤ W1 | ≤ W1 | ≤ 25 ms | ≤ 24.7 ms |

## Order and total

- **W0 + W1 + W2: about 3 days.** They remove most of the motion spike and do not depend on W3.
- **W3: 6-9 days.** It replaces the cache's fill with Lumen's design and is the only part that
  removes the per-cell scheduler and the anchor test.
- **Total: about 9-12 days.**

The recommended order is W0, then W1 (re-measure), then W2, then W3. W0 decides whether the
anchor test causes the long-unusable cells. If it does, W3 matters for quality as well as for
frame time.

## ZenApp measurements, 2026-11-21

The W0 diagnostic used the original scheduler on the RTX 5080 with `full.zenrender` at 3840×2160, fullscreen, hardware traversal and async compute disabled. It counted one in 64 first-hit probe lookups. Each row gives mean sampled lookups per frame. The walk starts on frame 601 in this shortened diagnostic.

| Frames | Exact | Neighbour | Absent | Incompatible | Unresolved |
|---|---:|---:|---:|---:|---:|
| Settled, 590–600 | 59,494 | 222 | 0 | 34 | 0 |
| Walk, 601–612 | 121,954 | 15,105 | 11,390 | 616 | 0 |
| After walk, 613–644 | 88,233 | 22,257 | 12,497 | 750 | 0 |
| After walk, 645–772 | 47,837 | 8,130 | 1,753 | 374 | 0 |

Absent keys accounted for 94% of failed lookups in the first 32 frames after the walk. A sampled fine surface first requested at frame 613 became usable at frame 749. The lookup-count readback ran only in the diagnostic build. Timing runs used the uninstrumented application.

### Frame-time medians

Times are `frame_cpu_ms` in milliseconds from the full 1,692-frame timeline. The settled column uses frames 1593–1692.

| Implementation | Startup 1–30 | 31–120 | 121–300 | Turn | Walk | After walk 1–32 | 33–160 | Settled |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Original, tl-o4k | 78.14 | 32.63 | 24.89 | 36.82 | 49.16 | 65.92 | 34.55 | 24.29 |
| W1, probe endpoint only | 76.81 | 32.07 | 24.79 | 37.08 | 31.70 | 32.59 | 26.33 | 24.14 |
| W1, including three-bounce updates | 76.20 | 35.70 | 24.61 | 37.05 | 31.22 | 32.55 | 26.30 | 24.08 |
| W2, parent history | 76.90 | 40.54 | 24.72 | 37.12 | 31.53 | 32.57 | 26.34 | 24.16 |
| W3, before startup allocation tuning | 65.06 | 36.96 | 21.41 | 33.89 | 25.05 | 23.73 | 20.10 | 20.03 |
| W3, before startup allocation tuning, repeat | 65.50 | 37.04 | 21.40 | 34.11 | 24.91 | 23.80 | 20.24 | 20.08 |
| W3, six-face capacity estimate | 69.64 | 29.38 | 21.98 | 31.60 | 26.54 | 24.87 | 20.88 | 20.86 |
| W3, six-face estimate, repeat | 65.41 | 28.22 | 21.19 | 33.97 | 25.15 | 23.93 | 20.18 | 20.07 |
| W3, restored release executable | 69.03 | 28.83 | 21.67 | 34.46 | 25.31 | 24.23 | 20.74 | 20.60 |

The settled W1 Nsight trace measured 1.59 ms in `ScreenProbeTrace`. The subsequent implementation shades recorded cold hits in `ScreenProbeShadeHits` with an inline visibility query. Batches outside the record pool use the one-segment ray pipeline.

### Implemented cache layout

At 3840×2160 the default tile size of 8 launches 129,600 paths. Each tile owns 64 bytes of transient path state. A diffuse start includes the visible receiver among its three material vertices so that an isolated surface can acquire history even when no secondary ray lands on it. A glossy start first traces its GGX direction. The terminal segment applies the hit material to cached irradiance, which preserves the receiver material convention without storing outgoing color in the cell.

A persistent cell uses 212 bytes with six records or 244 bytes with the optional indirect seed. These totals include the key, directional sums, resolved history and 12 bytes of cell state. The seed has its own maturity count. Resolve evicts an unsampled cell after the configured interval. The capacity planner accounts for transient paths, storage-buffer limits, available memory and the maximum number of vertices that can arrive before eviction.

The first vertex to touch a new cell snapshots the previous frame's parent during fill. Resolve can then update all cells concurrently. A light change recorded by the GPU invalidates pre-edit global lighting at lookup, even for cells outside the view during the edit. A cell's sampling sequence continues across lighting changes so that repeatedly editing a light still explores new directions.

### Settled GPU passes

Nsight Graphics GPU Trace captured 30 frames per phase at base clocks. Values are median GPU milliseconds. Each W3 trace and shade row sums all four dispatches in the frame. The cache total is the median of the per-frame sums.

| GPU range | Original | W1 | W2 | W3 |
|---|---:|---:|---:|---:|
| WorldCacheLighting | — | — | — | 0.001 |
| WorldCacheResetRequests | 0.013 | 0.013 | 0.013 | — |
| WorldCacheSeedInitialize | 0.014 | 0.012 | 0.011 | — |
| WorldCacheSeedSelect | 0.087 | 0.086 | 0.086 | — |
| WorldCacheSeed | 0.112 | 0.104 | 0.104 | — |
| WorldCacheSchedule | 0.345 | 0.327 | 0.325 | — |
| WorldCacheUpdate | 3.666 | 3.127 | 3.162 | — |
| WorldCacheStart | — | — | — | 0.038 |
| WorldCacheTraceHits | — | — | — | 0.301 |
| WorldCacheShade | — | — | — | 0.599 |
| WorldCacheResolve | 0.356 | 0.364 | 0.363 | 0.161 |
| ScreenProbeAllocate | 0.021 | 0.021 | 0.021 | 0.014 |
| ScreenProbeSampleHits | 1.055 | 1.035 | 1.036 | 1.054 |
| ScreenProbeTraceHits | 1.084 | 1.086 | 1.086 | 1.084 |
| ScreenProbeShadeHits | 2.346 | 2.351 | 2.354 | 2.078 |
| ScreenProbeTrace | 1.053 | 1.591 | 1.581 | 0.677 |
| **All world-cache passes** | **4.591** | **4.033** | **4.068** | **1.100** |

The W3 material-ray dispatches used 47.21%, 25.31%, 12.54% and 0.65% of peak RT-core throughput in bounce order. The corresponding shade dispatches, which issue inline visibility queries, used 13.03%, 3.44%, 0.20% and 0.01%. These are 30-frame medians of Nsight's RT-core cycles executed as a percentage of peak sustained elapsed cycles. The original update measured 7.32%, W1 measured 8.68% and W2 measured 8.58%.

### Memory and eviction

With the original capacity estimate, the first W3 frame allocated 8,388,608 slots and later shrank to 2,097,152. Peak device-local allocation was 11,943.6 MiB. Estimating six geometric faces per spatial patch gave 4,194,304 slots from startup through the end of both final runs. The six-record table used 848 MiB plus its 928-byte lighting header. Tile paths used 7.91 MiB.

The original renderer peaked at 10,823.6 MiB. Both final 240-frame runs and the 64-frame eviction run peaked and settled at 10,711.6 MiB. Eviction frees a slot for another key inside the allocated table. On this scene, changing the eviction interval made no difference to allocated VRAM. The 64-frame run measured 24.72 ms during the walk, 23.75 ms over the first 32 frames after it, 20.10 ms over frames 33–160 and 20.08 ms settled. The default remains 240 frames.

The 6×6 tile setting launched 230,400 paths. It measured 25.15 ms during the walk, 24.29 ms over the next 32 frames, 20.61 ms over frames 33–160 and 20.55 ms settled. With the default 8×8 tiles and a 16-update history, those medians were 24.80, 23.68, 20.20 and 20.06 ms. These were timing sweeps. The image comparisons below use the default 128-update history.

### Image comparisons

The nine-pose harness compares each moving frame against that implementation's settled image at the same pose. RMS is in 8-bit RGB levels. The threshold columns sum the percentages of pixels whose largest absolute channel error exceeded 4 or 16 levels. The light-edit column measures RMS against the settled frame with the lamp in its new position.

| Implementation | Nine-pose RMS sum | Above 4 sum, percentage points | Above 16 sum, percentage points | Light edit at frame 16 | Light edit at frame 128 |
|---|---:|---:|---:|---:|---:|
| Original scheduler replay | 12.1691 | 2.1121 | 0.3140 | 0.575 | 0.432 |
| W1 | 12.1664 | 2.0973 | 0.3136 | 0.578 | 0.438 |
| W2 | 12.1757 | 2.1013 | 0.3144 | 0.577 | 0.437 |
| W3 | 12.0927 | 1.8866 | 0.3014 | 0.630 | 0.413 |
| W3 repeat | 12.0817 | 1.8752 | 0.3021 | 0.635 | 0.418 |

W3 showed more error during the first 32 frames of the lamp translation and less at frames 64 and 128. Both W3 runs lowered the nine-pose RMS sum and the two threshold sums.

The forward-stop score averages RMS at 1, 2, 4 and 8 frames after the 12-frame walk. With W1's settled images, the original scheduler scored 0.742, W1 scored 0.763, W2 scored 0.690 and W3 scored 1.049. With W3's settled images, W3 scored 0.643 and 0.641 on repeat. At stop frame 64 the W3 result was 0.440 RMS from its own reference and 0.923 from W1's. Across the corresponding base and nine settled pose images, both W3 runs averaged 0.393 of an 8-bit level darker than W1. The first W3 run averaged 0.408 of a level darker than the original scheduler replay. The cross-version RMS therefore includes a change in settled illumination.

### Validation and capture inputs

The cache calibration runs the 32-bit and 64-bit key paths on Vulkan. It checks opposite faces, two observations sharing a cell, directional reconstruction, capped parent inheritance, seventh-record maturity and eviction. The lighting fixture toggles an unannounced directional light and analytic lighting, moves the camera, and checks recovery and retained history after a cut. At 65×65 the 81-path budget sometimes missed its 5% restored-light tolerance. The regression now uses an odd 129×129 viewport with 289 paths and the same thresholds and frame windows.

ZenApp also ran 128 frames at 512×288 with validation enabled, compute tracing, screen probes, no hardware ray features and async compute disabled. Its frame summary reported field traversal, 2,880 screen probes and 4,194,304 cache slots. All 237 VoxelGI shader modules with corresponding live source passed SPIR-V validation. Separate validated hardware runs covered both the six-record screen-probe path and the seven-record per-pixel seed path. The selected release suites passed 111 tests across runtime launch (22), configuration (20), render-core (37) and Vulkan integration (32).

The original scheduler's quality measurements used an in-place rebuild of ZenApp from the archived pre-change renderer sources. Each of its 732 SPIR-V binaries matched the original profiling manifest's SHA-256. The rebuild has its own executable identity and both motion and nine-pose manifests record it.

After the original scheduler replay, the canonical W3 release executable completed the 1,692-frame timeline and both targeted Vulkan calibration tests passed. Its shader-set hash matches the earlier W3 timing runs. Peak and settled allocation again measured 10,711.6 MiB. [Restored executable and shader hashes](../build/checks/native-gi/world-cache-plan/w3-restored-inputs.json), [frame summary](../build/checks/native-gi/world-cache-plan/w3-restored.summary.json) and [calibration results](../build/checks/native-gi/world-cache-plan/w3-restored-calibration.log).

The 4K timeline and trace used the release build of `ZenApp.exe` and `full.zenrender`. The timeline input manifest records the exact arguments, executable, shader set, setup, camera path, scene and texture hashes. Each run's frame summary records both actual extents, backend, fullscreen and async-compute state. The quality harnesses retain their own derived setups and capture manifests.

- [Original 4K Nsight trace summary](../build/checks/native-gi/fps-20261120/o4k-static/summary.json)
- [W0 diagnostic input manifest](../build/checks/native-gi/world-cache-plan/w0-inputs.json) and [sampled outcomes](../build/checks/native-gi/world-cache-plan/w0.log)
- [Original shader hash comparison](../build/checks/native-gi/world-cache-plan/baseline-replay/verification.json), [original forward-stop scores](../build/checks/native-gi/world-cache-plan/w0-replay-motion/report.json) and [original nine-pose scores](../build/checks/native-gi/world-cache-plan/w0-replay-quality/native/report.json)
- [W1 trace](../build/checks/native-gi/world-cache-plan/w1-static/summary.json) and [W2 trace](../build/checks/native-gi/world-cache-plan/w2-static/summary.json)
- [Final timeline input manifest](../build/checks/native-gi/world-cache-plan/w3-sized-inputs.json), [actual frame state](../build/checks/native-gi/world-cache-plan/w3-sized.summary.json) and [Nsight GPU Trace summary](../build/checks/native-gi/world-cache-plan/w3-sized-static/summary.json)
- [Compute traversal validation state](../build/checks/native-gi/world-cache-plan/w3-sized-field.summary.json) and [SPIR-V validation results](../build/checks/native-gi/world-cache-plan/w3-spirv-validation.json)
- [Forward-stop captures and scores](../build/checks/native-gi/world-cache-plan/w3-sized-motion/report.json) and [repeat](../build/checks/native-gi/world-cache-plan/w3-sized-repeat-motion/report.json)
- [Nine-pose and lamp-edit scores](../build/checks/native-gi/world-cache-plan/w3-sized-quality/native/report.json), [repeat](../build/checks/native-gi/world-cache-plan/w3-sized-repeat-quality/native/report.json) and [paired W1 reference comparisons](../build/checks/native-gi/world-cache-plan/w3-shared-reference.json)

## Native 4K startup

The W3 scheduler could request 33 batches of 16 directions for a cold probe with specular lighting enabled. A mature probe requested two. Cold and globally invalidated probes bypassed the motion budget, and the final ray-generation pass retraced directions that did not fit in the hit buffer. On the first few Sponza frames, that pass alone could take over 100 ms.

Lumen controls screen-probe work through the probe grid spacing, a quality-dependent tracing resolution and a capped allocation for adaptive probes. ZenEngine now applies a per-probe ceiling from the first frame, including after camera cuts and light changes. A young probe gets up to 48 directions with specular lighting enabled, a mature probe gets 32, and diffuse-only probes get 80. Hardware tracing reserves a hit record for every scheduled direction and accumulates the results in compute. Field traversal follows the same sampling budget. The 4K setup retains its native render and GI extents. [Lumen Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-performance-guide-for-unreal-engine)

ZenApp prepares the environment, static ray-tracing geometry, scene pipelines and empty probe histories during loading. Its first submitted scene frame starts with fresh temporal history. Vulkan requests one more swapchain image than the number of frames in flight, within the surface's supported range. On this setup a third image removed a roughly 65 ms wait in the second Vulkan present.

### Frame time

The table uses ZenApp's measured CPU frame-loop time, including submission waits, on the same 1,692-frame Sponza camera path. The release executable ran fullscreen on an RTX 5080 at 3840×2160, with hardware tracing, two frames in flight, validation enabled, async compute disabled and VSync off. Ranges show median frame time.

| Scene frames | W3 before (ms) | Startup fix (ms) |
|---|---:|---:|
| 1 | 210.40 | 12.25 |
| 2 | 370.86 | 29.06 |
| 3–8 | 141.84 | 23.91 |
| 10–30 | 66.53 | 23.84 |
| 35–60 | 38.27 | 23.07 |
| 70–100 | 27.24 | 23.72 |
| 601–640, camera turn | 34.46 | 25.76 |
| 1281–1292, forward walk | 25.31 | 22.54 |
| 1593–1692, settled | 20.60 | 20.43 |

The first eight frames consumed 185 ms of frame time, down from 1,448 ms. Two additional static launches measured 184–185 ms for the same eight frames. In the matched timeline, the interval from the scene inventory log to the first render log was 6.11 s before and 6.08 s after. The part before the loaded message grew by 0.17 s. Peak engine-reported GPU allocation fell from 10,712 to 10,356 MiB. [Original frame log](../build/checks/native-gi/world-cache-plan/w3-restored.rdg.log), [fixed frame log](../build/checks/native-gi/world-cache-plan/startup-fixed.rdg.log), [launch manifest](../build/checks/native-gi/world-cache-plan/startup-fixed-inputs.json), [actual render state](../build/checks/native-gi/world-cache-plan/startup-fixed.summary.json)

A separate 45-frame Nsight Graphics capture started with zero capture delay and the GPU locked to base clocks. Its first captured GPU frame measured 30.79 ms, its median was 30.55 ms and its maximum was 31.69 ms. Screen-probe accumulation took 0.73 ms in the first captured frame. [Nsight GPU Trace summary](../build/checks/native-gi/startup-fps/final-trace/summary.json)

With VSync enabled and validation disabled, a separate native-4K launch measured 12.19 ms and 31.62 ms for the first two scene frames, then 23.55 ms median over frames 3–100. The final in-place rebuild reproduced the measured executable and shader hashes. [VSync frame log](../build/checks/native-gi/startup-fps/final-vsync/startup.rdg.log), [build identity check](../build/checks/native-gi/startup-fps/source-verification.json)

### Startup image quality

The first frame had 3.275 RGB levels of RMS error against its own settled frame, up from 2.248 on the 8-bit output. At frame 8 the error was 1.212, and at frame 32 it was 0.622. Frame 8 now fits within the time the original renderer spent on frame 1. Comparing the two frame-512 images directly gave 0.190 RMS and a mean signed difference of −0.002 levels. Separate capture runs supplied these images so screenshot readback would not affect the frame-time comparison. [Original image scores](../build/checks/native-gi/startup-fps/before/quality.json), [fixed image scores](../build/checks/native-gi/startup-fps/final-quality/quality.json), [capture inputs](../build/checks/native-gi/startup-fps/final-quality/inputs.json)

Across the nine moving camera poses, the RMS sum rose from 12.093 to 12.525. The average percentage of pixels with an error above four levels rose from 0.210% to 0.397%. After the lamp moved, RMS at frames 16 and 128 rose from 0.630 and 0.413 to 0.667 and 0.426. Each measurement uses that version's settled image at the same camera and light position. [Original motion and lamp scores](../build/checks/native-gi/world-cache-plan/w3-sized-quality/native/report.json), [fixed motion and lamp scores](../build/checks/native-gi/startup-fps/final-gates/native/report.json)

The release test suites passed 22 runtime launch, 20 configuration, 37 render-core and 32 Vulkan integration tests. A separate validated compute-only ZenApp run completed 128 frames and reported field traversal with 2,880 screen probes. All 483 VoxelGI SPIR-V modules with live source, including compiled variants, passed validation. [Test results](../build/checks/native-gi/startup-fps/final-status.log), [compute-only render state](../build/checks/native-gi/startup-fps/final-field/startup.summary.json), [shader validation](../build/checks/native-gi/startup-fps/final-spirv-variants.json)
