# HW radiance cache experiment (2026-11-14)

The first [Phase 2](../GIRefinementPlan.md) candidate is selected by `radianceCache` in a `.zenrender`
setup or `voxel_gi_radiance_cache` in configuration. It is off by default and requires HW RT
pipelines. Ray queries, Compute, reference sampling, unavailable memory budgets and zero capacity
retain exact shading. This is not an accepted replacement. Phase 3 and later dependent work have
not been implemented. **Disposition: rejected by the Phase 2 cost gate.** The user requested stopping
after the current phase on 2026-11-14; work stops after this evaluation, with all options still off.

## Implementation

Three persistent buffers use the normal render-graph import/dependency tracking: 8-byte keys,
16-byte fixed-point accumulation, and 16-byte resolved records. Compact 32-bit keys retain padding,
so both capability paths use 40 bytes per entry. Vulkan enables buffer int64 atomics only together
with shader int64, following the [feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceShaderAtomicInt64Features.html).

Capacity is a power of two bounded by scene surface-area demand, storage-buffer limits and one
quarter of free budget headroom after a ten-percent reserve. Allocation alignment is included.
There is no floor. Live caches shrink under pressure; growth waits for a history reset. Allocation
failure releases partial resources and retries smaller capacities down to zero. Capacity and key
width are logged. Cell density scales with the bounds because renderer world coordinates are
normalized. The Sponza pilot selects 2,097,152 entries (80 MiB payload).

Keys contain hashed quantized position, a logarithmic camera-distance level and a three-bit dominant
normal direction. Eight linear probes include entries beyond eviction holes. Insertion searches
for an existing key before claiming a hole, avoiding duplicate means for one key.

`GICacheUpdate` traces from one different texel of each 4x4 GI block per frame. The existing exact
hit-lighting function supplies diffuse irradiance divided by pi, before reflectance and emission.
Accumulation uses 16 fractional bits, at most 255 samples per entry per frame, and invalidates
values outside its encoding range. Resolve stores half RGB, blends per-frame means to the history
cap, evicts after 120 stale frames, resets on response hints, and resets after 32 consecutive
updated frames with declining luminance. New entries at a camera-distance level transition may
blend the adjacent level's previous value.

History invalidation clears all buffers before update. Initialization remains pending after a
declined submission; failed submissions release the cache with the other histories. A query
requires a segment longer than the cell diagonal, a sufficiently wide reflection cone, distance
from local lights, and a populated entry. It remodulates by the actual hit reflectance and adds
actual emission. ReSTIR candidates consume that value; reservoirs and reuse visibility are unchanged.

The `GI radiance cache` debug output shows hash cells and occupied slots. Ray-count diagnostics
add query-hit-rate and trace-count faces. Per-texel dumps also contain cache queries and hits.
Counts retain their existing restriction: native-resolution GI and no ReSTIR. They are not the
shipping half-resolution ReSTIR workload.

## Evidence and qualification

`build/checks/gi-refinement-phase0/cache-integration.log` passes 16 selected native cases across
four submission modes, with validation and no reported leaks. Coverage includes both key widths,
negative-coordinate quantization, collisions, contention, stale eviction, resets, a directional-light
calibration mean within 0.1% of exact shading, and deliberate cache-on pass-list pins.
`cache-budget-test.log` covers pressure and allocation alignment.

The focused Sponza HW fast-pan pose-32 pilots are in `build/checks/gi-refinement-phase2/quality`,
with exact setups/commands, executable and input identities, actual render state, lossless images
and metrics. These are not the complete gate matrix.

| Candidate | Pixels above 4/255 | RMS /255 |
|---|---:|---:|
| Paired cache-off baseline | 0.28320313% | 0.63736508 |
| Cache only | 0.28757957% | 0.63859113 |
| Cache plus young receivers | 0.25673949% | 0.57480224 |

Cache alone regresses this pan measure. Cache plus young receivers still misses 0.1994%, and is
worse than the earlier young-only pilot. Neither qualifies a default.

Repeated Nsight warm launches are under `build/checks/gi-refinement-phase2/timing`, captured through
`tools/profile_hybrid_gi.py` using canonical ZenApp, base clocks and no RTSS. Three paired launches
measure the baseline trace at 8.32432–8.33928 ms; the initial cache candidate's trace/update/resolve
sum is 9.43169–9.45578 ms. Whole-frame medians are 32.99455–33.39205 ms off and 33.95030–34.02785 ms on.

The final candidate also fixes insertion past an eviction hole, applies the reflection-footprint
rule when diffuse rays also estimate reflection, and matches sparse-update normal-map policy to
the active diffuse path. Two more warm launches in `timing-final` measure:

| Run | HybridTrace | Update | Resolve | Sum | Whole frame |
|---|---:|---:|---:|---:|---:|
| Final 0 | 9.175235 | 0.214816 | 0.121920 | 9.511971 | 34.036200 |
| Final 1 | 9.175390 | 0.214800 | 0.121984 | 9.512174 | 34.062450 |

All times are ms. The sum is the plan's replaced-pass cost metric; whole-frame GPU time is measured
separately. This fails the requirement to beat the baseline trace, and the plan explicitly rejects
Phase 2 in this case. (Review correction: these captures do not use the accepted-baseline camera, so
the 33.018 ms frame budget does not apply to them. At this view the cache adds 0.6–1.0 ms of frame
time against the cache-off launches. See [Review](#review-2026-11-14).) The candidate remains available for
investigation behind its default-off setting; it is not accepted for dependent features.

`final-checks` records validation-enabled canonical ZenApp runs. The 8 GiB heap-cap emulation on the
RTX 5080 initially chooses 1,048,576 entries (40 MiB), then shrinks to zero as frame allocations
settle, and completes 64 frames without errors. This is a constrained run, not a physical 8 GB GPU
measurement or a cache-off pixel-identity proof. The warmed native-resolution/no-ReSTIR count run
reports 40,565,504 queries and 38,634,691 hits (95.2403%) on frame 128. That hit rate must not be
substituted for the profiled half-resolution ReSTIR workload. Hash-cell, occupancy, and denoiser
rejection debug outputs also render with validation; the occupancy image was visually inspected.

Final builds include ZenApp and zen_editor. Verification logs under `gi-refinement-phase0` record
784 passing RenderCore cases (9 disabled), 132 passing ray/field cases (4 fixture skips, 12 disabled),
52 passing denoiser cases, and 11 passing Python cases. No native validation errors or memory leaks
were reported. The 410-module accepted-baseline comparison has no non-count/non-capture production
shader differences. The final additional Compute hit-light counter classification fix compiles;
it changes diagnostic ray categories only.

Full one-bounce reference bias, static flicker/sparks, both pan speeds, lighting recovery and the
editor/Compute quality matrix were not completed after the cost rejection. These gates are
unqualified, not passed. No Phase 3 result, multi-bounce engine feature, reduced-sample default or
new accepted baseline is claimed.

## Review (2026-11-14)

An independent review of the Phase 0–2 work above. **The cost-gate rejection stands.** The
measurements are sound for the relative on/off comparison. No defect was found that would make the
cache look slower than it is. Evidence is in `build/checks/gi-refinement-review`.

**Why the cache did not pay.** Nsight counters from the same captures
(`build/hybridtrace-optimization/finish-20261112/occupancy.py`, medians over 30 frames):

| `HybridTrace` | ms | RT-core active, % of peak | RT-core time, ms | Local-memory sectors | Warps/SM | RT shared bytes/thread |
|---|---:|---:|---:|---:|---:|---:|
| Cache off (`timing/baseline-0..2`) | 8.33 | 38.9 | 3.24 | 3.77e9 | 14.37 | 70.3 |
| Cache on, final (`timing-final/cache-0..1`) | 9.18 | 25.8 | 2.37 | 2.95e9 | 13.22 | 76.6 |

Both use 128 registers per thread. The cache removed 27% of the RT-core work and 22% of the
stack (local-memory) traffic, but the pass got 0.85 ms longer. This pass is limited by SM-side work,
not by the RT units.
- Occupancy fell 8%. The cache pipeline needs about 6 more bytes of per-thread RT storage (the
  payload's cone field and the query state), and that storage is the warp limiter here.
- Likely but unmeasured: closest-hit lanes that miss the cache still issue the shadow trace, so a
  warp skips the nested trace only when every lane hits. At the 95% per-lane hit rate of the
  count run, that is about 0.95^32 ≈ 19% of warps.

A cache pays here only if misses leave closest-hit, for example deferred to a compacted follow-up
pass (Phase 5 item 3). That is new work with its own cost to measure.

**Corrections to the record**
- The timing captures use camera target (0.862, −0.005, −0.425); the accepted baseline uses
  (0.912, −0.005, 0.114). The relative on/off gate is valid. The 33.018 ms frame budget does not
  apply at this view, where cache-off frames already measure 33.0–33.4 ms.
- The final candidate's two launches have no fresh cache-off pair. The 8.324–8.339 ms baseline
  comes from the first timing batch, which used an earlier executable and shader set.
- The plan overstated the possible savings. The accepted baseline has environment lighting off, so
  its no-guide trace has no sky ray at hits. A cache hit can save at most one shadow ray, and only
  when the light-occluder cache did not already skip it.
- Spec deviation, recorded rather than changed so that the code matches the measured candidate: the
  reflection-footprint rule is `segment × roughness² > cell`, and it also applies to diffuse rays
  that feed the rough reflection estimate. SHaRC applies no footprint test to diffuse rays. For
  specular lobes it uses `2·segment·sqrt(0.5α²/(1−α²))` with α = roughness², which accepts segments
  1.4–2.4× shorter at roughness 0.5–0.9. Even a perfect per-lane hit rate does not address the
  limits above.

**Verification added by the review**
- The full native suites now pass with the current Release build: RenderCoreTest 784 (9 disabled),
  ConeVoxelGIIntegrationTest 168, RayQueryIntegrationTest 132 (12 disabled), VulkanRHIIntegrationTest
  510, VulkanRHITest 95, CommonTest 51, EditorRenderingTest 77, RuntimeLaunchTest 21,
  ConfigLoaderTest 20, and the Python tool tests (19). The earlier counts of 52 denoiser and 16
  cache cases were filtered subsets of ConeVoxelGIIntegrationTest and RayQueryIntegrationTest.
- Every earlier ZenApp run used the HW RT pipeline with ReSTIR and environment lighting off. The
  Vulkan backend fails a pass whose declared descriptors are not all bound, and the mock-RHI tests
  do not check that. `variant_matrix.py` therefore ran 48 validation-enabled 4K frames in nine
  configurations:
  - RT pipeline, environment lighting on and off, with and without ReSTIR;
  - ray queries, environment lighting on and off;
  - Compute, environment lighting on and off.

  Each run enabled the cache, young receivers, bicubic history, anti-lag and denoiser validation
  together; one RT-pipeline run enabled the cache alone. All nine finished with no validation,
  descriptor or binding errors. The RT pipeline allocated 2,097,152 entries and cleared them once,
  on the first frame. Ray queries and Compute logged `enabled=false` and kept exact shading.
- Fixes:
  - eleven working files that had been saved with CRLF are back to LF, as `.gitattributes`
    requires (content unchanged);
  - the cache's size log now prints only when the request or the chosen size changes, instead of
    on every 32-frame re-plan.
