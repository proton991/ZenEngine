# Native 4K cached GI plan (2026-11-15)

## Goal

**Target:** New Sponza at 3840x2160, GI resolved per pixel at full resolution, 60 fps (16.7 ms per
frame) on the RTX 5080 with clocks locked to base. The HW RT tier comes first. No upscaler is
assumed, but render and output resolution stay separate so DLSS, FSR or MetalFX can be added later.

**Why the current design cannot reach it.** Today's GI traces rays per pixel, reuses them with ReSTIR
and denoises them per pixel. Its cost scales with pixel count. Measured with GI at full 4K
(`build/checks/native-gi/baseline`, Nsight, base clocks):

| Stage | ms |
|---|---:|
| `HybridTrace` | 33.1 |
| ReSTIR (temporal and four spatial passes) | 24.5 |
| Denoiser (eight passes) | 26.4 |
| **GI total** | **84.0** |
| Direct-light visibility (`ReceiverVisibility`) | 4.65 |
| Forward translucency, G-buffer, lighting, sky, other | 4.5 |
| **Frame** | **93.2** |

That is 5.6x over budget. Even with the whole 16.7 ms frame, GI at full 4K would have about 2 ns per
pixel; today it spends about 10 ns. No tuning closes that, so the GI architecture changes instead.

## Architecture

Rays are traced at a much lower density into caches that persist across frames. Each pixel does only
a cheap gather from those caches, with its own normal. Lumen, DDGI and surfel GI share this shape.
This is an independent implementation from published descriptions; no SDK source is copied.

```text
G-buffer (4K) ──> screen probes (1/16) ──trace──> world radiance cache ──feedback──┐
                        │                                  ^                        │
                        │                                  └── cache update rays ───┘
                        v
        per-pixel gather at 4K (diffuse + rough specular) ──> lighting
glossy pixels only ──> reflection rays ──> cache at hit ──> glossy denoise ──> lighting
```

### Layer 1: world radiance cache (WRC)

- **Structure:** a sparse hash grid of world-space cells, sized by distance to the camera. The
  rejected [HW radiance cache experiment](HybridGI/RadianceCache.md) already provides keys, linear
  probing, eviction, budget-bounded capacity and invalidation, and they are reused.
- **Payload:** irradiance per cell in a few directions (L1 spherical harmonics or six axes), so a
  lookup can be evaluated for any normal.
- **Update:** a fixed ray budget per frame (about 0.5–1 M rays) from cell sample points, spread over
  recently used cells. Hits read the cache itself, so light gains a bounce every frame. That closes
  today's 57–62% one-bounce deficit against the 16-vertex CUDA reference.
- **Direct light at cache points:** one sampled, shadowed light per update ray, as the trace does
  today.

### Layer 2: screen probes

- **Placement:** one probe per 16x16 pixels on visible surfaces (240x135 = 32,400 at 4K). Extra
  probes go where interpolation fails, at depth and normal discontinuities, up to about 25% more.
- **Tracing:** 64 rays per probe in an 8x8 octahedral layout, about 2–2.6 M rays, similar to today's
  1080p trace. Rays are importance-sampled from the probe's previous radiance and the cosine lobe.
- **Hit shading:** a cache lookup times the hit's albedo plus its emission. There are no shadow rays
  at hits and no material evaluation beyond albedo and emission, which is where today's trace spends
  most of its time.
- **Temporal:** probes are anchored to world positions, so history survives camera rotation. This
  removes, at the design level, the history resets and sparks fought in
  [Denoiser.md](HybridGI/Denoiser.md).
- **Filter:** a small spatial filter across neighboring probes, weighted by plane distance and
  normal, then conversion to irradiance and a prefiltered radiance mip for rough specular.

### Layer 3: per-pixel gather at 4K

- **Diffuse:** each pixel interpolates its nearest probes, weighted by plane distance and normal,
  and evaluates their irradiance with its own normal-mapped normal. Bump detail is kept at full
  resolution, unlike today's half-resolution point samples.
- **Rough specular** (roughness at or above about 0.4): the probes' prefiltered radiance,
  integrated over the pixel's GGX lobe.
- **Cost:** about four probe reads per pixel, 8.3 M pixels.

### Glossy reflections

- **Scope:** only pixels below the roughness threshold trace a reflection ray. Hits are shaded from
  the cache, with exact shading kept for mirror-like surfaces.
- **Reuse:** the current ReLAX-style reflection path (temporal, history fix, spatial) is kept for
  this signal only. ReSTIR reflection reuse stays only if it pays for itself.

### Near-field detail

Probes 16 pixels apart miss contact occlusion in creases and under small objects. If Phase 2 shows
that loss against the reference, short-range rays at reduced resolution add contact occlusion.

## Frame budget (base clocks, 4K)

| Stage | Today, full 4K GI | Target |
|---|---:|---:|
| G-buffer, culling, HiZ | 1.6 | 1.6 |
| Direct-light visibility | 4.65 | 1.5 |
| WRC update and resolve | — | 1.5 |
| Screen probe placement and trace | 33.1 (per-pixel trace) | 3.0 |
| Probe temporal and spatial filter | — | 0.8 |
| ReSTIR | 24.5 | 0 for diffuse |
| Per-pixel gather (diffuse and rough specular) | — | 1.5 |
| Glossy reflections (rays and denoise) | in trace and denoise | 2.0 |
| Other denoising | 26.4 | 0.5 |
| Lighting, translucency, sky, tone map | 2.9 | 2.9 |
| **Frame** | **93.2** | **15.3, with 1.4 ms margin** |

The trace target assumes hit shading by cache lookup is several times cheaper per ray than today's
exact hit lighting. Phase 0 measures that before anything else is built.

## Phases

Each phase ends with a measurement and a decision. Verification follows the
[measurement protocol](GIRefinementPlan.md#measurement-protocol) and stays lean: only GI quality and
cost, the native tests of the shaders touched, and one real launch per new shader program.
`tools/gi_quick_gates.py` runs A/A only on request and measures known baselines with `--timing-only`.

### Phase 0: ray cost and gates (about 1 day)

- **Microbenchmark:** trace 2.6 M probe-pattern rays on New Sponza with hit shading by cache lookup
  only. This is the plan's key assumption: if the rays alone exceed about 3 ms, the probe density
  or ray count changes before Phase 2.
- **Gates for every later phase:**
  - frame time static and while panning, at base clocks;
  - static error against both CUDA references (one-bounce and 16-vertex multi-bounce);
  - pan, dolly and light-recovery error against the converged image;
  - moving sparks and flicker;
  - VRAM.
- **Baseline:** today's numbers at full 4K GI, above.

### Phase 1: world radiance cache (about 1–2 weeks)

- **Build:** cell payload, update scheduling, multi-bounce feedback, resolve, and a debug view that
  renders GI from the cache alone.
- **Gates:** update and resolve within 1.5 ms. The cache-only image reaches the 16-vertex reference
  energy within a tolerance set from the reference's own half-to-half spread. Light-change response
  converges within a stated number of frames.

### Phase 2: screen probes and per-pixel diffuse (about 2–3 weeks)

- **Build:** placement, adaptive extra probes, importance-sampled tracing with cache hits, probe
  temporal and spatial filtering, irradiance conversion, and the 4K gather. It replaces the diffuse
  part of `HybridTrace`, diffuse ReSTIR and the diffuse denoiser passes.
- **Gates:** diffuse GI within 5.3 ms in total (probes 3.0, filter 0.8, gather 1.5). Static error at
  or below today's against both references; motion error, sparks and flicker at or below today's.
- **Decision:** probe density 1/16 or 1/8, from the cost and leak measurements.

**2026-11-18 qualification priority.** Establish image quality first. Keep the original timing
figures as references and defer timing qualification. The one- and multi-bounce CUDA comparisons,
static consistency, camera motion, light recovery, flicker and spark checks still apply.

**2026-11-19 acceptance tolerance.** The user accepts a result within 1% of its limit
(relative) as passing. Reports keep the raw values; larger misses remain open.

**2026-11-20 Phase 2 acceptance tolerance.** The user accepts up to 3% above each
individual Phase 2 limit. A zero baseline still requires zero. Qualification keeps the
raw measurements and the frozen retained-renderer reference.

### Phase 3: reflections (about 1–2 weeks)

- **Build:** rough specular from probes; glossy rays only below the threshold; cache hit shading;
  the reflection denoiser on glossy pixels only.
- **Gates:** reflections within 2.0 ms; specular error at or below today's in a glossy-heavy view.
- **Decision:** the roughness threshold and the glossy ray density.

### Phase 4: near-field occlusion (only if Phase 2 needs it)

- **Measure first:** error in contact regions (creases, under props) against the reference.
- **Build:** short-range rays at reduced resolution, within the remaining margin.

### Phase 5: direct-light visibility (about 1 week)

- **Today:** 4.65 ms of traced shadows at every 4K pixel.
- **Options:** half-resolution shadow rays with a filter, or shadow maps for the dominant lights
  with traced shadows for small ones.
- **Gate:** within 1.5 ms with no visible shadow-edge regression.

### Phase 6: coverage and closure (about 2 weeks)

- **Scenes:** animated and skinned objects (cache invalidation, probe disocclusion), an outdoor
  sky-lit scene, emissive-heavy views, camera cuts, and lighting changes with no CPU hint.
- **Other tiers:** Compute, with probes traced against the distance field and the cache shared;
  and the editor preset.
- **Memory:** a VRAM budget per resource. Today's 4K Sponza run peaks at 9.9 GB.
- **Old path:** the per-pixel trace, ReSTIR and ReLAX stay as a reference or quality mode, or are
  retired; the user decides.

## Kept from today

- G-buffer and receiver guides, the HW RT pipeline and its acceleration structures.
- The hash-cache machinery from the radiance-cache experiment.
- The reflection half of the ReLAX-style denoiser, for glossy pixels.
- `tools/gi_quick_gates.py`, `tools/profile_hybrid_gi.py` and the CUDA references.

## Open decisions for the user

1. **Cache structure.** Recommended: a hash grid, which is sparse, follows geometry and already
   exists. DDGI irradiance volumes leak through thin walls and use a fixed grid; surfels need more
   management code.
2. **Probe density.** 1/16 (recommended start) or 1/8, decided at the end of Phase 2.
3. **Glossy threshold and ray density.** Phase 3 selected 0.4 and a 4×4 receiver grid with one mirror ray or three GGX samples per eligible receiver.
4. **The old per-pixel path.** Keep it as a reference or quality mode, or retire it, decided in
   Phase 6.

## Implementation record (updated 2026-11-20)

**Implementation and qualification are separate.** The cache, native diffuse gather, rough and
glossy reflections, shadow reuse, Compute path, editor preset and gate runners are implemented.
Phase 1 implementation and its timing, reference-energy and controlled light-response gates
passed on the recorded qualification build below. Phase 2 changes the cache surface keys,
scheduling and history, so that historical result does not qualify the current build. Full-scene cache diagnostics still show
substantial spatial error under memory pressure; they do not establish visual convergence.
The failed later-phase gates remain requirements; a frame below 16.7 ms does not qualify the
complete plan. The retained pixel renderer remains available.

On 2026-11-20 the `guided` build passed all 15 Phase 2 diffuse image checks in two
launch pairs and in their crossed comparison, both probe CUDA comparisons, the glossy
checks and the 28 targeted GPU tests. Phase 2 timing remains open, and the cache-only
sixteen-bounce energy straddles its limit across two launches (−0.695% and −0.636%
against ±0.659%).

### Phase 2 diffuse reconstruction and lamp edits (2026-11-20)

The 4K New Sponza diffuse fixture compares camera motion against independently held
poses. The frozen retained report supplies each limit. Counts below apply the
3% tolerance separately to the fifteen checks.

Each diffuse ray now contributes to the angular lighting estimate used to shade
normal-mapped receivers. Previously only the first sixteen rays affected the directional
moments. Adaptive placement scores the area each candidate can cover, and the filter
and gather read neighboring adaptive probes. The world cache integrates the geometric
hemisphere and corrects historical direct irradiance by the signed difference of the
old and new shadowed lamp contributions when a finite light moves.

| Full 4K launch | Static RMS | Lamp RMS, +128 | Slow pan, +8 | Fast pan, +8 | Dolly, +8 | Checks within tolerance |
|---|---:|---:|---:|---:|---:|---:|
| Allowed ceiling | 0.18358 | 0.23653 | 0.00934% | 0.01244% | 0.00925% | 15 |
| Direct-light correction | 0.18030 | 0.21783 | 0.05228% | 0.00729% | 0.00963% | 13 |
| Measured lamp-footprint sampling | 0.18022 | 0.23459 | 0.01209% | 0.00852% | 0.01027% | 12 |
| Guided secondary paths and demand-weighted cache updates | 0.16403 | 0.22028 | 0.00699% | 0.00696% | 0.00971% | 14 |
| Projected probe coverage and interpolated cache feedback | 0.16225 | 0.22595 | 0.00752% | 0.00690% | 0.01081% | 14 |
| Revised adaptive-anchor retention and local-light sampling | 0.16215 | 0.21561 | 0.07458% | 0.02810% | 0.00813% | 9 |
| Reprojected lighting history and surface-centered light proposals | 0.16363 | 0.22474 | 0.05418% | 0.01384% | 0.00678% | 9 |
| Stable anchors and variance-weighted reprojected history | 0.16243 | 0.22564 | 0.00785% | 0.00791% | 0.00774% | 15 |

The first two launches had zero stationary and moving sparks. In the first launch, three
64×64 blocks in the held slow-pan view accounted for 3,447 of 4,336 differing
pixels. The second launch had a separate dolly failure at frame 64. Across three
launches, 424 pixels repeatedly failed dolly at frame 8, including narrow moldings
and column edges. These captures distinguish intermittent bright cache patches from
repeatable geometry-dependent reconstruction errors.

The raw reference and moving frames, ZenApp arguments, executable and shader hashes,
framebuffer and GI extents are in `build/checks/native-gi/p2-root-20261120/`.
`relight/report.json`, `angular/report.json` and `continuation/report.json` retain the original gate values.

In the third run, 805 pixels exceeded four display levels eight frames into the
dolly. The 3% ceiling permits 767 pixels at 3840×2160. The renderer used all
4,194,304 cache cells, with hardware tracing, fullscreen output and async compute
disabled. All 28 targeted GPU integration tests passed. The separate glossy
fixture measured 1.46322 RMS against its retained 1.42706.

The 30-frame base-clock Nsight pan trace measured 52.356 ms median for diffuse GI.
Placement, proposal and tracing together took 48.417 ms, filtering took 1.588 ms
and gathering took 2.137 ms. Each median is calculated from the per-frame sum
of its stages. ScreenProbeTrace alone took 44.77 ms. The report and the trace
are in `continuation-qualification/pan/`. The Phase 2 timing target is 5.3 ms.

The fourth run also missed only the eight-frame dolly, with 897 affected pixels.
Both runs repeatedly lit a narrow groove around pixel (800, 1600) more strongly
during translation than in its independently settled view. The frozen diffuse
fixture has environment lighting disabled. The separate sharp-turn fixture uses
the HDR environment, normal maps and reflections.

Single-precision sampling of the narrowest local-light lobe (g = 0.999) rounded
3,956 of 65,536 radial samples onto its axis. Computing the transverse component
directly preserves all 65,535 off-axis samples. The numerical comparison is in
`hg-fp32.json`. The cache's GPU test checks the resulting angular separation.

The fifth run passed the eight-frame dolly with 674 pixels. It failed six other
motion checks. Its moving slow-pan image at frame 8 differed from its held image
at 6,186 pixels, including the curved edges of two arches. The corresponding
blocks in the preceding run had zero pixels over the threshold. In the separate
one-bounce glossy fixture, replacing the per-frame random GGX normalization with
the hemisphere integral reduced RMS to 1.11821. All 28 targeted GPU tests passed.
`receiver/phase2-3pct.json` and `receiver-qualification/glossy/report.json` contain
the two measurements.

At the eight-frame dolly pose, the normal map could reverse the orientation of
the rasterized triangle plane at 11 of 66,197 sampled receivers. None of those
11 pixels was among the 674 dolly errors. The comparison uses the plain geometric
normal from ZenApp's floating-point capture and the mapped shading normal from
its normal debug view. See `receiver-guides/guide-comparison.json`.

The seventh run passed all 15 full-frame diffuse checks in one launch pair.
It retained world-space anchors across compatible motion. A relocated probe
reconstructed its prior from raw measurements on its previous surface. The
local variation between those measurements reduced the prior's weight at
lighting edges. Slow-pan errors at frame 8 fell from 4,494 pixels in the
preceding run to 651. The dolly measured 642 pixels and neither spark check
reported any. In a 384×224 crop of the arch, 1,793 pixels in the preceding
run exceeded four display levels of error. The same crop had zero in this run.
`variance-qualification/arch-history.png` shows both moving and held crops at
native resolution. The full gate table is in `variance/phase2-3pct.json`.

An independent repeat on the same executable and shader set passed 14 of 15.
The eight-frame dolly had 862 affected pixels against a ceiling of 767. Of
those, 628 also failed in the first launch. In a lower-left 64×64 patch,
118 additional pixels crossed the threshold. Their moving-image mean rose
from 2.69 to 4.38 display levels while their held-image mean stayed at 1.21.
A second patch crossed mainly because its held image changed. The repeat is
in `variance-repeat/phase2-3pct.json`. On the separate HDR-lit 165-degree
snap, bright arrival pixels fell from 3,582 to 1,740. The three-frame turn
fell from 3,537 to 2,153. Both compare the arrival with its own 512-frame
hold. Glossy reference RMS was 1.14612 against 1.42706 retained. All 28
targeted GPU integration tests passed on this build.

At 78.2% of the 628 repeatable dolly errors, the surface normal changed by
more than 30 degrees within one pixel, even with normal maps disabled. The
rate was 31.8% at controls selected in the same 32×32 patch and within two
display levels of the same held brightness. In both runs, 455 of the 628
pixels were brighter in motion. `dolly-normal-neighborhoods.json` records
the neighborhood measurements at one, four and eight pixels.

A ray-budget experiment used measured probe variance, a constant-radiance
control variate, shared gather tiles and connected surface patches for adaptive
placement. It passed 10 of the 15 image checks. Static RMS rose to 0.27585
and the eight-frame dolly rose to 990 pixels. A 30-frame base-clock Nsight pan
measured 41.018 ms for diffuse GI, including 30.81 ms for ray tracing and
5.61 ms for adaptive placement. This experiment was removed. Its limits and
timings are in `budget/phase2-3pct.json` and `budget-qualification/pan/phase2.json`.

The next build fitted a local spatial gradient at the receiver and selected
each world-cache sample from competing claims in separate GPU passes. The
selection uses a stratified position on the cell surface. It publishes the
selected position and its geometric normal together. The first launch passed
all 15 checks. The repeat passed 14, with **771 pixels against 767 allowed**
on the eight-frame dolly. With each moving image compared against both held
images, the four counts were 741, 736, 775 and 771. The miss followed the
moving image. The full results are in `fit-seeds2-qualification/crossed-holds.json`.
On the HDR-lit snap the build had 2,712 bright arrival pixels, including
2,012 in the upper arcade. Its glossy reference RMS was 1.14281.
In the CUDA fixture, all 3,830 receivers matched. Diffuse mean bias was
+0.302% at one bounce and −0.181% at sixteen. The corresponding RMS values
were 0.002225 and 0.003701. Cache-only bias at sixteen was −0.430%, inside
the reference's own tolerance. All 28 targeted GPU tests passed, including
the pass-order checks for the three steps of cache seeding.

The next build used a continuous cubic B-spline to gather from compatible
surface anchors. It estimated a spatial lighting gradient only to the extent
that the gradient explained variation above the probes' measured uncertainty.
Two fresh ZenApp launch pairs each passed all 15 checks. Their eight-frame
dolly counts were 755 and 754. Comparing each moving capture with the other
launch's held pose gave 741 and **778**. The latter exceeds the 767-pixel
ceiling by 11. Static RMS was 0.16052 and 0.15593. Lamp recovery at frame 128
was 0.22380 and 0.22993. Neither launch reported a stationary or moving spark.
See `spline-qualification/crossed-holds.json`.

With the HDR environment and reflections enabled, the one-frame 165-degree
turn produced 1,769 bright arrival pixels compared with 20,248 in the original
reproduction. Eight and 32 frames later the counts were 993 and 10. The
three-frame turn produced 2,354 on arrival. Each count uses the same camera
path and the respective build's 512-frame held view. In the 472×264 upper
arcade crop the original 5,145 bright pixels fell to 121. The captures and
positive differences appear in `spline-qualification/sharp-turn-crops.png`.

The two probe CUDA comparisons passed at 3,830 receivers each. Mean biases
were +0.363% and −0.156% for one and sixteen bounces. The separate cache-only
sixteen-bounce comparison measured −0.821%, outside its ±0.659% reference
spread. The glossy reference measured 1.13604 RMS, all 15 reflection motion
and stability checks passed within their 1% tolerance, and all 28 targeted
GPU integration tests passed.

On the same build, 30-frame base-clock Nsight captures measured **24.478 ms
static and 59.039 ms panning** for diffuse GI on the RTX 5080. During the
pan, placement, proposals and tracing took 51.708 ms, filtering 1.814 ms
and the full-resolution gather 5.483 ms. The 3% total ceiling is 5.459 ms.
`spline-qualification/static/phase2.json` and
`spline-qualification/pan/phase2.json` retain the exact launch configuration.

The HDR-lit center view was also recaptured at frame 3,072 on that build.
Across 518,227 matched 4×4 boxes, diffuse indirect irradiance was 98.69%
of the path-traced reference with Lambert secondary surfaces, and its bounced
component was 98.32%. Against the full-material reference, those values were
82.96% and 76.96%. Changing only the secondary material in the reference
accounts for 94.3% of the remaining mean bounced-light deficit. Sky irradiance
was 99.47% of the reference. This comparison uses normal maps off to match
the reference. The sharp-turn capture uses normal maps on.
`spline-qualification/center/comparison/secondary-material.json` contains
both reference comparisons.

At the eight-frame dolly pose, a separate held-view readback found cache
requests arriving faster than the 262,144-entry queue could hold them.
At frame 1,024 it dropped 33,059 of 295,203 requests. The next build combined
repeated observations of the same part of a surface before queue insertion.
It stored 238,691 requests and dropped zero at the same point, using the
same 32 bytes per request and the same 8,388,608 cache cells. Both readbacks
rendered 3840×2160 with a 1280×720 framebuffer and normal maps off. The
fullscreen image test uses its original framebuffer, normal maps and capacity.
See `spline-qualification/dolly-demand/report.json` and
`demand-qualification/dolly-demand/report.json`.

With duplicate requests removed, the fullscreen run passed 14 of 15 checks.
All nine camera-motion checks passed. The eight-frame dolly had 682 changed
pixels against 767 allowed. Static RMS was 0.15804 and both spark counts were
zero. At frame 128 after the lamp moved, RMS was 0.236840 against a ceiling
of 0.236532, a 3.134% gap to the retained result. The largest excess was on
the brick wall beside an arch. In a 224×272 crop there, 995 pixels were over
four display levels brighter than the hold. The one- and sixteen-bounce CUDA
runs both had zero dropped requests at frame 4,096, down from 19,838 and
24,241. They matched 3,830 receivers with mean biases of +0.368% and −0.088%.
See `demand/phase2-3pct.json` and `demand-qualification/lamp-region.png`.

Accounting for the history accepted at a relocated probe lowered static RMS to
0.14460 and lamp RMS to 0.16804. The eight-frame dolly rose to 871 pixels, so
this version also passed 14 of 15. It treated the cache's lighting-response
weight as a count of independent ray batches. The 1,024-frame section that
moved the lamp took 133 seconds on this build and 43 seconds on the preceding
build, measured between capture writes. These are elapsed run times. The
GPU stage timings above come from Nsight. `acquisition/phase2-3pct.json`
contains the image results.

The next build, `guided`, fits three directional lobes to each probe's own
observations, one per 120-degree sector around its normal. Up to four of every
16 rays sample those lobes, taken from the local-light and learned-direction
lanes, and every ray still uses the full mixture density. The fit is frozen on
the same independently scrambled block as the direction table, so a ray never
guides its own sample. Its concentration is scaled by the number of independent
observations, so one bright sample cannot localize a source. Ray allocation
counts accepted bounce history in units of the cache's current lighting
response, because extra rays cannot make the cache respond sooner.

| Guided build, full 4K | Static RMS | Lamp RMS, +128 | Slow pan, +8 | Fast pan, +8 | Dolly, +8 (pixels) | Checks within tolerance |
|---|---:|---:|---:|---:|---:|---:|
| Allowed ceiling | 0.18358 | 0.23653 | 0.00934% | 0.01244% | 0.00925% (767) | 15 |
| First launch | 0.15739 | 0.20194 | 0.00732% | 0.00693% | 0.00684% (567) | 15 |
| Repeat | 0.15389 | 0.20539 | 0.00725% | 0.00652% | 0.00702% (582) | 15 |

**This is the first build to pass all 15 checks in both launches and in the
crossed comparison.** Comparing each moving capture with either launch's held
images gave 15 of 15 in all four pairings. Their eight-frame dolly counts were
567, 572, 588 and 582. Neither launch reported a stationary or moving spark.
The first repeat attempt stopped at frame 841 of 5,081 and is retained unscored
as `guided-repeat-stopped`.

The two probe CUDA comparisons matched 3,830 receivers each. Mean biases were
+0.385% and −0.140% at one and sixteen bounces. RMS was 0.0022299 against
0.0022261 (0.17% over, within tolerance) and 0.0037058 against 0.0074176.
The cache-only sixteen-bounce comparison measured −0.695%, outside the ±0.659%
reference spread. A repeat on the same executable and shaders measured −0.636%,
inside it. Single launches since `fit-seeds2` ranged from −0.146% to −0.903%,
so the cache stays slightly dark at sixteen bounces and the Phase 1 energy
qualification is still not carried forward.

The glossy fixture measured 1.13398 RMS against 1.42706 retained, and all 15
reflection motion checks passed within 1%. All 28 targeted GPU integration
tests passed. The HDR sharp-turn snap and Nsight timing were not run on this
build, so its cost is unmeasured. The spline build's 24.478 ms static and
59.039 ms panning diffuse GI remain the latest timing, against the 5.3 ms target.

The executable SHA-256 is
`dd216472c30ca7bdec2117335bb0db88fe4bcee15bb0c50b2671b699ccf19f24` and the
compiled-shader SHA-256 is
`a9e9468b4cfed9fd212befd863b44c8ec35f3af7446c7b6e8995ad40aaa56c1e`.
`guided-qualification/qualification.json` records every report hash, and
`crossed-holds.json` the four pairings. The cache repeat is in
`guided-qualification/cache-16-repeat/report.json`.

### Screen-probe frame cost breakdown (2026-11-20)

The `guided` build was profiled in the interactive configuration: New Sponza at 3840×2160
fullscreen with HW tracing, reflections, the Kloofendal HDR sky, normal maps, exact direct
shadows and async compute off. Each 30-frame Nsight capture used base clocks after 60 seconds.
The pan is the slow timing path in `current-gates-20261117/timing-pan`, about 0.024 degrees
per frame. Three one-line shader variants isolated causes; the sources and compiled shaders were
restored to the `guided` hashes afterwards.

| Median ms | Static | Pan | Mature allocation, pan | No gather specular, static |
|---|---:|---:|---:|---:|
| Frame | 39.19 | 67.94 | 41.20 | 33.38 |
| ScreenProbeTrace | 7.29 | 34.24 | 7.27 | 7.19 |
| ScreenProbeGather | 9.20 | 10.46 | 10.68 | 3.55 |
| WorldCacheUpdate + resolve | 6.61 | 6.22 | 6.87 | 6.73 |
| ReceiverVisibility | 4.91 | 4.82 | 4.89 | 4.91 |
| Probe prefilter + specular spatial | 2.50 | 2.68 | 2.62 | 2.46 |

- **Motion cost is the age-based ray allocation.** A probe whose anchor drifts 0.75 tile from
  its tile center relocates with its age clamped to 64 frames, and the proposal shortens it
  further by the discounted history. `ProbeDiffuseBatches` then gives it 9–33 batches of 16
  paths instead of two. Even this slow pan keeps most probes young. Forcing the mature count
  brought the panning trace from 34.24 to 7.27 ms, the static value.
- **Path continuation is not a cost driver.** Ending probe paths at their first hit changed the
  trace by 0.1 ms static and 0.8 ms panning.
- **The gather spends 5.6 ms on rough specular.** Each pixel blends about 16–20 probes over the
  cubic B-spline footprint. Each tap reads eight GGX atlas texels. The diffuse part alone costs
  3.55 ms against the 1.5 ms target.
- **Settled tracing costs 7.2 ms** for about 5.2 M paths (162,000 slots × 32), about 1.4 ns per
  path. That is 2.7 times the Phase 0 cost per ray.
- The world-cache update costs four times its Phase 1 measurement. Each update loops over every
  light, and cells with more than one sample per frame take exact shadow rays for each
  contributing light. This was not isolated.

Evidence: `build/checks/native-gi/fps-20261120/` (`guided-*`, `mature-*`, `onehit-*`,
`nospecgather-static`, `variants.py`, `table.py`). The optimization phases that follow from it,
and the fast-motion quality guard, are in [NativeGIOptimization.md](NativeGIOptimization.md).

### Ground-truth comparison, center view (2026-11-19)

The Sponza image gates compare native with its own settled images, so they measure stability,
not correctness. One view was therefore compared with a path-traced ground truth: the gate's
base pose in New Sponza, with the 23 lamps, the sun and the
`kloofendal_48d_partly_cloudy_puresky_1k.hdr` sky. Diffuse and specular indirect light include
all bounces. Normal maps are off on both sides, because the reference shades with vertex
normals.

The engine side is a ZenApp ground-truth capture of the selected build at frame 3,072. It
renders 3840×2160 with screen probes, 4,194,304 cache cells, reflections on and exact direct
shadows, and exports linear lighting terms per pixel. The reference, `tools/ground_truth_path.py`,
is a new CUDA path tracer that uses Mitsuba only for intersection and environment sampling.
At every vertex it uses the engine's material model: unclamped GGX, the engine's Smith–Schlick
shadowing with the image-lighting constant k = α/2, Schlick Fresnel, and a Lambert lobe
weighted by (1 − F)(1 − metallic). It samples lamps and the sun with shadow rays, applies
multiple importance sampling to the sky, and continues paths up to ten vertices with Russian
roulette. Both sides average 4×4 pixel boxes, with 4,096 reference paths per box.

| Term | Truth | Engine | Engine ÷ truth | Relative RMS (reference noise) |
|---|---:|---:|---:|---:|
| Direct irradiance, check | 0.03323 | 0.03322 | 1.000 | 0.010 |
| Direct radiance, check | 0.00654 | 0.00657 | 1.006 | 0.010 |
| Diffuse indirect irradiance | 0.01707 | 0.01156 | **0.677** | 0.42 (0.13) |
| — sky seen from the surface | 0.00455 | 0.00454 | 0.997 | |
| — light bounced off surfaces | 0.01252 | 0.00702 | **0.560** | |
| Diffuse indirect radiance | 0.00305 | 0.00191 | 0.628 | 0.30 (0.06) |
| Specular indirect radiance | 0.00058 | 0.00079 | **1.362** | 1.18 (0.10) |
| Final image | 0.01016 | 0.00927 | 0.913 | 0.033 |

Values are mean luminance over 518,227 of 518,400 boxes; receiver masks agree exactly.

**The deficit is bounced light.** Sky irradiance is within 0.3%, sun disc included. The engine
has 56% of the true bounced light. About a fifth of the truth's bounced light comes from
specular reflection at bounce points and from metals, which the world cache shades as Lambert
albedo × 0.96 × (1 − metallic). With that secondary material in the reference, true bounced
light is 0.00980, and the engine still reaches only 72% of it. The loss is largest on directly
sunlit and distant surfaces (0.45–0.6), smaller on the near arcade walls (0.8–0.97), and
slightly reversed on walls next to the lamps. Dropping cache samples above the encoder's 255
limit is not the cause: 80 occur per frame, and clamping them instead changed bounced light by
1%. HDR texels above that limit supply only 0.8% of the sky irradiance on this view.

**Specular indirect light is 36% too bright** on average, with large per-pixel error. It was
not decomposed further.

**Validation.** Direct irradiance and radiance match the engine to 1% RMS, so the camera,
lights, shadows, units and materials agree. The tracer's GGX sampling reproduces a brute-force
lobe integral. In a one-bounce Lambert mode it agrees with the earlier fixture-validated
reference within 1.1%, which is inside that tool's own 16% half-to-half spread. The two halves
of the final reference give the per-box noise in the table.

**Defects found along the way.**
- `tools/ground_truth_reference.py` truncates the interpolated primitive index, so some hits
  take the previous primitive's material. This affects the `newsponza/v3` `D_bounce` truth.
- The engine's direct-light `DistributionGGX` divides by `max(pi * d * d, 1e-6)`, which caps
  highlights below roughness ~0.155.

Evidence is in `build/checks/native-gi/gt-center-20261119/` (`summary.json`,
`compare/comparison.png`, `surface-ratio-lambert.png`, and the reference `.npz` files with their
`.json` metadata).

### Fast-turn recovery with environment lighting (2026-11-19)

Turning the view quickly shows sparkles and darker lighting that settle over hundreds of frames.
Turns below 60 degrees per frame keep screen history (the camera-cut test), so this is
reconstruction, not a reset. Each measurement warms up 1,024 frames at the gate's base view,
turns, holds, and compares each frame with the frame 1,024 after arrival. The setup is New
Sponza with the sky HDR, reflections on and normal maps on, at 4K.

| 180 degrees in 6 frames | +1 | +8 | +32 | +128 | +512 |
|---|---:|---:|---:|---:|---:|
| Final image, % above 4 levels | 4.16 | 3.41 | 2.76 | 1.84 | 1.32 |
| Final image, % more than 16 levels darker / brighter | 1.94 / 0.12 | 1.79 / 0.11 | 0.96 / 0.14 | 0.27 / 0.16 | 0.10 / 0.03 |
| Final image, mean bias (levels) | −1.07 | −0.83 | −0.39 | −0.13 | −0.02 |
| Probe sky view, % above 4 levels | 13.8 | 11.2 | 6.4 | 2.5 | 1.6 |
| Reflection view, % above 4 levels | 17.8 | 9.1 | 5.8 | 2.9 | 0.5 |
| Bounce view, % above 4 levels | 3.4 | 2.4 | 1.3 | 0.26 | 0.09 |

The debug views (output 8, faces 0, 1 and 2) come from separate launches. A 60-degree turn
measured 0.64% above four levels at +1. The world cache played no role: with its cells cold,
retained 200 frames, or evicted, recovery matched within launch noise. Probe-only reflections
(glossy threshold 0) recovered like the full reflection path.

**The slow, sparkly part is a sampling gap, not the architecture.** Probe rays sample a uniform
hemisphere, lobes around local lights and a learned 8×8 octahedral distribution. They never
importance-sample the environment. A fresh probe therefore finds the HDR's sun disc and bright
sky regions only by chance. It is too dark until it does, and it sparkles when a rare hit arrives.
Settling lasts until history and the learned distribution catch up. The settled sky term is
correct (0.997 against the ground truth), so the estimator is unbiased but slow. The engine
already has an environment importance sampler with separated sun sources
(`environment_sampling.glsl`), which the retained renderer uses. Probe rays and the probe GGX
reflection atlas could use it with multiple importance sampling. The remaining bounce-light
settling, 64–128 frames for surfaces that just became visible, is the inherent cost of
temporal accumulation. Evidence: `turn180-*` and `turn-*` with their reports under
`build/checks/native-gi/gt-center-20261119/`.

**Fix (2026-11-19).** Four of the 16 probe lanes now sample the environment's importance
distribution. The shared environment sampler is bound for the Compute probe trace as well.
The uniform lanes' stratification grid now tiles six lanes; it previously left two of six
outside it. Seeing the sun every frame exposed a second issue. The spatial filter sized its
footprint, and enabled lighting-edge stopping, by frame age, with edge stopping off before 32
frames. Young probes therefore spread sunlight across sun-shadow edges. Both now use
accumulated ray batches, measured in frames of a mature probe's sampling rate.

| 180-degree turn, final image | +1 | +4 | +16 | +32 | +128 | +512 |
|---|---:|---:|---:|---:|---:|---:|
| % above 4 levels, before → after | 4.16 → 2.38 | 3.61 → 1.42 | 3.07 → 0.51 | 2.76 → 0.33 | 1.84 → 0.13 | 1.32 → 0.02 |
| % more than 16 levels darker, before → after | 1.94 → 0.03 | 1.85 → 0.008 | 1.60 → 0.003 | 0.96 → 0.003 | 0.27 → 0.002 | 0.10 → 0.001 |
| % more than 16 levels brighter, before → after | 0.12 → 0.16 | 0.14 → 0.13 | 0.09 → 0.018 | 0.14 → 0.003 | 0.16 → 0.0001 | 0.03 → 0.0001 |
| Mean bias in levels, before → after | −1.07 → −0.10 | −0.91 → −0.09 | −0.66 → −0.09 | −0.39 → −0.10 | −0.13 → −0.08 | −0.02 → −0.01 |

With environment lanes alone, early sparkles tripled. They formed a dense band of
sunlight spread along a sunlit wall, which the sample-based filter removes. Isolated sparkles
from young probes catching lamp-lit samples, about 3,000 pixels at +1, are unchanged from
before. The −0.09-level bias over the first 128 frames comes from the reflection atlas.

Checks on this build: the ground-truth recapture measured sky irradiance 0.999 of truth, so the
estimator stays unbiased. Specular indirect radiance moved from 1.36 to 1.18 of truth. Bounced
light was unchanged at 0.56, and the final image is 0.90 of truth. One-bounce CUDA RMS was
0.0022221, below 0.0022261. Sixteen-vertex RMS was 0.0036943 with −0.19% bias. All 28
targeted GPU tests passed. Validation-enabled hardware and Compute launches with the sky on
reported no errors or fallback. Within the 20-minute validation budget, the Sponza diffuse
quality gate and the glossy checks were not rerun. The gate setup has no environment
lighting, but the filter change also affects its young probes. Evidence: `turn180-final`,
`turn180-envlanes-final`, `engine-envlanes`, `compare-envlanes`, `envlanes-cuda-*`,
`envlanes-tests.log` and `validate-envlanes-*`.

### New Sponza, 165-degree turn with environment lighting (2026-11-19)

ZenApp turned 165 degrees in three frames, then repeated the turn in one frame after
returning to the starting view. Both turns used native hardware GI, 4K fullscreen,
the Kloofendal HDR sky, normal maps and the scene lights. Each arrival was compared
with its own image after 512 frames at that pose. The three-frame turn stayed below
the camera-cut threshold on each step. Both turns exposed similar artifacts.

| Pixels more than 16 display levels too bright at arrival | Before | After |
|---|---:|---:|
| Three-frame turn, whole image | 18,879 | 3,537 |
| One-frame turn, whole image | 20,248 | 3,582 |
| One-frame turn, upper arcade region | 16,492 | 2,156 |
| One-frame turn, orange floor spot | 586 | 0 |
| One-frame turn, window grille region | 106 | 44 |

The arcade band came from the wide sky filter crossing a sun-shadow boundary at
one-frame-old probes. The sky term now gives distant taps less weight. The floor
spot was a reflected lamp-lit hit in the rough probe atlas. Its spatial filter now
compares three groups of taps while history is young, using the same outlier test as
diffuse reconstruction. The temporal atlas still accumulates the observed radiance.
The window grille was a mirror. Its temporal samples were dark at the flash, but the
spatial pass spread a neighboring lamp reflection onto them. Mirror receivers now
use the luminance edge test from the first frame. More glossy rays left this flash
unchanged.

All 106 original grille outliers in the snap fell below the threshold. The 44
remaining outliers were at different pixels. The largest three-frame outlier,
pixel (3726, 914), remains 160 levels. On arrival the gather had no supporting
probe and supplied full environment visibility. The held view had coverage and
zero visibility. The pixel went from RGB (142, 146, 160) to black with only
environment specular visibility enabled. Disabling environment lighting kept
it black throughout. Trying visibility from a coplanar neighboring object did
not remove the flash. The snap's mean signed error went from −0.07 to −0.21
display levels on arrival while RMS fell from 2.12 to 1.39. At frame 32 the bright
pixel count went from 199 to 156.

The selected launch is `sharp-candidate` under
`build/checks/native-gi/sponza-turn-120/`. `selected-comparison.png`,
`selected-overlap.json` and `instrumentation-glossy/` retain the crops, pixel
comparisons and mirror-filter diagnostic.

**Phase checks on the selected shaders.** The Phase 2 diffuse setup disables the
environment and reflections. It passed 10 of 15 image checks. The misses were
static cross-launch RMS, all three eight-frame motion poses and slow pan at frame
32. Both spark checks passed. Across the three eight-frame poses, 2,849 of 3,766
failing pixels also failed on the earlier 128-update cache build. A matched
probe-age replay put 1,250 failures at ages 1–7 and 2,194 at ages 64 or older.
Newly exposed geometry has an elevated error rate, but the median failing pixel
already has established diffuse history. One- and sixteen-vertex probe RMS were
0.002222 and 0.003700, within their respective limits. All 28 targeted GPU tests
passed. In the separate 3,072-frame center-view comparison, mean sky irradiance
was 99.9% of the path-traced reference. Bounced diffuse was 56.0% and specular
indirect radiance was 117.6%. The settled lighting discrepancies remain.

The Phase 3 fixture passed all 15 motion checks. Static specular RMS was 1.3034
against the retained limit of 1.4271. Two 30-frame Nsight captures at base clocks
measured the glossy trace and denoise stages at 1.410 ms median when static and
1.543 ms during the pan. The whole-frame medians were 25.18 and 65.77 ms.
Screen-probe tracing alone took 45.41 ms during the pan. These captures measure
the selected build against the plan's budgets. A same-build timing comparison
with the filters reverted was not run. Reports and captures are in
`sponza-turn-120/qualification/`.

### Phase 2 world-cache window (2026-11-19)

**Phase 2 remains open.** A stable world-cache cell now averages 128 updates instead of 64.
This removes the overbright light-edit defect diagnosed in the next section, without clamping
energy. Light recovery passes in both launches. Total motion error falls a further 8–20%,
dolly 32 and 64 now pass in both launches, and the build passes **11 and 10 of 15** Sponza
image checks. Static cross-launch and the three 8-frame motion poses still fail. Slow pan at
frame 32 passes in one launch and misses by 2.1% in the other.

| Selected 4K diffuse result, launches A / B | Measured | Retained limit |
|---|---:|---:|
| Static cross-launch RMS | 0.20685 / 0.20246 | 0.17823 |
| Moved-light RMS at frame 128 | **0.22600 / 0.22043** | 0.22964 |
| Slow pan, % above four levels at frames 8 / 32 / 64 | 0.01515 / **0.01072** / **0.00945**; 0.01536 / 0.01161 / **0.01162** | 0.00907 / 0.01137 / 0.01501 |
| Fast pan, same metric | 0.01446 / **0.01936** / **0.01636**; 0.01624 / **0.02016** / **0.01757** | 0.01208 / 0.03674 / 0.02844 |
| Dolly, same metric | 0.01271 / **0.01229** / **0.01400**; 0.01175 / **0.01153** / **0.01130** | 0.00898 / 0.01542 / 0.01922 |
| Stationary flicker, mean / p99.9; static / moving sparks | pass in both | |

The nine-pose motion sums are 0.1245% and 0.1271%, against 0.1383% and 0.1590% for the
preceding candidate. In the ten-move repro launch, the share of pixels above four levels
128 frames after a light move fell from 0.0128% to 0.0043% on average, and from 0.0408%
to 0.0061% at worst. RMS fell from 0.2328 to 0.2214. After the light returned, the average fell
from 0.0164% to 0.0026%. Hot cells in the static cache view fell from 17.8 to 11.6 per frame,
and the largest block excess from 69 to 35 levels. The mean level was unchanged at 30.22.
A 255-update window measured 11.2 hot cells, so 128 was kept.

The light-edit rules scale with the window. During propagation a distant cell's history
limit is interpolated toward the configured window, and a distant prior keeps at most that
many updates. Young cells still use 16 updates until they have 256 samples.

All energy checks pass. One-bounce probe RMS is 0.0022220 against 0.0022261, with +0.41%
bias. Sixteen-vertex probe RMS is 0.0036965 against 0.0074176, with −0.16% bias. Two
cache-only sixteen-vertex launches measured +0.09% and +0.44% bias against ±0.66%, with
spatial RMS 0.0120 and 0.0128. All 28 targeted GPU tests pass. The glossy fixture passes the
static specular check, 1.28776 against 1.42706, and all 15 reflection motion checks.

Evidence is in `qualification-w128.json` under `build/checks/native-gi/p2-close-20261119/`,
with `w128-quality-a`, `-b`, `w128-cuda-*`, `w128-cache-16-*`, `w128-glossy-*`,
`w128-tests.log`, `repro/r4-w128` and `hot/w128`. The change is `kHistoryUpdates` in
`WorldRadianceCache.cpp` and the two window-relative rules in `gi_cache_resolve.glsl`.

### Phase 2 motion outliers (2026-11-19, later)

**Phase 2 remains open.** The new selected build removes history borrowing and widens the
gather to a continuous 3×3 tile window. It passes both CUDA references and **9 of 15**
Sponza image checks in each of two launches, up from 7. Total motion error over the nine poses
fell by 21–31%. By RMS, every motion pose is now below the retained renderer. The
remaining motion failures are a few thousand pixels of large, repeatable differences at
creases, thin moldings, column capitals and lamp fixtures. Static cross-launch error and an
intermittent overbright defect near point light 3 also remain. Timing is still deferred.

| Selected 4K diffuse result, launches A / B | Measured | Retained limit |
|---|---:|---:|
| Static cross-launch RMS | 0.19970 / 0.20538 | 0.17823 |
| Moved-light RMS at frame 128 | 0.24403 (defect) / **0.22107** | 0.22964 |
| Slow pan, % above four levels at frames 8 / 32 / 64 | 0.01959 / 0.01220 / **0.01042**; 0.01704 / 0.01165 / **0.01114** | 0.00907 / 0.01137 / 0.01501 |
| Fast pan, same metric | 0.01878 / **0.01974** / **0.01700**; 0.01796 / **0.02285** / **0.02180** | 0.01208 / 0.03674 / 0.02844 |
| Dolly, same metric | 0.01291 / **0.01418** / **0.01348**; 0.01262 / 0.02834 (defect) / **0.01559** | 0.00898 / 0.01542 / 0.01922 |
| Stationary flicker, mean / p99.9; static / moving sparks | pass in both | |

The one-bounce CUDA comparison measured RMS 0.0022224 against 0.0022261, with +0.40% mean
bias. The sixteen-vertex comparison measured RMS 0.0036972 against 0.0074176, with −0.21%
bias. Both matched 3,830 receivers with 8,388,608 cells. Each depth ran once on this build.
All 28 targeted GPU tests passed. The glossy fixture passed all 15 reflection checks, and its
static specular RMS was 1.28985 against 1.42706. A validation-enabled 4K launch with reflections
kept the native gather, 162,000 probe slots and 4,194,304 cache cells, without errors.
None of the remaining failures is within the 1% acceptance tolerance; slow pan at frame 32
is closest, at 2.5–7% over.

**Native launches are not reproducible bit for bit.** Two launches with identical inputs differ
by up to 20 display levels, probably from atomic accumulation in the world cache. Repeated
launches of one build moved single 8-frame poses by 15–33%. The sum over all nine poses moved
by 6%. `gi_quick_gates.py` still describes fixed-step runs as bit-identical; that no longer
holds for native GI. Compare candidates by their nine-pose sum, and repeat any launch that
decides a gate.

**The remaining motion error is repeatable.** Between the two launches of the borrowing-free
build, 75–92% of failing pixels failed in both. The moving frames themselves differed by more
than four levels at only 28–166 pixels. In both final launches, native motion RMS was
0.222–0.290 against 0.224–0.312 for the retained renderer, lower at every pose. The gate
counts only differences above four levels, and those cluster at the features listed above.

**Borrowing was harmful once young probes gained rays.** The previous build gave a probe without
a coplanar predecessor the history of a neighbor within 45 degrees, for 32 frames. Error rates
by gathered probe age, per 10,000 pixels across the nine poses:

| Gathered age | Previous build | Borrowing removed |
|---|---:|---:|
| 1–7 frames (new probes, 1,040 paths a frame) | 4.1 | 7.2 |
| Exactly 32 (borrowed this frame) | 38.2 | 1.3 |
| 33–63 (mostly borrowed history) | 12.5 | 1.1 |
| 251 and older | 0.39 | 0.35 |

Removing borrowing cut total motion error by 28–32% in two launches. Detailed geometry now
starts fresh and uses the young-probe ray allocation.

**The 2×2 gather window made steps.** Each pixel used the four probes of a 2×2 tile window
chosen from its pixel coordinate. The tent reached 1.5 spacings, beyond the window, so a probe
with nonzero weight dropped out wherever the window changed. That made a 25% step along every
line of probe centers. During motion, drifted anchors moved those lines. On the vault ribs it
appeared as an eight-pixel staircase. A 3×3 window around the pixel's own tile removes the
steps. A 1.5-spacing tent raised one-bounce RMS to 0.0022263, just above the limit. A
1.25-spacing tent keeps the window continuous, and the CUDA results above use it. The image
metrics of the two tents agree within launch-to-launch variation. Moving and settled images still
differ smoothly where a shadow band is two or three tiles wide. There, drifted probes sample the
band at a different phase.

| Rejected trial, on the borrowing-free build | Nine-pose motion sum (% above 4) | Finding |
|---|---:|---|
| Borrowing-free, two launches | 0.1365 / 0.1448 | Selected |
| Relocated probes start fresh | 0.2079 | Inherited history is better than none |
| Keep anchors within 0.5 tile | 0.3738 | More relocations; dolly 64 reached 0.101% |
| Keep anchors within 1.0 tile | 0.1636 | Larger drift |
| Each old anchor owned by one tile | 0.2346 | More relocations |
| Weight base probes at tile centers | 0.1539 | Ignores where drifted probes sample |
| Filter footprint by accumulated batches, not age | 0.1338 | Within launch variation |
| Relocated probe starts from the interpolated previous field | 0.1385 | Within launch variation |
| Quincunx adaptive probes, 1.0-spacing tents | 0.1548 | Twice the probes; worse |
| Stable history of 2,304 instead of 1,152 frames | 0.1634 | Static RMS rose to 0.20306; dolly worse |

**Static cross-launch is not a noise floor.** About 96% of its squared error comes from
one-level differences in dark, indirectly lit regions. Doubling stable history halved
stationary flicker, from 0.0023 to 0.0013 levels, yet static RMS rose. The long static warm-up
and the reference launch, which passes through nine poses and a light move, therefore settle
to slightly different states. The next step is the cold-start control from the
path-dependence work: start cold at a motion pose, then hold the base pose.

**An intermittent overbright defect affects the lighting and motion gates.** In some launches a
wall or column face near point light 3 brightens and stays bright. In one launch, after the light
moved, a block went from 5.5 to 12.4 levels over 128 frames; the held image is 5.75. In
another, the reference launch's held dolly 32 image was 1.1 levels brighter over a 100×120
region. Two of fourteen launches without borrowing had such a region at frame 128, with
0.0685% and 0.0256% of pixels above four levels; the other twelve measured 0.0024–0.0152%.
None of four earlier selected-build launches exceeded 0.0093%; that sample is too small to
tie the defect to borrowing. Four replays of the first launch's timeline did not reproduce it,
so its trigger is stochastic.

**Diagnosis: rare bright world-cache samples, exposed by light edits.** A repro launch moves
light 3 ten times, with a camera cut each cycle, and compares each capture with the per-pixel
median of its kind. It reproduces the defect in most launches. In the gathered probe-bounce
view the patches are soft, lamp-colored glows tens of pixels across. They form a few frames
after a light edit, grow for 128 frames, and fade over the next two or three cycles, about
1,500 frames. They appear far from the moved light. In the raw world-cache view, single
cells sit tens of display levels above their temporal median: in a static 512-frame window,
about 18 blocks of 32×32 pixels per frame exceed their median by more than eight levels,
by up to 69. Toggling sources in that view isolates the cause:

| Static cache-view variant | Hot blocks per frame | Mean level |
|---|---:|---:|
| Selected | 17.8 | 30.27 |
| No multi-bounce feedback | 1.0 | 20.83 |
| No emissive lighting | 12.7 | 30.14 |
| No light-guided feedback rays | 33.8 | 29.44 |
| Roulette survivors weighted at most 2× | 22.1 | 30.21 |
| Guide lobe anisotropy 0.85 / 0.75 instead of 0.94 | 18.7 / 26.7 | 30.04 / 29.80 |
| Guide toward every contributing lamp | 16.0 | 29.84 |
| Light distance floor of 0.003 / 0.01 scene units (diagnostic only) | 12.1 / 3.1 | 29.79 / 28.40 |
| Each feedback sample clamped to 16× its cell (diagnostic only) | 1.2 | 26.62 |

The hot samples are feedback paths that land on surfaces lit strongly by nearby lamps. They
carry real energy: clamping them removed 12% of the mean level. Each one persists through
its cell's 64-update window. Mature probes average 1,152 frames and dilute these transients.
But every finite-light edit capped all probe histories at 256 frames for 64 frames (16 near
the light). Probes then followed hot cells three to four times faster, and the transients
became visible.

Letting far probes keep their full history on light edits removed the large patches. In the
repro, the share of pixels above four levels 128 frames after a move fell from 0.0128% to
0.0056% on average, and from 0.0408% to 0.0140% at worst. It also kept old lighting in those
probes. Two quality launches then measured light/128 RMS 0.234 and 0.225, static RMS 0.219
and 0.193, and no lower motion error. The change was reverted. The fix belongs in the cache:
the 128-update window in the section above. Sharing rare bright feedback samples with
neighboring cells, or spending more updates where lamp-lit surfaces dominate a cell's bounce
light, could reduce the remaining hot cells. The longer window did not lower static
cross-launch error, so these transients are not its main cause. `repro.py`,
`repro_score.py`, `repro_recovery.py` and `hotcells.py` in the evidence directory are the
harnesses. The `repro/`, `hot/` and `fixA-quality-*` folders hold the runs.

Evidence is under `build/checks/native-gi/p2-close-20261119/`. `qualification.json` records
raw and 1%-tolerance results, executable and shader hashes, and every trial report hash.
`final.patch` holds the source change. The final launches are `f2-reach125` and
`final-quality-b`, with CUDA runs `f2-reach125-cuda-1` and `-16`. `final-glossy-static`,
`final-glossy-motion` and `final-combined` hold the reflection and launch checks. The age
analyses use `age_hist.py` and `*-age` captures. The determinism checks are `aa_overlap.py`
and `light_rep.py`.

### Phase 2 diffuse density and qualification (2026-11-19, preceding checkpoint)

**Phase 2 remains open.** The selected build uses an eight-pixel base grid and one adaptive
slot per tile for diffuse-only rendering. It passes both CUDA transport comparisons and
**7 of 15** Sponza image checks. The passing checks cover both flicker metrics, both spark
metrics, frame-128 light recovery and fast pan at frames 32 and 64. Static cross-launch
consistency and seven camera-motion checks remain above the retained limits. Timing
qualification remains deferred.

| Selected 4K diffuse result | Measured | Retained limit |
|---|---:|---:|
| Static cross-launch RMS | 0.19432 | 0.17823 |
| Moved-light RMS at frame 128 | **0.22360** | 0.22964 |
| Slow pan at frames 8 / 32 / 64 (% above four display levels) | 0.01817 / 0.01396 / 0.01887 | 0.00907 / 0.01137 / 0.01501 |
| Fast pan at frames 8 / 32 / 64 (%) | 0.02221 / **0.02802** / **0.02691** | 0.01208 / 0.03674 / 0.02844 |
| Dolly at frames 8 / 32 / 64 (%) | 0.01897 / 0.02655 / 0.02785 | 0.00898 / 0.01542 / 0.01922 |

The selected diffuse-only allocation traces 1,040 paths per valid probe aged 1–8 frames,
528 at 9–32, 272 at 33–64, 144 at 65–128 and 80 at mature probes. A recent lighting or
geometry change can also request the younger allocation. The 16 lanes accumulate the extra
diffuse samples locally, so this change fits the same ray buffer. The combined path keeps
the preceding ray counts and the eight-pixel base grid with one adaptive slot per four tiles.
Two independent 4,096-frame launches at each depth matched 3,830 CUDA receivers. One-bounce
RMS was 0.00222180 and 0.00222285 against 0.00222612. Sixteen-vertex RMS was 0.00369532 and
0.00369583 against 0.00741765. Mean energy passed in all four launches. The reports are
`p2-resume/quality-samples/report.json`,
`p2-resume/samples-cuda-1/report.json` and `p2-resume/samples-cuda-16/report.json`
under `build/checks/native-gi/`, with the repeats in `samples-repeat-1/` and `samples-repeat-16/`
in the same evidence directory.

On the selected binary and shaders, all 28 targeted GPU integration tests passed, the glossy
fixture passed all 15 stability checks, and static specular RMS was 1.30833 against 1.42706.
The combined 4K Sponza capture retained the native gather, 162,000 probe slots and 4,194,304
cache cells. `p2-resume/qualification.json` records the checked report hashes and the identity
of the canonical ZenApp executable and shader set.

#### Uniform four-pixel grid

The four-pixel Sponza run passed **5 of 15** diffuse image checks, up from 4 of 15 at eight
pixels. Both densities passed mean and p99.9 stationary flicker and both spark checks.
Four-pixel spacing also passed fast pan at frame 32. Each renderer used its own held image
at each camera pose. The frozen retained-renderer limits, 4,096-frame warm-up, 1,024-frame
holds, fullscreen 3840×2160 output, hardware tracing and disabled async compute match the
preceding eight-pixel run.

| Diffuse comparison | Eight pixels | Four pixels | Retained limit |
|---|---:|---:|---:|
| Static cross-launch RMS (display levels) | 0.22153 | 0.21337 | 0.17823 |
| Moved-light RMS after 128 frames (display levels) | 0.24342 | 0.23422 | 0.22964 |
| Slow pan, frames 8 / 32 / 64 (% above four levels) | 0.03049 / 0.03496 / 0.04130 | 0.01987 / 0.01728 / 0.01553 | 0.00907 / 0.01137 / 0.01501 |
| Fast pan, frames 8 / 32 / 64 (%) | 0.04004 / 0.04815 / 0.03057 | 0.01473 / 0.03330 / 0.03218 | 0.01208 / 0.03674 / 0.02844 |
| Dolly, frames 8 / 32 / 64 (%) | 0.04227 / 0.03845 / 0.04925 | 0.01974 / 0.02470 / 0.02979 | 0.00898 / 0.01542 / 0.01922 |

With reflections disabled, Sponza rendered 648,000 probes and 4,194,304 cache cells.
Peak device-local VMA allocation was 9.87 GiB. Enabling reflections at the same density
raised the peak to 15.01 GiB. The world cache fell to zero capacity, and frame 256 reported
an inactive gather after a fallback to minimum PBR. Its image was excluded from scoring.

The four-pixel calibration scene with reflections enabled retained 2,097,152 cache cells
and matched 3,830 receivers in each 4,096-frame CUDA comparison. One-bounce mean bias was
+0.308%, within the ±1.971% limit. Spatial RMS was 0.00226697, above 0.00222612.
At sixteen vertices, bias was −0.243% and RMS was 0.00374970, both within their limits
of ±1.491% and 0.00741765. The largest one-bounce regression was an illuminated receiver
with effectively zero probe support at four pixels. In the Sponza frame pairs, large
motion differences cluster along arch edges. Pixels differing by at most one display
level account for 88.1% of the static squared difference.

The evidence root is `build/checks/native-gi/p2-resume/`. The full-frame gates are in
`quality4/report.json`, the 4K memory runs in `dense-diffuse/` and `dense-reflections/`,
and the matched-reference runs in `cuda4-1/` and `cuda4-16/`. Each run records the
canonical ZenApp binary, shader and input hashes, command arguments and actual render state.

#### Coverage at thin surfaces

The placement and gather passes compared projected window coordinates with integer pixel
indices. Using pixel centers for distance tests and checking the surrounding tile ring
where the four usual gather taps have weak support recovered the largest missed
calibration receiver. Its one-bounce mean irradiance was zero before the change, 0.01897
afterwards, and 0.01752 in the CUDA reference. The focused fast-pan comparison changed
from 0.02931% above four levels to 0.02332%, and RMS from 0.25431 to 0.24781.

With filter radius held constant in screen pixels, four-pixel probes passed **7 of 15**
full diffuse image checks. Light recovery reached 0.22845 RMS against 0.22964. Slow
pan at frame 64 and fast pan at frame 32 passed, along with both flicker and spark
checks. Static cross-launch RMS was 0.20865 against 0.17823. Six of the remaining
seven motion comparisons missed by less than 0.007 percentage points. Dolly at frame
8 was 0.02121% against 0.00898%. See `quality-coverage/report.json`.

Both diffuse-only CUDA runs retained 8,388,608 cache cells. One-bounce RMS with the
expanded gather was 0.00223876 and with the screen-space filter radius was 0.00223764.
These exceed the 0.00222612 limit. The corresponding sixteen-vertex RMS values were
0.00371356 and 0.00371080. Integrating around the anchor's sampled material normal
also exceeded the one-bounce limit at 0.00223929. That trial is archived under
`shading-cuda-1/` and `shading-cuda-16/`.

Eight-pixel spacing with glossy shading active kept native GI enabled throughout the
256-frame Sponza memory check. Frame 256 reported 162,000 probes, 4,194,304 world-cache
cells and a 3840×2160 fullscreen framebuffer. Peak device-local VMA allocation was
9.91 GiB. See `selected-reflections/report.json` and its validation log.

#### Eight-pixel base grid with adaptive diffuse probes

The diffuse-only path keeps the eight-pixel base grid and gives every tile one adaptive
slot for an underrepresented surface. At 4K this is 129,600 base and 129,600 adaptive
slots. When specular occlusion is enabled, four base tiles share one adaptive slot,
giving 162,000 total slots with glossy history. Disabling specular occlusion releases
the GGX and glossy histories and omits their trace and filter passes. The diffuse trace
also omits the additional ray records consumed exclusively by the GGX prefilter.

The first diffuse-only runs matched 3,830 calibration receivers. At one bounce, mean
bias was +0.458% and RMS was 0.00222258. At sixteen vertices, bias was −0.253% and
RMS was 0.00369532. Both passed. The full Sponza suite passed **6 of 15** checks, including
fast pan at frames 32 and 64. All nine camera-motion errors decreased from the previous
eight-pixel checkpoint. Static RMS decreased from 0.22153 to 0.21108. Light recovery
increased from 0.24342 to 0.24515, above the 0.22964 limit.

| Adaptive eight-pixel motion error, % of pixels above four display levels | Frame 8 | Frame 32 | Frame 64 |
|---|---:|---:|---:|
| Slow pan | 0.01931 | 0.02029 | 0.01913 |
| Fast pan | 0.02083 | **0.02923** | **0.02341** |
| Dolly | 0.01982 | 0.02804 | 0.02866 |

The 28 targeted GPU integration cases passed, including diffuse-only and glossy pass
lists on the hardware and compute backends. The results and capture provenance are
in `adaptive-cuda-1/`, `adaptive-cuda-16/`, `quality-adaptive/` and `tests-adaptive.log`.

The same build passed all 15 reflection stability checks on the glossy fixture. Static
specular RMS was 1.31134 against the retained limit of 1.42706. In 4K Sponza, enabling
reflections preserved the native gather and 4,194,304 cache cells at frame 256. On the
calibration scene, the combined path passed both CUDA references with RMS 0.00222265
and 0.00369613. See `glossy-motion-adaptive/`, `glossy-static-adaptive/`,
`adaptive-combined/` and `combined-cuda-1/`, `combined-cuda-16/`.

### Phase 3 glossy receiver density and motion (2026-11-19)

The 4K HW glossy fixture passes the **2.0 ms glossy trace and denoise budget** in static and
panning captures, all **15/15** reflection stability comparisons, and the matched static
specular gate. The fixture covers mirror, glossy and probe-shaded materials
at roughness 0.04, 0.18 and 0.55. The transition to probes stays at 0.35–0.4.

Use one receiver per 4×4 full-resolution pixels (960×540 receivers at 4K). A mirror receiver
below 0.08 traces one reflected direction. Other eligible receivers sample three GGX directions.
Trace, temporal reprojection and full-resolution reconstruction share the same grid, including
the clipped receiver at a partial edge tile. Compared with the 2×2 grid this saves 237.3 MiB
of glossy history and 94.9 MiB of transient glossy buffers at 4K.

The 2×2, two-ray dense filter passed 14/15 stability comparisons. Moving to a 3×3 grid alone
also passed 14/15. Slow-pan pose 32 increased from 0.4972% to 0.5174% of pixels above 4/255,
against a retained limit of 0.4531%. The revised filter reads the corresponding history texel
directly when the receiver image is unchanged. Reprojection discounts history age for rejected
taps and subpixel interpolation. Both spatial passes prefer established neighbors when repairing
a young receiver. The revised 3×3 configuration passed 15/15 and reached 1.22659 static RMS,
but its glossy stages cost 2.174 ms median during the pan.

The 4×4 two-ray grid brought the panning median to 1.351 ms. Its only quality miss was dolly
pose 8 at 0.4339% against a retained limit of 0.4258%. Relaxing hit-distance rejection for
subpixel parallax worsened it to 0.4607%, so the final build retains hit-distance rejection.
A third GGX sample per eligible non-mirror receiver brought dolly pose 8 to 0.4059%.

| Glossy fixture, 3840×2160 | Native | Retained |
|---|---:|---:|
| Specular RMS against sparse exact reference, 8-bit units | 1.33850 | 1.42706 |
| Flicker mean / p999 standard deviation, 8-bit units | 0.01230 / 0.58447 | 0.03137 / 1.48922 |
| Static cross-launch RMS | 0.22175 | 0.41916 |
| Light recovery RMS at frame 128 | 0.31298 | 0.67080 |
| Slow pan, % above 4 at poses 8 / 32 / 64 | 0.3890 / 0.4075 / 0.4169 | 0.5243 / 0.4531 / 0.6284 |
| Fast pan, same metric and poses | 0.3740 / 0.3502 / 0.2323 | 0.5752 / 0.6029 / 0.3396 |
| Dolly, same metric and poses | 0.4059 / 0.4124 / 0.4185 | 0.4258 / 0.6588 / 0.9614 |
| Static / moving spark pixels per frame | 0 / 2.17 | 0 / 25.83 |

The motion run used 512-frame warm-up and holds, each renderer's own settled references,
exact direct shadows, one-bounce hit lighting, and disabled diffuse and environment lighting.
ZenApp ran fullscreen with async compute off. Windows GPU Process Memory counters sampled a
maximum of 12.10 GiB dedicated to ZenApp during the moving launch, including capture work.
The static comparison used a 4K render and a 1280×720 framebuffer. Reports under
`build/checks/native-gi/p3-grid` include `quality-three-rays/report.json` and
`static-three-rays/report.json` with the build, input hashes and frozen retained baselines.

| RTX 5080 base clocks, 30 frames per capture | Static | 1°/frame pan |
|---|---:|---:|
| Glossy trace + temporal + history repair + spatial, median | 1.415 ms | 1.527 ms |
| Same four stages, p95 | 1.432 ms | 1.556 ms |
| Rough-probe GGX prefilter + specular spatial filter, median | 3.320 ms | 2.803 ms |
| All six reflection stages, paired median | 4.741 ms | 4.332 ms |
| Whole frame, median | 25.468 ms | 56.635 ms |

The 2.0 ms row in the frame budget covers glossy rays and denoising. Probe filtering is
charged separately. Both Nsight captures ran canonical ZenApp with a 3840×2160 fullscreen
framebuffer and render extent, threaded RHI, HW RT and async compute off. Screen-probe
tracing took 37.620 ms during the pan. The whole-frame 16.7 ms target remains open.

All 12 targeted GPU integration cases passed across the four submission modes, including
the native HW and Compute pass lists. A validation-enabled HW launch at 1279×721 exercised
partial edge tiles. `build/checks/native-gi/p3-grid/audit-three-rays.json` cross-checks
the executable and compiled shader hashes across quality, static and panning timing, and
odd-extent captures. The two Nsight runs retain their commands, input hashes and per-frame
pass timings under `timing-three-rays/` and `pan-three-rays/`.

### Phase 3 initial qualification checkpoint (2026-11-19)

The initial reflection implementation passed the listed static specular-error gate and the
**glossy rays plus denoise** budget from the table above on the build identified below.
This does not close Phase 2 or qualify the complete native renderer.

This checkpoint used a roughness threshold of **0.4**, with a smooth blend to probes over **0.35–0.4**,
and one glossy ray per eligible half-resolution receiver (half each axis, one quarter of the
4K pixel count). Below **0.08**, HW mirrors use the resolved reflection direction and exact hit
lighting; other glossy hits use the world cache. Rough reflections use the probe GGX atlas.
Trace and denoise work is masked by receiver eligibility; dispatches still cover the half grid.
The glossy fixture exercises roughness 0.04, 0.18 and 0.55. These choices meet the measured
gates; they are not a claim that this is the optimal threshold for every scene.

| Check | Current result | Scope |
|---|---:|---|
| Specular RMS against sparse exact reference, 8-bit units | **1.16330 native / 1.42706 retained** | Pass; matched one-secondary-surface hit lighting, diffuse and environment lighting off |
| Glossy trace + temporal + history repair + spatial | **1.986 ms median, 1.996 ms p95** | Pass against 2.0 ms; 30 paired frame samples, maximum 1.999 ms |
| Rough-probe GGX prefilter + specular spatial filter | **1.443 ms median** | Additional probe work, accounted separately in Phase 2 |
| All six reflection-specific stages together | **3.429 ms median** | Not a 2.0 ms result for the combined stages |
| Pinned frame flow | **4/4 passed** | Native HW and Compute paths in all four submission modes; no validation errors |

Timing used canonical `ZenApp.exe`, RTX 5080, base clocks, 3840×2160 rendering and framebuffer,
fullscreen, threaded RHI and async compute off. Diffuse GI, reflections and native shadow reuse
were enabled. Static quality used a 3840×2160 render with a 1280×720 windowed framebuffer and
async compute off. The reference and retained images are the hashed frozen baseline from
`p3-audit-20261118/specular`; the native measurement is fresh. Timing has little headroom.

Artifacts: `build/checks/native-gi/p3-20261119/specular-final/report.json`, `timing/summary.json`,
`timing-gate.json`, `frame-flow.log` and `build-final.log`. Executable SHA-256 is
`e2eb618588b51d52fff3a9df672bf62a71e409aa7731d7c4727cff664ff0f385`; the static report records
compiled-shader SHA-256 `1040d205e5ad8d2f219121ca2b37e2ba2211c32d2c88909378cdf30a9d9fc135`
and input/setup hashes. Captures, commands and logs are retained with lossless filesystem compression.

`tools/native_gi_quality.py --reflections-only` now isolates the reflection signal for the
supplemental motion/light-recovery checks using the same one-bounce contract, exact direct
shadows and no environment lighting. Frozen baselines from another signal are rejected.

**The initial build missed the shared motion-quality gates.** The complete 512-frame warm-up /
512-frame held-reference comparison at `p3-20261119/motion-serial/report.json` passes only 2 of
15 comparisons. It uses 4K fullscreen, HW RT, async compute off and each mode's own settled
reference. These are reflection-only findings, not the Phase 2 diffuse measurements.

| Supplemental metric | Native | Retained | Result |
|---|---:|---:|---|
| Flicker mean std, 8-bit units | 0.04080 | 0.03137 | Open |
| Flicker p999 std | 2.07289 | 1.48922 | Open |
| Static cross-launch RMS | 0.39119 | 0.41916 | Pass |
| Light recovery at 128 frames, RMS | 1.07569 | 0.67080 | Open |
| Slow pan, % pixels above 4, poses 8 / 32 / 64 | 1.0673 / 1.3323 / 1.4063 | 0.5243 / 0.4531 / 0.6284 | Open |
| Fast pan, % pixels above 4, poses 8 / 32 / 64 | 1.0055 / 1.0975 / 0.8174 | 0.5752 / 0.6029 / 0.3396 | Open |
| Dolly, % pixels above 4, poses 8 / 32 / 64 | 0.8647 / 1.1273 / 1.2478 | 0.4258 / 0.6588 / 0.9614 | Open |
| Static sparks, pixels/frame | 0 | 0 | Pass |
| Moving sparks, pixels/frame | 26.5833 | 25.8333 | Open |

The first attempt, using the source setup's async-compute setting, lost the Vulkan device in
the retained baseline before native ran. That incomplete run is retained at `p3-20261119/motion`
and is not a quality comparison. All four launches of the async-disabled retry completed
without validation errors. This does not establish the cause of the original device loss or
qualify async compute. Phase 0 requires motion, recovery, sparks and flicker for every later
phase, so these failures prevent full Phase 3 closure even though its two specific gates pass.
No renderer algorithm was
changed in this qualification pass.

### Selection and frame flow

`rendering.gi.cone.screenProbeGI=true` selects full-resolution probe gather and enables the world
cache. `worldRadianceCache=true` alone selects cached hits in the retained pixel renderer.
`worldCacheFeedback=false` isolates one-secondary-surface transport for reference comparisons.
`nativeShadowReuse` selects coarse visibility with exact refinement in the native HW path.
Reference sampling and ray-count diagnostics select the retained renderer. The **Native** editor
preset selects probes, the world cache and full-resolution output; the other presets retain their
existing paths. Configuration examples include the corresponding `voxel_gi_*` settings.

The native frame records cache seed/request scheduling, cache update and resolve, probe placement,
proposal, trace, temporal/spatial filtering, GGX prefiltering, glossy trace/temporal/history repair/
spatial filtering, and the full-resolution gather. Diffuse pixel trace, diffuse ReSTIR and diffuse
pixel ReLAX do not run. The gather reads filtered probe lighting directly. Histories advance
only after successful submission. Resizes, camera cuts,
environment changes and failed frames invalidate the appropriate histories.

### Implemented policies

- **World cache:** sparse distance-scaled keys, six directional records and surface sample points.
  The first claim fixes a cell's geometric normal for compatibility tests. Each later compatible
  claim (normal within 18 degrees, plane within 5% of the cell) moves the update origin and
  normal-mapped shading normal, so a cell averages the surface that queries it. A fixed
  first-claim origin made settled lighting depend on camera history.
  Base cell size scales with the granted capacity: `max(0.0005, 0.001 * sqrt(2097152 / capacity))`
  times scene extent. Near finite lights, the cache level is limited by light distance divided by eight, down to four levels below the base. Up to 430,080 primary update invocations trace bounded fallback paths when
  cached lighting is unavailable or a feedback segment is too short. These paths may issue
  additional diffuse and shadow rays; they use weighted roulette after two unresolved hits.
  Updates read the previous resolved cache; a separate resolve publishes the new values. Existing cells refresh sparsely; missing
  cells request initialization immediately through a 262,144-entry queue. Geometry edits replace
  incompatible anchors. Each cell stratifies 64 actual updates per independently scrambled block.
  The base sampler mixes uniform and cosine-weighted hemisphere directions. With feedback
  and a contributing local light, half of the proposals sample a normalized Henyey–Greenstein
  lobe around the strongest local source. Both components use their combined density. The
  one-bounce comparison uses the base sampler.
  Rotating 8x8 tile seeds visit every visible receiver within 64 frames. Scheduling retains
  recently requested cells for 240 frames and evicts them after 480 frames without a request.
  Newly requested cells are scheduled before mature-cell refreshes. Resolve visits only the selected update cells. When the update window gives each invocation
  a unique cell, it writes accumulation directly; shared updates keep atomic additions.
  Six records store FP32 intensity and FP16 normalized color coordinates, preserving small
  intensity changes in long feedback histories.
  Normal interpolation preserves the anchor's first moment and cannot fall below the signed
  first-moment contribution at a tilted receiver; the oblique-light calibration checks this.
  History is capped at 16 updates while lighting changes propagate or a cell gathers its first
  256 samples, then grows to 128 updates. A GPU light signature restarts history without a CPU
  revision hint; a disabled camera light does not trigger resets. A sequence of low Monte Carlo
  samples does not discard rare bright transport. Renderer analytic/emissive/shadow toggles
  and material revisions advance a persistent lighting epoch, so they also restart stale histories. Camera cuts retain
  world-cache lighting while resetting screen history. Small memory-budget fluctuations retain an
  existing cache up to twice the planner's quarter-headroom grant instead of clearing it.
  Physical material
  factors and intensity are applied once. Missing cells borrow the nearest adjacent, coplanar, similarly facing cell
  while still requesting exact initialization. Native probe and cache-update rays use local
  lighting and bounded continuation when no sample is usable; a sampled black cell is valid.
  Opposite-facing and displaced planes are rejected for exact lookups as well as borrowing. Incompatible surfaces
  sharing a primary cell receive a secondary key containing quantized orientation and plane offset.
- **Screen probes.** The current candidate uses 129,600 base slots at 4K with eight-pixel spacing.
  Diffuse-only rendering allocates one adaptive slot per tile and evaluates 16 candidate receivers.
  With specular occlusion enabled, each of the 32,400 adaptive slots evaluates 64 candidates across
  four tiles. A second placement pass measures support against the anchored base probes.
  In each 16-ray batch, a receiver with local lights takes 12 samples around up to eight
  lights, two uniform-hemisphere samples and two samples from an octahedral directional
  distribution. Light selection uses distance, radiant intensity and the receiver normal.
  Without a local light, the two broad components each receive eight samples. When the
  environment lights the scene, four samples come from its importance distribution instead.
  That leaves eight light, two uniform and two directional samples, or six uniform and six
  directional without local lights. Every valid ray uses the full mixture density. The
  directional distribution uses incident-radiance spherical harmonics and a local-light prior.
  The GGX atlas serves reflections.
  Compatible history stays anchored in world space and can be recovered from the last three
  submitted frames. A relocated probe borrows nearby compatible history for up to 64 frames.
  Stable anchors accumulate up to 1,152 frames. Once the view is still relative to a drifted
  anchor, a coplanar probe returns to its tile center with up to 256 frames of history, so
  settled placement does not depend on the preceding motion. A probe without a coplanar
  predecessor starts fresh with the young-probe ray allocation; borrowing a differently
  oriented neighbor's history was removed on 2026-11-19. A relocated predecessor's irradiance
  moves to the new normal with the stored L1 gradient.
  Source changes restart history, with a 16-frame cap during propagation.
  Horizontal and vertical spatial passes weight samples by plane, surface identity, age
  and variance. Young probes sample up to 12 neighbors in each direction per axis across
  a radius of up to 24 tiles. The radius shrinks with age. Three interleaved groups
  estimate the spread of neighboring irradiance and limit isolated bright estimates while
  history is young. The gather interpolates the filtered coefficients at full resolution
  using each receiver's shading normal. Each pixel weights the base or adaptive probe of
  the 3×3 tiles around its own tile with a tent of 1.25 probe spacings, so every tap reaches
  zero weight before the tile window changes.
- **Reflections:** 112 packed RGBA16F texels per probe represent four GGX roughness levels.
  Roughness below 0.4 uses a glossy receiver every fourth pixel along each axis. Each eligible
  receiver samples three GGX directions, except below 0.08 where one mirror ray uses the resolved
  direction and HW mirrors receive exact analytic lighting. Glossy temporal history, young-history
  repair and spatial filtering operate on that signal alone. Roughness 0.35–0.4 blends the two results. Lag classification uses completed
  history so a new bright sample cannot immediately promote itself into a lighting change.
- **Direct visibility:** a 4x4 coarse grid plus full-resolution refinement. Masks describe visibility
  independently of normal maps; each receiver still evaluates its own BRDF. Incompatible geometry,
  disagreeing light bits and bright receivers near point-light fixtures use exact queries.
- **Compute:** static and light-detail geometry use the distance field. Current dynamic triangles
  are intersected in Compute alongside that field, with instance bounds and material alpha tests.
  This avoids making an animated object's entire lighting unavailable while its field rebuilds.
  The dynamic triangle loop is linear within intersected instances; large animated scenes still
  need cost qualification. Hit materials use the current voxel reflectance/emission fields and
  the same world cache. The existing field's stale-static-region rejection remains active.

Probe capture raw/reconstructed channels both contain gathered output; they are not independent
pixel-ray observations. Cache capture component 13 contains sampled exact-cache hit/miss counts,
scheduled cells and frame number. Component 14 contains fixed-point overflow samples, dropped
requests and queued requests. Components 15–17 contain direct cache irradiance/validity, the
anchor position/update count, and the anchor shading normal/cell size.

### Phase 1 qualification (2026-11-17)

The final configuration uses 557,056 updates per frame. All results below use the canonical
ZenApp executable, explicit `.zenrender` setups and the same final shader build. The executable
SHA-256 is `77f16466416cae39f41fe733c24a27c6c4c6b7c4c7409c473e9e66165416e7c2`.

| Phase 1 gate | Final result | Evidence under `build/checks/native-gi/` |
|---|---|---|
| Update + resolve, static | 1.469408 ms median; 1.478037 ms p95; **passes 1.5 ms** | `phase1-controls-static/summary.json` |
| Update + resolve, continuous pan | 1.472497 ms median; 1.481767 ms p95; **passes 1.5 ms** | `phase1-controls-pan-repeat/summary.json` |
| Cache-only 16-vertex energy, launch A | −0.2394%; RMS 0.011366; **passes ±0.6592%** | `phase1-controls-energy-a/report.json` |
| Cache-only 16-vertex energy, launch B | +0.1549%; RMS 0.010849; **passes ±0.6592%** | `phase1-controls-energy-b/report.json` |
| On/off/on light response without CPU revision hints | Within 5% by frames 65–80 in all four submission modes | `implementation/phase1-controls-tests.log` |
| Analytic-lighting toggle and camera recovery | Toggle response within 80 frames; pan/dolly coverage ≥99% and mean within 5% of the later image, 65–80 frames after motion stops | Same log |
| Resource and launch checks | 24 native/cache GPU tests, 2 cache-budget tests, 22 runtime-launch tests pass; ZenApp and editor build | `implementation/phase1-controls-*.log` |

Timings are paired per-frame sums from 30-frame Nsight GPU Trace captures on RTX 5080 at base
clocks, 3840×2160 fullscreen, async compute off. Both accepted timing captures retained
4,194,304 cells (984 MiB of WRC resources). Seeding, scheduling and counter reset add about
0.275 ms beyond update/resolve. Whole-frame medians were 31.507 ms static and 34.348 ms panning;
these include the retained half-resolution pixel renderer. Device-wide memory peaked at
12,660 of 16,303 MiB under Nsight; this is not an application-only allocation measurement.
The CSVs are `implementation/phase1-controls-device-memory.csv` and
`implementation/phase1-controls-pan-device-memory.csv`.

Energy captures render a full 4K G-buffer and read the cache directly, before pixel/probe filters,
after 4,096 frames. Both use 8,388,608 cells (1,960 MiB), match the same 3,830 CUDA receivers,
cover 100% of valid receivers, and report zero dropped requests and zero encoding overflows at
the captured frame. The reference tolerance is unchanged: three standard errors estimated from
the reference halves, or 0.0001499895 in linear irradiance/π. The direct-only and one-vertex rows
in these reports are cross-contract diagnostics; the Phase 1 gate uses the 16-vertex row.

The uniform fixture was recooked after removing 72 unreferenced images, 72 unreferenced textures
and one unused sampler. Geometry and material data are unchanged; this avoids paying GPU memory
for textures that the constant-material reference never uses. The source/package hashes and
verification are in `implementation/phase1-fixture.json`. The CUDA reference remains
`phase1-reference-final/reference.npz`.

The supplementary `phase1-cache-quality` run records the raw cache debug view in textured
Sponza at 3840×2160 fullscreen, HW tracing and async compute off. Both reference and moving
launches shrank to 2,097,152 cells (496 MiB); this is a memory-pressure case, separate from the
four-million-cell timing qualification. The moving launch warms up for 4,096 frames. Reference
poses are held for 1,024 frames each; these held images are diagnostic comparisons, not an
independent transport reference or proof of convergence. Values below use the Reinhard/sRGB
debug image in 8-bit units, before any probe or pixel reconstruction.

| Raw-cache diagnostic | Result |
|---|---|
| Static flicker, mean / p99.9 standard deviation | 0.327 / 34.466 |
| Last 16-frame difference in the reference hold, RMS | 1.360 |
| Independent static launch versus held reference, RMS | 15.202 |
| Slow pan, RMS at steps 8 / 32 / 64 | 15.572 / 15.104 / 14.076 |
| Fast pan, RMS at steps 8 / 32 / 64 | 15.258 / 12.461 / 10.802 |
| Dolly, RMS at steps 8 / 32 / 64 | 14.319 / 14.421 / 14.759 |
| Detected static / moving spark pixels per frame | 0 / 0 |
| Light move, RMS after 1 / 64 / 256 / 1,024 frames | 23.308 / 14.755 / 14.836 / 13.755 |
| Light move, pixels differing by more than 4 after 1,024 frames | 23.50% |

The controlled recovery regression establishes the stated 80-frame response on its fixture.
It does not establish an 80-frame full-image recovery bound for textured Sponza. The raw-cache
light comparison remains far above the short-term reference residual after 1,024 frames; the
image-level convergence and motion limitations remain visible in this record. The zero spark
count only applies to the detector's isolated transient criterion and does not imply a smooth
image. Reports retain commands, actual extents/capacity, executable, shader and input hashes.

The larger 573,440-update trial passed energy but took 1.512/1.514 ms static/pan and was rejected.
The earlier 1.338 ms mixture timing had shrunk to 2,097,152 cells and is not the accepted
four-million-cell cost result. `phase1-controls-pan` likewise records a two-million-cell
memory-pressure launch; `phase1-controls-pan-repeat` is the matching four-million-cell capture.
Shorter histories, finer cells and nested direct-light initialization did not qualify together;
their reports are retained, and the nested-ray prototype was removed.

`implementation/phase1-final.json` collects the final gate results and their limitations.
Lossless filesystem compression of completed Phase 1 captures reclaimed another 53.96 GiB
across 157 files, bringing this resumed work's cleanup to 79.18 GiB. No source, executable or
capture was deleted then; the raw captures were deleted on 2026-11-18 (see Cleanup). The per-file records are
`implementation/phase1-compression.json` and `implementation/phase1-compression-final.json`.

### Phase 2 receiver-query investigation (2026-11-18)

E13 remains the selected build. A focused ZenApp comparison holds the fast-pan/32 pose
for 2,048 frames, returns to the base pose, and reaches the same target after 32 moving frames.
Only frames 2,048 and 3,104 are captured. Runs use diffuse-only Sponza, 3840×2160 fullscreen
hardware tracing, async compute off and validation on. This camera history differs from the
full quality gate. The experiment scripts, exact commands, hashes, actual runtime settings
and reports are in `build/checks/native-gi/p2-receiver-20261118/`.

At every fourth pixel on both axes, an independent receiver pass traced 128 cosine-weighted
paths through the same four-million-cell world cache. The two poses use the same directions
and random numbers at each sampled pixel. This measures sensitivity to camera history at a
fixed sample set. It does not measure the flicker of 128 newly scrambled rays per frame.
A separate capture recorded gathered history age at the moving pose.

| Moving probe age | Sampled pixels | Probe pixels above 4/255 | Receiver pixels above 4/255 |
|---|---:|---:|---:|
| Zero | 1,423 | 2.178% | 0.070% |
| 1–31 | 207,996 | 0.183% | 0.197% |
| 32–64 | 23,003 | 0.691% | 0.043% |
| 65–254 | 34,213 | 0.067% | 0.035% |
| 255 or older | 251,765 | 0.003% | 0.028% |

The receiver RMS over all sampled pixels rose from 0.426 to 0.549 despite a smaller
fraction above four levels. The result supports selective reuse on borrowed histories and
exposes substantial variance elsewhere. Whole-image results on this short path:

| Variant | Pixels above 4/255 | RMS in 8-bit levels |
|---|---:|---:|
| E13 probe control | 0.1153% | 0.4228 |
| Confidence-weighted per-pixel reprojected gather | 0.1113% | 0.4306 |
| Four probe rays guided toward a previously sampled secondary hit | 0.1627% | 0.4463 |
| Probe reservoir reconnected at the receiver, with visibility and a fresh ray in holes | 0.2728% | 0.5733 |
| Retained full-resolution per-pixel ReSTIR and diffuse denoiser | 0.0800% | 0.3710 |

The retained run used four spatial samples and its world cache shrank to 524,288 cells.
The selective reconnection trial kept a secondary surface reservoir at each probe,
carried it through stationary-anchor history, and estimated a visibility-tested change
in illumination from probe to receiver. Its fresh hole samples and reconnection residual
were too noisy in aggregate. It was reverted.
That comparison includes the denoiser and the capacity change. The reprojected-gather
and directional-guidance trials were rejected. A separate fixed-camera light test retained
short probe and cache priors on a light edit. At frame 128 its RMS rose from 0.3424 to 0.3630,
and after restoring the original light the settled difference rose from 0.2118 to 0.2287.
The original reset policy was restored.

#### Local light motion and independent sampling

The fixed-camera light test runs 2,048 frames at the original position, moves point light 3
by 0.04 over four frames, holds it for 1,024 frames, then restores it for 1,024 frames.
Each run captures the two endpoints, the 128-frame response and the restored view. The
128-frame column compares the response with the held **moved-light** view. The restored
column compares two settled views at the original light position. Values are RMS in display levels.

| Light-history policy | Frame 128 | Restored |
|---|---:|---:|
| Full source reset | 0.3424 | 0.2118 |
| Preserve distant history from the immediately preceding source | 0.2870 | 0.2043 |
| Recognize the last four source positions during continuous motion | 0.2697 | 0.2134 |
| Shorten the swept response region and cap distant probe history at 256 frames | 0.2412 | 0.2205 |
| Keep sky history and allow up to 512 distant frames | 0.2890 | 0.2254 |

The 256-frame version is the candidate. GPU-detected changes to intensity, light direction,
source membership or renderer lighting policy still restart history. During a pure finite-light
translation, nearby probes and cache cells restart, and distant ones retain a bounded prior.
Cells can skip updates during successive light edits. The four preceding signatures limit which
old states can contribute. The focused test shows faster response and a higher restored residual.
The official light and static gates use different camera and lighting histories.

| Focused fast-pan experiment | Pixels above 4/255 | RMS |
|---|---:|---:|
| E13 probe control | 0.1153% | 0.4228 |
| Additional world-space history of the probes' own lighting | 0.1138% | 0.4236 |
| Analytically integrate a previous L1 radiance estimate and sample the residual | 0.1137% | 0.4253 |
| Stratify the probe random sequence on the dominant world-space surface plane | 0.1165% | 0.4281 |
| Eight ray batches on anchors aged up to 32 frames, four up to 64 | 0.0751% | 0.3755 |
| Curvature-aware spatial weights and first-moment normal transfer | 0.1295% | 0.4187 |
| Gather each compatible base and adaptive probe once | 0.1122% | 0.4208 |

The extra batches reduced failing pixels in the 1–31 age group from 6,292 to 3,016.
The 32–64 group went from 2,361 to 2,384. Independently changing all screen-probe
direction scrambles reproduced 86% of the control failures in this latter group, with the
same error sign at 99%. That points to a persistent reconstruction error on those surfaces.
The extra-batch build recorded 5.569 ms for placement, proposal and tracing and 7.538 ms
for diffuse overall in the 30-frame base-clock pan trace. It exceeds both budgets and was
reverted. The separate retained 960×540 reservoir test, with the same four-million-cell
world cache, reached 0.3619% and 0.5252 RMS on the focused pan.

#### Ray allocation at newly exposed receivers

The full diffuse qualification run with local light invalidation, a filtered prior at relocated
probes and the unique-tap gather passed stationary flicker (mean 0.00415, p99.9 0.4961) and
both spark gates. Static cross-launch RMS was 0.22255 against 0.17823. Frame-128 light recovery
was 0.29746 against 0.22964. All nine pan and dolly comparisons exceeded their limits. Pixels
changing by at most one display level between the two settled lighting states accounted for
86% of the light recovery squared error. The full report is in
`p2-receiver-20261118/local-qualified-quality/report.json`.

On the focused fast pan, the 1–2 and 3–7 frame history bands together accounted for 4,925 of
9,563 control failures. Additional independent paths at these receivers contribute to diffuse
irradiance. The rough-specular prefilter continues to use its original directional sample.
Each ray slot stays at 32 bytes.

| Focused 4K Sponza trial | Pixels above 4/255 | RMS in display levels | Pan placement, proposal and trace | Pan total diffuse |
|---|---:|---:|---:|---:|
| E13 reference | 0.1153% | 0.4228 | 2.653 ms | 4.607 ms |
| Eight batches at age 1–2, four at 3–6, unique-tap gather | 0.0734% | 0.3837 | 3.323 ms | 5.626 ms |
| Four batches at age 1–2, two at 3–6, square adaptive groups with coverage scoring | 0.0729% | 0.3864 | 3.730 ms | 5.733 ms |
| Four and two batches, original placement and gather | 0.0767% | 0.3921 | 3.067 ms | 5.065 ms |
| Same allocation, proposal mixture learned from returned irradiance | 0.0932% | 0.4064 | — | — |
| Same allocation, history and filter radius limited by harmonic mean hit distance | 0.1071% | 0.4227 | — | — |
| Same allocation, ray origins snapped to a verified dyadic lattice on the visible facet | 0.1407% | 0.4511 | — | — |
| Same allocation, 16-bit cumulative probabilities | 0.0770% | 0.3905 | 3.039 ms | 5.044 ms |

The original-placement four/two candidate passed one 4,096-frame one-bounce CUDA comparison,
with RMS 0.00222282 and mean bias +0.438%. The square placement candidate exceeded the same
RMS limit of 0.00222612 with 0.00222905. The ray budgets above include adaptive placement and
proposal generation. The focused visual comparison uses a different camera history from the
official quality gates. Trial reports and capture provenance are under `p2-receiver-20261118/`.

The full four/two original-placement run (`birth-lean-quality/report.json`) passed both
stationary flicker checks and both spark checks. The table gives its motion error as the
percentage of pixels exceeding four display levels at the same pose.

| Motion | Frame 8 | Frame 32 | Frame 64 |
|---|---:|---:|---:|
| Slow pan | 0.03947% | 0.04748% | 0.05871% |
| Fast pan | 0.04901% | 0.07850% | 0.04719% |
| Dolly | 0.04996% | 0.05112% | 0.06140% |

All nine exceed the retained limits. Static RMS was 0.22204 and light recovery RMS was 0.30198.
The stored proposal can be reduced from 65 words to 34 by packing the 64 cumulative probabilities
in pairs. Sampling and PDF evaluation use the same quantized distribution. At 162,000 probes
and four history banks this saves 76.6 MiB. Full qualification above used the 65-word layout.
An eight-frame ZenApp diagnostic behind the original gather read each adaptive slot on the GPU.
At 4K, 12,216 of 32,400 slots contained a valid receiver and the other 20,184 had been written
and rejected. See `adaptive-write-audit/audit.json`. The diagnostic was removed after scoring.

The energy runner now requests the four bilinear taps for each reference receiver plus pixel
zero for cache diagnostics. ZenApp counts coverage over the full frame before writing these
records. The four/two run wrote 4,718,904 bytes of capture data and matched 3,830 receivers.
The convergence check and full-frame ground-truth export remain available to ordinary batch runs.

#### Quality-first sampling

With timing qualification deferred, the probe tracer now takes 528 paths at ages 1–8, 272 at
9–32, 144 at 33–64, 80 at 65–128, and 32 on mature probes. Recent lighting and geometry
changes can also trigger the extra paths. The first ray in each lane continues to supply the
directional moments and rough-reflection sample. Additional paths stratify the diffuse estimate
within each frame. The temporal filter tracks how many batches contributed to the accepted
prior. The spatial filter estimates the irradiance slope along each axis to account for a
weighted sample footprint whose center shifts away from the receiver.

On the focused 4K fast pan, 0.04581% of pixels exceeded four display levels, with RMS 0.32915.
The lower-sample version of the same filter reached 0.06981% and 0.37569. Against E13, failing
pixels in the baseline age 1–31 group decreased from 6,292 to 1,778, and ages 32–64 from 2,361
to 1,206. Pixels with zero baseline history remained at 471 failures against 468. The
fixed-camera light test measured 0.21002 RMS at frame 128 and 0.21722 after restoration.
The first 4,096-frame CUDA runs passed energy and spatial RMS at both depths. One-bounce RMS
was 0.00222190 with +0.416% energy bias. Multi-bounce RMS was 0.00369700 with −0.351% bias.
Reports are in `p2-receiver-20261118/quality-batches*`.

**Paused checkpoint, 2026-11-18.** The eight-pixel quality-first configuration is the
restart point in the worktree. Its full 4K qualification passed both stationary flicker
checks and both spark checks. Static cross-launch RMS was 0.22153 (limit 0.17823), and
frame-128 light recovery RMS was 0.24342 (limit 0.22964). Each cell below shows the
measured percentage of pixels above four display levels followed by its retained limit.

| Motion | Frame 8 | Frame 32 | Frame 64 |
|---|---:|---:|---:|
| Slow pan | 0.03049 / 0.00907 | 0.03496 / 0.01137 | 0.04130 / 0.01501 |
| Fast pan | 0.04004 / 0.01208 | 0.04815 / 0.03674 | 0.03057 / 0.02844 |
| Dolly | 0.04227 / 0.00898 | 0.03845 / 0.01542 | 0.04925 / 0.01922 |

The four-pixel trial reached 0.02931% and 0.25431 RMS on the **focused** fast pan, down
from 0.04581% and 0.32915 with eight-pixel spacing. Both snapshots reported 648,000 probes,
4,194,304 cache cells and hardware tracing at 3840×2160 without tier fallback. This required
omitting the reflection resources when reflections and specular occlusion were both disabled.
An earlier dense run fell back to the minimum PBR tier and was rejected. The focused harness
had an eight-pixel probe-count check; `score_four.py` checked the new count from tile geometry
and repeated the original image comparison on the retained pair before deleting the images.

The full eight-pixel report is `p2-receiver-20261118/quality-batches-full/report.json`.
The four-pixel result and source changes are in `quality-four-bound/pair.json` and
`four-pixel.patch` under the same evidence root. Resume with the full four-pixel quality
suite and CUDA comparisons, and verify memory behavior with reflections enabled before
selecting a default density. Timing qualification remains deferred.

### Phase 2 path-independent settling and detailed-geometry history (2026-11-18)

**Phase 2 remains open.** Two persistent states made the settled native image depend on the
camera and lighting history, not only on the pose and current lighting. Both are fixed. A third
change gives fresh probes on detailed geometry a short history. Static cross-launch error falls
from 0.414 to 0.213 RMS (limit 0.178); motion error falls by 28–79% across the nine poses,
most for dolly. All nine motion gates, static cross-launch and light recovery still exceed the
retained limits; flicker, sparks, every CUDA energy check and the diffuse time budgets pass.

**Diagnosis.** In the light-grid build's quality run, the static cross-launch comparison had
more pixels above 4/255 (0.209%) than most motion poses. Most of them formed one broad region,
not noise: the top-right vault of the base view. A cold start at the base pose settled that vault
at a mean of 7.7/255 by frame 256 and held it through frame 6,144. Any earlier history event
settled it at 6.4 instead, for thousands of frames:

- moving light 3 by 0.04 and restoring it (the quality runner's light step);
- a cold start at the pan-slow/8 pose followed by 2,048 base-pose frames.

The second case alone differs from the base-pose cold start by RMS 0.399 and 0.182% of pixels
above 4/255, the whole static failure. The probe bounce view (vault 14.5 versus 12.4) and the
world-cache view showed the same shift. Ignoring the cache's light-signature reset entirely
left the 6.4 state unchanged, so the reset was not the cause.

- **World cache.** A cell's update origin, geometric normal and normal-mapped shading normal were
  fixed by whichever receiver first claimed it. A coarse cell across a sun/shadow edge or a
  normal-map bump stayed lit or shadowed for its lifetime, and claim order depends on camera
  history. Compatible claims now move the origin and shading normal; the first geometric normal
  stays for compatibility tests. Cold starts from the two poses then settle to the same vault
  (6.46 and 6.45) and differ by RMS 0.221 and 0.018% above 4/255.
- **Screen-probe placement.** A probe keeps its world anchor while it stays within 0.75 tile of
  its tile center. That is what lets tiles inherit full history during motion, but it also left
  probes at drifted offsets indefinitely after motion stopped, while a held reference has them at
  tile centers. Edges then interpolated different probes. A coplanar probe now returns to its
  tile center once the view is still relative to its anchor, keeping up to 256 frames of history.
- **Detailed geometry.** After those two fixes, a debug capture of gathered probe age along the
  quality timeline showed that probes aged 1–4 frames carried 50–55% of the remaining motion
  errors, at a 2–3% error rate per pixel, against 0.01% for mature probes. They lie mainly on
  ornament such as column capitals. There the sample at a tile center lands on a different facet
  each frame and fails the coplanar history test (normal within 11 degrees, plane within 4%).
  Such a probe now borrows the nearest visible probe of the same surface within 45 degrees for
  up to 32 frames, with irradiance moved to the new normal by the stored L1 gradient.

Each row below is one native quality launch: 4,096-frame warm-up, 1,024-frame held references,
diffuse-only setup, HW tracing, async compute off, 3840×2160 fullscreen, compared with the
frozen retained baseline. Motion columns give the percentage of pixels above 4/255 at steps
8 / 32 / 64. The final row is the validated launch of the selected build; it differs from the
unvalidated E13 launch of the same shaders by at most 0.02 percentage points of motion error.

| Build | Static RMS | Light/128 RMS | Slow pan | Fast pan | Dolly |
|---|---:|---:|---:|---:|---:|
| Light-grid build (start) | 0.414 | 0.395 | 0.132 / 0.117 / 0.124 | 0.144 / 0.187 / 0.112 | 0.248 / 0.226 / 0.168 |
| E8: moving cache origins | 0.246 | 0.408 | 0.106 / 0.110 / 0.108 | 0.133 / 0.184 / 0.103 | 0.107 / 0.130 / 0.129 |
| E9: grid placement every frame, rejected | 0.206 | 0.357 | 0.124 / 0.313 / 0.341 | 0.137 / 0.182 / 0.126 | 0.114 / 0.175 / 0.194 |
| E10: E8 plus settled recentring | 0.206 | 0.353 | 0.113 / 0.105 / 0.111 | 0.129 / 0.172 / 0.107 | 0.094 / 0.118 / 0.111 |
| E11: adaptive-slot hysteresis, rejected | 0.239 | 0.421 | 0.117 / 0.124 / 0.113 | 0.134 / 0.182 / 0.104 | 0.101 / 0.127 / 0.131 |
| E13: E10 plus surface borrowing | 0.209 | 0.350 | 0.046 / 0.062 / 0.069 | 0.081 / 0.155 / 0.074 | 0.049 / 0.054 / 0.065 |
| E14: longer borrowed histories, rejected | 0.216 | 0.385 | 0.041 / 0.068 / 0.068 | 0.073 / 0.150 / 0.069 | 0.055 / 0.067 / 0.074 |
| E15: gather hole fallback, rejected | 0.212 | 0.374 | 0.043 / 0.058 / 0.070 | 0.080 / 0.145 / 0.073 | 0.053 / 0.061 / 0.069 |
| **Selected (E13), validated launch** | **0.213** | **0.352** | **0.044 / 0.057 / 0.072** | **0.072 / 0.135 / 0.074** | **0.052 / 0.056 / 0.064** |
| Retained limit | 0.178 | 0.230 | 0.009 / 0.011 / 0.015 | 0.012 / 0.037 / 0.028 | 0.009 / 0.015 / 0.019 |

E9 confirmed the placement diagnosis but re-anchored on every frame. It discarded the inherited
histories that keep slow pans clean, so slow-pan error nearly tripled. E11 held adaptive probes
on their previous holes; that made settled placement history-dependent again. E14 used 256-frame
coplanar and 64-frame relaxed borrows. E15 searched a 4x4-tile neighborhood for similar probes
when the bilinear footprint had none. Neither E14 nor E15 improved the selected build beyond
launch-to-launch variation, and both were reverted. Static flicker (mean / p99.9 standard
deviation 0.0042 / 0.496) and sparks (0 / 0) pass in every row.

The remaining errors have different causes:

- **Motion.** In E13's age capture, ages 32–64 hold 20–64% of motion errors at a 0.34–0.77%
  rate. These are borrowed histories, so their remaining error is short-window noise and
  borrowing bias rather than missing history. Pixels with no supporting probe hold 4–12%. The
  pan-fast/32 pose improved least (28%).
- **Light recovery.** RMS at frame 128 (0.352) is 1.65 times the static floor; the curve is 0.944,
  0.700, 0.593, 0.498 and 0.352 at frames 1, 8, 16, 64 and 128. Both probe and world-cache
  histories restart on the GPU light signature. At frame 128 a probe averages roughly 80 frames,
  and a cache cell is still in its 16-update window until it has 256 samples since the restart.
- **Static cross-launch.** At 0.213 RMS, only 0.008% of pixels differ by more than 4/255 and
  none by more than 16/255. A single E8 launch drifted by 0.157 RMS between frames 2,112 and
  3,136 at a fixed pose, so most of the remaining difference is slow residual noise.

Both independent launches pass every CUDA check on the selected shaders, using the unchanged
uniform fixture, 4,096 frames and 3,830 matched receivers. Both cache-only launches pass. On
the light-grid build, one of the two missed.

| CUDA comparison | Launch A | Launch B | Limit |
|---|---:|---:|---:|
| Probe one bounce, mean bias / RMS | +0.471% / 0.0022224 | +0.420% / 0.0022238 | ±1.971% / 0.0022261 |
| Probe 16 vertices, mean bias / RMS | −0.397% / 0.0036955 | −0.276% / 0.0036961 | ±1.491% / 0.0074176 |
| Cache only, 16 vertices, mean bias | +0.107% | −0.274% | ±0.659% |

One-bounce RMS still passes by less than 0.2%. Cache-only spatial RMS is 0.0117 / 0.0118. E10,
which has the same cache change, also passed all six (cache-only −0.205% / +0.467%).

| Median GPU cost, ms | Static | Pan | Light-grid static / pan | Limit |
|---|---:|---:|---:|---:|
| Placement, adaptive selection, proposal and trace | 2.722 | 2.653 | 2.648 / 2.607 | 3.0 |
| Temporal and both spatial filter passes | 0.620 | 0.720 | 0.603 / 0.725 | 0.8 |
| Gather including rough specular | 1.229 | 1.231 | 1.229 / 1.231 | 1.5 |
| Total diffuse | 4.572 | 4.607 | 4.479 / 4.563 | 5.3 |
| World-cache update and resolve | 3.236 | 3.257 | 3.186 / 3.203 | 1.5 in Phase 1 |
| Whole frame | 18.611 | 18.662 | 18.466 / 18.684 | 16.7 overall |

Nsight GPU Trace captured 30 frames after 60 seconds at base clocks on the RTX 5080. ZenApp
ran fullscreen at 3840×2160 with HW tracing and async compute off. Whole-frame p95 was 18.765 /
18.999 ms. Device-wide memory peaked at 12,794 MiB in both captures, including other users of
the GPU. The extra visibility tests in placement add about 0.07 ms in the static capture; every
diffuse component remains within budget. Cache cost and the whole frame remain over target, as
before.

All 28 targeted native/cache GPU tests pass (`RayQueryIntegrationTest`, filter
`*RadianceCache*:*WorldCache*:*Native*:*HybridGIFramePassLists*`). The changed shaders pass
clang-format. The canonical ZenApp SHA-256 is unchanged at
`6b6b39eb0329e2881691bafb6e8bee565e92e7f991a3ddf8b7c114a5f3d0dfd6`; the shader-set SHA-256 is
`ff7003de483e5e7b3b55a78ea6c86cb259bf1ca32687b77f8782380f496a9f6b`. Reports and
`qualification.json` are in `build/checks/native-gi/p2-anchor-20261118/`; its raw captures were
deleted afterwards (see Cleanup).

**Next steps.** Light recovery and the residual motion error both come from short-window probe
noise. Candidates, in order:

1. On a light-signature change, keep the cache's and probes' previous values as a prior with a
   short window instead of restarting them.
2. Add rays only for borrowed and fresh probes, without taking any from mature probes; the
   earlier fixed-budget reallocation that starved mature probes was rejected.
3. Add a lightweight per-pixel temporal accumulation (8 bytes per pixel) after the gather. Its
   motion error and cost must be measured against this build before it can count as a fix.

### Phase 2 qualification with finer world-cache cells near lights (2026-11-18, preceding checkpoint)

**Phase 2 remains open.** The 8-pixel, 16-ray screen probes pass both CUDA probe references in both repeats, all
diffuse timing budgets, both stationary flicker checks and both spark checks. All nine motion comparisons,
independent static convergence and light recovery at frame 128 exceed their retained limits. One of the two
cache-only repeats misses its energy limit.

Point and spot lights now limit the cache footprint when a hit is within eight cell widths. Both HW and Compute
paths use the same level for inserting and querying samples. The cache still follows the camera-selected level
farther from lights. A paired slow-pan capture reduced the red-channel difference on the ceiling next to a lamp
from 5.6 to 0.1 levels. Full-scene slow-pan error improved at all three steps. Dolly error and the independent
static comparison regressed relative to the preceding 8-pixel build.

| Median GPU cost, ms | Static | Pan | Limit, ms |
|---|---:|---:|---:|
| Placement, adaptive selection, proposal and trace | 2.648 | 2.607 | 3.0 |
| Temporal and both spatial filter passes | 0.603 | 0.725 | 0.8 |
| Gather including rough specular | 1.229 | 1.231 | 1.5 |
| Total diffuse | 4.479 | 4.563 | 5.3 |
| World-cache update and resolve | 3.186 | 3.203 | 1.5 in Phase 1 |
| Whole frame | 18.466 | 18.684 | 16.7 overall |

Nsight captured 30 frames after 60 seconds at base clocks on the RTX 5080. ZenApp ran fullscreen at 3840×2160,
with hardware ray tracing and async compute disabled. Total device memory, including other users of the GPU,
peaked at 12,461 MiB static and 12,407 MiB panning.

| Quality gate | Retained limit | Current |
|---|---:|---:|
| Slow pan, % above four levels at steps 8 / 32 / 64 | 0.00907 / 0.01137 / 0.01501 | 0.13197 / 0.11669 / 0.12365 |
| Fast pan, % above four levels at steps 8 / 32 / 64 | 0.01208 / 0.03674 / 0.02844 | 0.14389 / 0.18696 / 0.11222 |
| Dolly, % above four levels at steps 8 / 32 / 64 | 0.00898 / 0.01542 / 0.01922 | 0.24767 / 0.22598 / 0.16833 |
| Independent static launch, RMS | 0.178231 | 0.414224 |
| Light recovery at frame 128, RMS | 0.229642 | 0.395214 |
| Stationary flicker, mean standard deviation | 0.0115237 | 0.00516789 |
| Stationary flicker, p99.9 standard deviation | 0.5 | 0.499022 |
| Stationary spark count | 0 | 0 |
| Moving spark count | 2 | 0 |

The native quality suite used 4,096 warm-up frames and 1,024-frame held references per pose. The retained
comparison used 1080p GI at 4K output. Each renderer used its own settled references. The independent CUDA
captures used 4K GI and 3,830 matched receivers per run.

| CUDA comparison | RMS A / B | RMS limit | Mean energy bias A / B | Bias limit |
|---|---:|---:|---:|---:|
| Probe one bounce | 0.00222390 / 0.00222553 | 0.00222612 | +0.330 / +0.310% | ±1.971% |
| Probe 16 vertices | 0.00369909 / 0.00370177 | 0.00741765 | -0.298 / -0.063% | ±1.491% |
| Cache only, 16 vertices | 0.01082595 / 0.01110635 | — | -0.884 / -0.513% | ±0.659% |

All 57 targeted tests passed, including GPU checks of near-light refinement and the distant camera level on both
cache-key widths. The canonical executable, shader hashes, CUDA comparisons, GPU traces and per-gate results are
in `build/checks/native-gi/p2-20261118/light-grid-qualification.json`.

### Phase 2 experiment under consideration — probe candidates for per-pixel ReSTIR GI

The current native frame graph dispatches screen probes. The retained pixel path can dispatch
`ReSTIRTemporal`, `ReSTIRSpatialDiffuse0` and `ReSTIRSpatialDiffuse1`. A setup can have both
`screenProbeGI` and `restirGI` enabled while only the probe path runs. Changing the ReSTIR
setting also invalidates GI history, which matters when comparing an interactive toggle.

A hybrid could select a diffuse path sample from the rays already traced at a compatible probe.
Preserve the secondary surface position and normal, its outgoing radiance and the sampling probability
before probe integration discards the hit geometry. At the pixel, reevaluate the target, account
for the change in sampling density when reconnecting to the hit, and test visibility. A freshly
disoccluded pixel also needs a source of valid candidates when nearby probes cannot see its
surface. Use the reservoir estimate once for diffuse bounce. Sky and reflections can continue
through their existing paths. [RTXDI ReSTIR GI integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirGI.md)

Start with one temporal update and one spatial resampling iteration. A single spatial dispatch
can read an immutable temporal result and write a separate destination. The retained implementation
uses two checkerboard dispatches to allow in-place updates. One diffuse reservoir image at 4K
occupies 189.8 MiB at the engine's current 24 bytes per pixel. Compare probe-only, retained
ReSTIR and the hybrid using the same warm-up, motion, light changes and CUDA references. Measure
residual noise before enabling a separate diffuse denoiser. Qualification still uses the 5.3 ms
4K diffuse limit.

The same ZenApp executable and shaders measured the retained diffuse ReSTIR stages under
continuous pan, with the world cache enabled, reflection reservoirs disabled and 4K output.
Each median covers 30 frames at base clocks after 60 seconds of warm-up. Candidate tracing
and subsequent image denoising fall outside these timings.

| Reservoir resolution | Temporal, ms | Spatial checkerboard halves, ms | Combined median, ms |
|---|---:|---:|---:|
| 3840×2160 | 3.911 | 4.900 / 4.932 | 13.738 |
| 1920×1080 | 0.984 | 1.311 / 1.316 | 3.611 |

The native diffuse path has 0.737 ms of panning headroom. For a first hybrid, restrict per-pixel
reservoirs to disocclusions and receivers with weak probe support. Measure the cost of selecting
those pixels, candidate generation, reconnection, visibility and final shading together. The
two timings above measure the retained implementation. A working hybrid still needs its own
quality and cost comparison. Capture manifests are under `restir-diffuse-2160p-timing-pan/`
and `restir-diffuse-1080p-timing-pan/` in the same evidence directory.

### Phase 2 guided screen probes (2026-11-18, preceding checkpoint)

**Phase 2 remains open.** The 8-pixel candidate passes the static and continuous-pan diffuse time
budgets, both flicker gates and both spark gates. All nine motion comparisons, the cross-launch
static comparison and light recovery at frame 128 exceed the retained thresholds.

| Median GPU cost, ms | Static | Pan | Limit, ms |
|---|---:|---:|---:|
| Placement, adaptive probes, proposal and trace | 2.508 | 2.452 | 3.0 |
| Temporal and both spatial passes | 0.602 | 0.706 | 0.8 |
| Full-resolution gather including rough specular | 1.227 | 1.229 | 1.5 |
| Diffuse total | 4.336 | 4.387 | 5.3 |
| Diffuse plus the reflection-only GGX prefilter | 4.842 | 4.908 |  |
| World-cache update and resolve | 2.947 | 2.947 | 1.5 in Phase 1 |
| Whole frame | 18.170 | 17.783 | 16.7 overall |

Nsight Graphics captured 30 frames on the RTX 5080 at base clocks after 60 seconds of warm-up.
ZenApp ran fullscreen at 3840×2160 with native 4K GI, HW RT and async compute disabled.
Removing the two 64-byte-per-pixel diffuse histories saved 1,012.5 MiB at 4K. The horizontal
filter adds a 19.8 MiB transient buffer. Total device memory peaked at 12,398 / 12,420 MiB during
the two captures, including other device users.

| Full quality gate | Retained | Guided |
|---|---:|---:|
| Static mean / p99.9 standard deviation, 8-bit levels | 0.011524 / 0.500000 | 0.003622 / 0.496078 |
| Static / moving spark pixels per frame | 0 / 2 | 0 / 0 |
| Slow pan, pixels differing by more than 4 levels, % at steps 8 / 32 / 64 | 0.00907 / 0.01137 / 0.01501 | 0.21859 / 0.24098 / 0.28338 |
| Fast pan, same metric | 0.01208 / 0.03674 / 0.02844 | 0.24744 / 0.18199 / 0.09725 |
| Dolly, same metric | 0.00898 / 0.01542 / 0.01922 | 0.14031 / 0.15682 / 0.14843 |
| Independent static launch, RMS in 8-bit levels | 0.178231 | 0.333429 |
| Moved light at frame 128, RMS in 8-bit levels | 0.229642 | 0.418681 |

The diffuse-only quality run used 4,096 warm-up frames and 1,024-frame reference holds. Each renderer
was compared with its own held poses. Native GI ran at 4K and retained GI at 1080p with 4K output.
Fast-pan endpoint error was 2.509% at the November 17 checkpoint.

The independent CUDA fixture matched 3,830 receivers in each of six 4,096-frame launches.
One-bounce probe mean energy passed twice (−0.548% / −0.560%). RMS was 0.00223011 / 0.00222765
against 0.00222612. Sixteen-vertex probe mean energy (−0.914% / −0.994%) and RMS
(0.00373245 / 0.00374372) passed twice. Cache-only energy passed once (+0.018%) and missed once
(−1.200%) against ±0.659%. One-bounce and cache-only accuracy therefore remain open on this build.

The equal-ray-budget density experiment used 16-pixel spacing with 64 rays or 8-pixel spacing with
16 rays. In the short fast-pan diagnostic the two endpoint errors were 1.112% and 0.108%.
The 16-pixel run also missed the one-bounce CUDA RMS limit at 0.00245818. The selected density
is 8 pixels. The production candidate then increased local-light samples from eight to twelve.
Twenty-eight GPU tests, seven RenderCore tests and 22 runtime tests passed.

The evidence directory is `build/checks/native-gi/p2-20261118/`. `guided-qualification.json`
links the six CUDA runs, full quality suite and both timing traces and checks their executable
and shader hashes. The `pan-*/report.json` files are shorter diagnostics with different camera
histories from the full quality suite.

### Phase 2 RTXGI-inspired recovery work (2026-11-17, preceding checkpoint)

**Phase 2 remains incomplete.** The transport fallback, final diffuse reconstruction and ray-allocation
experiments have been implemented and measured. The selected candidate uses the original 430,080
primary cache updates and fixed 16 rays per screen probe. It is an unqualified candidate, not a
claim that the quality gap has closed. Evidence is under
`build/checks/native-gi/p2-recovery-20261117/`; `qualification.json` checks that the six energy
captures, full quality suite and both timing captures share the final executable and shader set.
All six mean-energy checks and all four probe RMS checks pass on this candidate. Motion,
light recovery and cost must be assessed independently; passing energy does not complete P2.

The implementation distinguishes an exact cache hit from a usable neighboring sample; sampled
black remains usable. Unresolved HW and Compute transport evaluates local light/emission and
continues a bounded diffuse path, up to 16 hits, ending at a usable cache sample or the sky.
Feedback-disabled mode retains its one-bounce endpoint. Short segments below the cache-cell
diagonal are rejected for feedback updates to avoid local self-feedback. Display queries retain
usable cache samples. After two unresolved hits, Russian roulette terminates weak paths with
inverse-probability weighting; this preserves the bounded estimator's expectation. Nested shadow
rays require RT recursion depth two, while diffuse continuation stays iterative. No SDK code or
dependency was added; the pinned RTXGI/SHaRC references below informed the independent implementation.

Full-resolution diffuse history is reprojected using motion, surface identity, plane and normal
checks, and lighting-source signatures. It is fused into the gather and does not feed the probe
estimator, sampling proposal or cache. Two 64-byte-per-pixel buffers add **1,012.5 MiB at 4K**.
History is capped at 32 frames, or four during source/geometry recovery. This explicitly revisits
the original plan's assumptions of cache-only hit shading and no final per-pixel diffuse history.

Experiments rejected during this work:

- Half-resolution final history increased motion and light-change error.
- Moving a fixed ray budget from mature probes to fresh probes worsened all nine motion comparisons;
  the live code uses 16 primary rays per probe again.
- Applying the short-segment restriction to display queries as well as feedback updates cost
  30.042 ms per static frame before roulette, with 11.410 ms in cache update/resolve. Roulette
  reduced those to 20.475 / 3.346 ms; the selected candidate restricts only feedback updates.
- Halving primary cache updates to 215,040 reduced static/pan frames to 17.828 / 18.130 ms but
  worsened all nine motion comparisons, light recovery, and both one-bounce RMS repeats. The
  original update budget and 256-sample warm-up threshold were restored. `update-budget-*`
  captures and `reduced-budget-source/` preserve this rejected trial.

The final quality suite uses the unchanged diffuse-only setup, HW tracing, async compute off,
3840x2160 fullscreen output, 4,096 warm-up frames and 1,024-frame reference holds. Native GI is
4K; the frozen retained GI is 1080p. Each renderer is compared with its own held images, which
measure recovery and noise rather than independent transport truth.
Both the preceding and current native quality runs retain 4,194,304 cache cells.

| Quality measurement | Retained | Preceding native | Current candidate |
|---|---:|---:|---:|
| Static mean standard deviation, /255 | 0.011524 | 0.009138 | 0.004380 |
| Static p99.9 standard deviation, /255 | 0.500000 | 0.499022 | 0.499022 |
| Static / moving spark pixels per frame | 0 / 2 | 0 / 0 | 0 / 0 |
| Slow pan, % above 4/255 at steps 8 / 32 / 64 | 0.009 / 0.011 / 0.015 | 0.156 / 0.517 / 0.753 | 0.515 / 1.082 / 1.298 |
| Fast pan, same metric | 0.012 / 0.037 / 0.028 | 1.060 / 2.375 / 2.517 | 1.556 / 2.683 / 2.509 |
| Dolly, same metric | 0.009 / 0.015 / 0.019 | 0.270 / 0.274 / 0.292 | 0.565 / 0.561 / 0.623 |
| Light move after 128 frames, RMS /255 | 0.229642 | 0.593601 | 0.802617 |
| Independent static launch versus held image, RMS /255 | 0.178231 | 0.391063 | 0.631121 |

All nine motion comparisons, light recovery at frame 128 and cross-launch static error fail.
`restored-budget-motion-comparison.png` shows the fast-pan
endpoint and each candidate's held image; display values are multiplied by three and differences
by twelve. Numerical improvement in one metric must not be counted as passing the remaining gates.
Compared with the preceding native build, eight of the nine motion measurements worsen. Light
recovery RMS rises from 0.594 to 0.803, and
cross-launch static RMS rises from 0.391 to 0.631.
The roughly halved static flicker therefore does not establish an overall visual improvement.
The candidate must not be promoted as the P2 quality fix. The next controlled comparison should
separate fallback-feedback variance from final-history lag using a warmed, fixed cache, before
adding more temporal accumulation or changing ray allocation again.

Two independent 4,096-frame launches per transport/depth use the unchanged CUDA fixture, with
3,830 matched receivers. Probe mean tolerances remain +/-1.9714% for one bounce and +/-1.4910%
for sixteen vertices; the cache-only tolerance remains +/-0.659162%. Retained probe RMS limits
remain 0.0022261231 / 0.0074176499. Results are not cherry-picked from the earlier experiments.

| CUDA comparison | Launch A | Launch B | A / B |
|---|---:|---:|---|
| One-bounce RMS | 0.0022091 | 0.0022066 | pass / pass |
| One-bounce mean energy bias, % | -0.1436 | 0.2649 | pass / pass |
| Sixteen-vertex RMS | 0.0038579 | 0.0037374 | pass / pass |
| Sixteen-vertex mean energy bias, % | 0.6704 | -0.5025 | pass / pass |
| Cache-only mean energy bias, % | 0.5536 | 0.4894 | pass / pass |

Cache-only spatial RMS is 0.0161359 / 0.0183978; passing its mean-energy gate
does not establish low spatial error or visual convergence.

These energy runs render at 3840x2160 with a 1280x720 windowed framebuffer. Probe mode reports
4K GI; cache-only mode reports 1080p GI. The reports retain actual state, commands and input
hashes. Exact hit/miss counters do not measure fallback usage. These finite repeats do not
establish that all future launches pass. Cache-only mean energy passes twice here; the cache
cost and full-scene recovery limitations still prevent treating Phase 1 as requalified in full.

Nsight GPU Trace captures use the full timing setup including reflections and shadow reuse,
30 frames after a 60-second warm-up, RTX 5080 base clocks, HW tracing, 4K fullscreen and async
compute off. All captures use the canonical `build/x64-windows-msvc-release/bin/ZenApp.exe`.

| Cost, ms median | Static | Continuous pan |
|---|---:|---:|
| Placement, proposal, trace and guidance prefilter | 3.137 | 3.021 |
| Diffuse temporal and spatial filter | 0.592 | 1.264 |
| Gather including final diffuse reconstruction | 1.918 | 1.993 |
| Diffuse total including guidance dependency | 5.650 | 6.276 |
| Cache update and resolve | 3.329 | 3.341 |
| Whole frame | 19.377 | 20.192 |

The preceding frame medians were 16.401 / 16.732 ms, and diffuse totals were 4.639 / 5.148 ms.
The user's acceptance of those earlier small deviations is not blanket acceptance of these
new costs. Current whole-frame p95 is 19.486 /
20.422 ms. Device-wide memory peaks at
13,614 / 13,627 MiB
of 16,303 MiB. `qualification.json` retains all component and total budget comparisons.

The canonical executable SHA-256 is
`eff21dbc81436131f276b566a55952a2a3eafee51dbb39196a8426462a39c8be`;
the shader-set SHA-256 is
`0bbd50fec4a176c12f6020e12628e5b5cd7626813a65dc68fef591551475adbc`.
ZenApp/editor builds, formatting, 28 targeted native/cache GPU tests, six render-state tests and
22 launch tests pass. GPU regression coverage includes usable neighboring samples and valid black
cache cells. Compute functionality is covered by those tests; 4K Compute quality/performance is
not qualified. The source snapshots and incremental patch preserve the actual change against the
pre-task workspace. Lossless compression of 643 finished capture files
reclaimed 90.66 GiB without deleting evidence.

### Phase 2 quality follow-up (2026-11-17, preceding checkpoint)

**Phase 2 remains incomplete.** The user accepts the preceding small frame-time deviations and
prioritizes quality. Motion quality remains the main visible failure. The new filter improves all
nine motion comparisons, but none reaches the retained baseline. Both repeated sixteen-vertex
energy captures also miss the unchanged tolerance. Evidence is under
`build/checks/native-gi/quality-focus-20261117/`; `qualification.json` records the results and limits.

The retained change is confined to `screen_probe_filter.comp`: use a broader footprint for young
histories and continuously fade both its outer rings and lighting-edge rejection. At 1,024 frames
and older, the radius and rejection match the previous filter. This output does not feed temporal
accumulation or the sampling proposal. A shorter sampling-block experiment did not improve recovery
and was reverted; its source files were verified byte-for-byte against the preceding build.

A separate camera-cut control first holds the fast-pan endpoint for 4,096 frames, turns away for
eight frames to discard screen histories, then returns while retaining the world cache. Before the
filter change, 2.190% of pixels still differed by more than 4/255 after 64 recovery frames, dropping
to 0.045% after 512. The last 1,024 frames of the preceding hold differed at only 0.000530% of pixels.
This reproduces slow probe recovery with an already-warmed cache; it does not assign every motion
error to a single cause. `reset-feedback/report.json` retains this control and its actual state.

The full quality comparison keeps the preceding diffuse-only setup, async compute off, 4K
fullscreen output, 4,096-frame warm-up and 1,024-frame reference holds. Native GI is 3840x2160;
the frozen retained GI baseline is 1920x1080. Each renderer is compared with its own held images,
which remain diagnostics rather than independent transport truth.

| Quality measurement | Retained | Previous native | Current native |
|---|---:|---:|---:|
| Static mean standard deviation, /255 | 0.011524 | 0.009151 | 0.009138; pass |
| Static p99.9 standard deviation, /255 | 0.500000 | 0.499022 | 0.499022; pass |
| Static / moving spark pixels per frame | 0 / 2 | 0 / 0 | 0 / 0; pass |
| Slow pan, % above 4/255 at steps 8 / 32 / 64 | 0.009 / 0.011 / 0.015 | 0.314 / 1.420 / 2.865 | 0.156 / 0.517 / 0.753; fail |
| Fast pan, same metric | 0.012 / 0.037 / 0.028 | 1.694 / 6.092 / 5.991 | 1.060 / 2.375 / 2.517; fail |
| Dolly, same metric | 0.009 / 0.015 / 0.019 | 0.653 / 0.635 / 0.680 | 0.270 / 0.274 / 0.292; fail |
| Light move after 128 frames, RMS /255 | 0.230 | 0.801 | 0.594; remains worse |
| Independent static launch versus held image, RMS /255 | 0.178 | 0.457 | 0.391; remains worse |

The slow-pan, fast-pan and dolly endpoint errors fall by 73.7%, 58.0% and 57.1%, respectively.
Inspection still shows broad noisy patches on newly exposed walls. `motion-comparison.png` shows
the fast-pan endpoint before and after; display values are multiplied by three and absolute
differences by twelve to expose the dark-region artifacts. The improvement does not qualify motion.

Two independent 4,096-frame launches per CUDA depth use the unchanged uniform-material fixture
and 3,830 matched receivers. All four captures share the current executable and shader hashes.

| CUDA comparison | Launch A | Launch B | Gate |
|---|---:|---:|---|
| One-bounce RMS | 0.00220418 | 0.00219929 | Both pass retained 0.00222612 |
| One-bounce mean energy bias | -0.1213% | +0.0394% | Both pass +/-1.9714% |
| Sixteen-vertex RMS | 0.00380300 | 0.00373935 | Both pass retained 0.00741765 |
| Sixteen-vertex mean energy bias | -1.6059% | -1.7057% | Both fail +/-1.4910% |

These are repeated observations, not proof that all future launches meet the narrow one-bounce
RMS margin. The unchanged cache-only path also retains its earlier launch-to-launch energy failure
below; the filter change does not restore Phase 1 qualification.

Base-clock Nsight captures use the same full native timing setup as the preceding checkpoint,
including reflections and shadow reuse, with 30 captured frames, 4K fullscreen and async compute off.

| Current cost, ms median | Static | Continuous pan |
|---|---:|---:|
| Placement, proposal and trace including guidance prefilter | 2.824 | 2.646 |
| Diffuse temporal and spatial filter | 0.591 | 1.281 |
| Gather | 1.225 | 1.224 |
| Diffuse total including its prefilter dependency | 4.639 | 5.148 |
| Cache update and resolve | 1.523 | 1.506 |
| Whole frame | 16.401 | 16.732 |

Diffuse total remains within 5.3 ms. The larger young-probe filter exceeds its original 0.8 ms
component allocation while panning; the user's acceptance of earlier small timing deviations is
not recorded as blanket approval of arbitrary future regressions. Whole-frame p95 is 16.533 /
16.916 ms. Device-wide memory peaks at 12,616 / 12,620 MiB of 16,303 MiB under Nsight.

The canonical executable SHA-256 remains
`1b05058ac030e1e0432295179bec0e34fb38e4f2afed8ec4f16ed886648f6eb7`;
the current shader-set SHA-256 is
`5d540295fb6d0c2dae0165a2806fa27ac43e4a71c481138e40681b9428e13257`.
ZenApp and editor builds pass, the changed shader passes formatting, and all 28 native/cache GPU
tests pass. `current-build.json` verifies the capture identity; the quality and energy reports keep
exact commands, setups and actual state. Lossless compression of 171 new capture files reclaimed
15.38 GiB, retaining all evidence at its original paths (`compression.json`).

The remaining quality work is to accelerate fresh-probe recovery without merely spreading noisy
lighting over a larger area, close light-change error, and stabilize the cache/multi-bounce energy
checks. The measurements above establish the current gap; those fixes are not yet implemented.

### RTXGI reference review (2026-11-17, before recovery work)

This review describes the implementation before the recovery work above. The first two proposals
have now been implemented and tested; their measured benefits and regressions are recorded above.

Reviewed NVIDIA RTXGI v2.9.0 at commit `04372d7f91bb3a0444f9fd95d3f34381ea4e3b39`, including
the indirect-diffuse sample and its pinned SHaRC v1.8.3 commit
`4e21b585c33c83d723ca9a1e11bbb1090d145793`. The existing `E:/Dev/RTXGI` checkout has SHaRC v1.6.5
checked out instead; it was also inspected, but was not treated as the parent's pinned version.
Neither that checkout nor its submodule revisions was changed. Source hashes and experiment
evidence are under `build/checks/native-gi/rtxgi-followup-20261117/`.

RTXGI provides two relevant, distinct references: current SHaRC caches outgoing radiance for traced
paths; the separate older DDGI SDK maintains world-space irradiance probes. Neither is the same
algorithm as Zen's screen-probe gather. See the [RTXGI overview](https://github.com/NVIDIA-RTX/RTXGI)
and [DDGI algorithms](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Algorithms.md).

The comparison identifies these next experiments, in priority order:

1. **Continue transport when a cache query cannot supply lighting.** RTXGI's query sample evaluates
   local lighting and continues the path after an unsuccessful or ineligible lookup. Its cache
   acceptance also depends on segment length and the launching lobe's footprint. Zen's active
   `WorldLookup` path can return zero when neither the exact nor neighboring cell is usable;
   `HardwareHitRadiance` requests initialization but still terminates that ray. This is a confirmed
   missing-data approximation, not proof that it explains the entire measured energy deficit.
   The next controlled experiment should use a bounded fallback path, preserve the one-bounce
   contract when feedback is disabled, and measure both reference energy and cold-view ray cost.
   [RTXGI query implementation](https://github.com/NVIDIA-RTX/RTXGI/blob/04372d7f91bb3a0444f9fd95d3f34381ea4e3b39/Samples/IndirectDiffuse/Shaders/SharcIndirectDiffuse.hlsli).

2. **Reconstruct the noisy query signal explicitly.** RTXGI's indirect-diffuse sample can run
   `REBLUR_DIFFUSE` after its cache queries, with temporal reprojection and surface guides. A
   radiance cache alone is therefore not evidence that final gather denoising can be omitted.
   Zen currently filters probe estimates, then gathers directly into transient output textures;
   it has no final diffuse pixel history. Prototype a small reconstruction pass or better
   allocation of rays to fresh probes, then compare disocclusion error, lag and total cost.
   This revisits the plan's assumption that probe filtering alone can replace diffuse denoising;
   it does not authorize counting the existing image-quality failures as passes.
   [RTXGI denoiser integration](https://github.com/NVIDIA-RTX/RTXGI/blob/04372d7f91bb3a0444f9fd95d3f34381ea4e3b39/Samples/IndirectDiffuse/Pathtracer.cpp).

3. **Carry compatible history across cache levels, with qualification.** SHaRC's resolve combines
   samples using accumulated sample counts and can reuse adjacent-level history during camera
   movement. Its default query accepts any positive accumulated sample count; this does not
   guarantee a well-converged estimate. Zen instead weights cache update batches by age and only borrows
   neighboring cells at the current level. Sample-weighted initialization and level transitions
   deserve isolated checks, but a simple level lookup fallback was rejected below.
   [Pinned SHaRC resolve and query code](https://github.com/NVIDIA-RTX/SHARC/blob/4e21b585c33c83d723ca9a1e11bbb1090d145793/include/SharcCommon.h).

DDGI also supplies directional distance moments for visibility-aware interpolation. That is useful
for a confirmed leak at a discontinuity, but does not explain or cure all current diffuse blotches.
Its cosine-weight normalization, nonlinear irradiance encoding and brightness limiting are not
direct substitutes for Zen's importance-sampled estimator. Any trial must retain the unchanged
CUDA energy checks. [DDGI interpolation](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/Irradiance.hlsl),
[probe blending](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/rtxgi-sdk/shaders/ddgi/ProbeBlendingCS.hlsl),
[estimator math](https://github.com/NVIDIAGameWorks/RTXGI-DDGI/blob/main/docs/Math.md).

**Tested and rejected:** an independently written, read-only fallback checked the immediately finer
and coarser cache levels after same-level lookups failed. It retained exact-cell initialization
requests, used the finer plane tolerance, rejected opposite-facing surfaces, and never replaced a
valid black exact cell. All 28 GPU tests passed, including additional calibration checks for both
level directions, both key widths and all four submission modes. The unchanged full Sponza quality
protocol nevertheless measured:

| Endpoint error, % above 4/255 | Existing filter | Level-reuse trial |
|---|---:|---:|
| Slow pan | 0.753 | 1.108 |
| Fast pan | 2.517 | 2.469 |
| Dolly | 0.292 | 0.498 |

Static cross-launch RMS rose from 0.391 to 0.471; light-recovery RMS after 128 frames rose from
0.594 to 0.635. Flicker and sparks still passed. These captures do not isolate why every pixel
changed, but they provide no basis to retain the trial as a quality improvement. All prototype
source and calibration changes were reverted; the rejected patch and captures remain in the
evidence directory. At that checkpoint, the preceding quality-follow-up build remained active; the recovery work above supersedes it.
No RTXGI SDK code or dependency was added. Phase 2 remains incomplete.

### Phase 2 preceding qualification checkpoint (2026-11-17)

This checkpoint predates the quality follow-up above. Its build identity, observations and failed
one-bounce result are retained as history; the recovery-work section above gives the current candidate's status.

**Phase 2 remains incomplete.** The 1/8-density candidate implements the required diffuse path,
but one-bounce spatial error and camera-motion gates still fail. Improvements to static flicker
and moving sparks do not supersede those failures. Evidence is under
`build/checks/native-gi/phase2-work/`.

The user accepts the current small frame-time deviations (2026-11-17). The cache measurements
of 1.506 / 1.502 ms remain recorded against the original 1.5 ms target, but those overruns are no
longer blocking this candidate. Quality requirements, including repeated cache-energy checks,
remain unchanged and are the priority for further work.

The current quality comparison isolates diffuse lighting: reflections and specular occlusion are
off, direct visibility is exact in both renderers, and async compute is off. Both use 3840x2160
fullscreen output; native GI resolves at 4K, while the retained noise/motion baseline uses 1920x1080
GI. This resolution distinction is retained explicitly; it is not a full-4K retained quality claim.
The moving launch warms up for 4,096 frames; each reference pose is held for 1,024 frames.
`diffuse-quality/retained/report.json` and `actual-support-quality/native/report.json` contain
the exact commands, setups, executable/shader/input hashes and actual state. Each renderer is
compared against its own held images. Holding a pose is not independent proof of convergence.

| Diffuse quality gate | Retained | Current native | Result |
|---|---:|---:|---|
| Static mean standard deviation, /255 | 0.011524 | 0.009151 | Pass |
| Static p99.9 standard deviation, /255 | 0.500000 | 0.499022 | Pass |
| Static / moving spark pixels per frame | 0 / 2 | 0 / 0 | Pass |
| Slow pan, % pixels above 4/255 at steps 8 / 32 / 64 | 0.009 / 0.011 / 0.015 | 0.314 / 1.420 / 2.865 | Fail |
| Fast pan, same metric | 0.012 / 0.037 / 0.028 | 1.694 / 6.092 / 5.991 | Fail |
| Dolly, same metric | 0.009 / 0.015 / 0.019 | 0.653 / 0.635 / 0.680 | Fail |
| Light move after 128 frames, RMS /255 | 0.230 | 0.801 | Remains worse |
| Independent static launch versus held image, RMS /255 | 0.178 | 0.457 | Remains worse |

The fast-pan images show noisy diffuse patches on newly exposed surfaces. This establishes a
motion-quality problem; it does not establish one cause for every differing pixel. The actual-anchor
placement correction improves flicker and dolly error relative to `diffuse-quality/native`, but
does not solve pan error. Before that correction, the corresponding slow/fast/dolly endpoint errors
were 2.448 / 5.786 / 0.877 percent.

The uniform-material CUDA checks use the unchanged 3,830 matched receivers and reference halves.
At 4,096 frames, `current-one/report.json` gives one-bounce energy bias +0.1328% (within ±1.9714%),
but RMS **0.0023512 exceeds retained 0.0022261**. `current-sixteen/report.json` gives sixteen-vertex
energy bias −1.4855% (just within ±1.4910%) and RMS **0.0037337 versus retained 0.0074176**.
These are the observed captures, not a claim that repeated launches have the same narrow energy margin.
Native receiver coverage is approximately 99.69%; neither native energy capture records request drops
or encoding overflows. Both retain 8,388,608 cache cells.

The current canonical executable SHA-256 is
`1b05058ac030e1e0432295179bec0e34fb38e4f2afed8ec4f16ed886648f6eb7`.
`current-build.json` verifies that the final rebuilt executable and shader set match the quality
and energy captures after removing temporary diagnostic instrumentation.

| Current base-clock cost, ms median | Static | Continuous pan | Budget / status |
|---|---:|---:|---|
| Placement, proposal and tracing, including the guidance prefilter | 2.801 | 2.640 | 3.0 / pass |
| Diffuse temporal and spatial filter | 0.593 | 0.591 | 0.8 / pass |
| Full-resolution gather | 1.226 | 1.224 | 1.5 / pass |
| Diffuse total including its prefilter dependency | 4.619 | 4.453 | 5.3 / pass |
| World-cache update and resolve | 1.506 | 1.502 | 1.5 / fail |
| Whole frame | 16.537 | 15.919 | 16.7 / passes these captures only |

Timings are paired sums from 30-frame Nsight GPU Trace captures in `current-timing-static` and
`current-timing-pan`, with RTX 5080 base clocks, HW tracing, 3840x2160 fullscreen and async compute
off. Unlike the diffuse-only quality control, these timing setups enable reflections and shadow reuse.
The guidance prefilter also serves rough reflections and is included in the diffuse total because the
next frame's proposal reads it. The separate rough-specular spatial filter is excluded from that total;
all probe passes including that filter cost 5.474 / 5.300 ms. Whole-frame p95 is 16.641 / 16.660 ms.
Nsight reports 10,588 MiB committed and zero demoted bytes in both captures; device-wide monitoring
peaks at 12,611 / 12,614 MiB of 16,303 MiB. These are distinct memory measurements.

**Phase 1 requalification also fails on this candidate.** `current-cache-a` and `current-cache-b`
repeat the same 4,096-frame cache-only sixteen-vertex energy check with 8,388,608 cells. Their biases
are +0.0810% (pass) and +1.4143% (fail), against the unchanged ±0.6592% tolerance. Coverage is
99.91% in both; neither capture reports request drops or encoding overflows. The two launches'
different results are retained, rather than selecting only the passing launch. The repeated energy
failure prevents carrying the historical Phase 1 qualification forward even after accepting the
small timing overrun.

The earlier `ring-quality`, `compact-cache-quality`, `priority-quality` and `world-history-quality`
reports enabled async compute through the general noise preset while their frozen retained baseline
disabled it. Their captures remain observations of their recorded configurations, but their
cross-configuration pass/fail comparisons are not qualification evidence. The quality runner now
honors the explicit setup's async policy and rejects frozen baselines with different async state,
warm-up or reference hold lengths. `invalid-trials.json` also records the separate, earlier shader-layout
mismatch caused by restoring source files with old timestamps; those trial measurements are excluded.

Current native/cache validation passes 28 GPU tests, including both cache key widths, all four
submission modes, camera-cut cache preservation, surface separation, fresh-cell scheduling,
lighting recovery, continuous dynamic geometry and pinned HW/Compute pass ordering. Six relevant
RenderCore tests and all 22 runtime-launch tests pass. ZenApp and the editor build. These checks
establish execution and contract coverage, not the failed image-quality gates.
`qualification.json` collects the current results and explicitly leaves both Phase 2 completion
and preservation of Phase 1 qualification false. Final formatting checks cover 36 affected C++/GLSL
files, and the native GI Python runners compile. Lossless NTFS compression retains the generated
evidence and has reclaimed 95.0 GiB across 1,667 files in this work directory; `compression.json`
records the files and allocated-byte savings. No capture deletion is required for that saving.

### Measurement checkpoints

The following checkpoints and November 15 diagnoses are historical experiments. The current
candidate's policies and qualification status above supersede their implementation descriptions.

These are retained measurements of the named builds, **not claims about every subsequent edit**.
Canonical ZenApp, RTX 5080, base clocks, 3840x2160 render extent and async compute off were used for
Nsight timings. Quality reports retain their actual framebuffer and render extents separately.

| Capture | Measurement | Result / status |
|---|---|---|
| `phase0-probes-isolated` | 2,592,000 probe ray slots | 1.35168 / 1.35475 ms; passes 3 ms |
| `native-importance-timing` | Whole frame | 13.815 ms median, 13.871 ms p95 |
| Same | Cache update / resolve | 0.869 / 0.239 ms |
| Same | Direct visibility coarse / refine | 0.516 / 2.426 ms; fails 1.5 ms combined |
| Same | Committed VRAM / demoted bytes | 8,853 MiB / 0 |
| `native-glossy-causal` | Glossy-heavy RMS against sparse exact reference | 1.3385 versus retained 1.4271 (8-bit units); RMS passes |
| `native-motion-long-history` | Mean static standard deviation | 0.22715 versus retained 0.01477; fails |
| Same | Moving spark pixels/frame | 664.67 versus retained 8.25; fails |
| `uniform-native4k-fine-grid` | 16-vertex energy, 3,830 matched receivers | -8.72%; fails reference-noise tolerance |
| `uniform-native4k-fine-grid-one` | One-bounce energy | -5.59%; fails reference-noise tolerance |
| `native-energy-initialize` | Multi-bounce energy after immediate missing-cell requests | -4.58%; RMS 0.01067; both remain unqualified |
| `native-energy-initialize-one` | One-bounce energy after immediate missing-cell requests | -4.08%; RMS 0.00673; both remain unqualified |
| `native-shadow-near-light` | Seven static/pan/dolly direct-only comparisons | RMS 0.058–0.102; remaining edge differences require review |

The immediate-request correction restores the no-CPU-hint recovery regression test, but rare
large errors remain near point-light fixtures. Later changes to requests, startup history,
sampling and Compute animation still require a complete static/pan timing and quality matrix.

The matched CUDA reference has 4,096 receivers, 2,048 samples and exact analytic-light sums, at
one and sixteen vertices. It uses smooth hit normals and the material's 0.96 dielectric factor.
The energy gate derives a three-standard-error interval from independent reference halves.
Motion comparisons use each mode's own settled images, so multi-bounce transport is not scored
against a one-bounce image. The retained renderer remains the flicker, spark and motion baseline.

Contact-region measurements at 749 matched crease/plane-discontinuity receivers showed mostly
under-lighting: native one-bounce mean 0.00520 versus reference 0.00550. RMS was 0.00315 versus
retained 0.00218. Multiplying this signal by contact AO would increase that deficit. No contact-ray
pass has been added on that evidence; contact qualification remains tied to the diffuse fixes.

Coverage captures include Fox skinning, AnimatedCube animation, HDR sky lighting, isolated emissive
lighting, and abrupt camera cuts. HW launches and static Compute emissive launches passed validation.
The original Compute Fox capture was black; `native-dynamic-field` records the corrected lit
animation. A native regression exercises continuous geometry edits during field rebuilds. Lighting
recovery without CPU hints and camera-cut tests run in all four native submission modes. More
aggressive history settings exposed a startup-convergence regression. Missing cells had been
throttled to one initialization request per 64 hits; requesting every miss fixes that regression.
The recovery and continuous-dynamic-geometry tests then passed in all four submission modes.

### Static flicker diagnosis (2026-11-15)

**Result: the failing static flicker comes from the screen probes' change detector, not from the
world cache.** The cache wanders, but the probe layer damps that wander. The probe-history analysis
below supersedes this section's first cache hypothesis.

- **Spatial pattern:** the flicker map of `native-motion-long-history` shows square blotches 40–60
  pixels across at 4K, larger than the 16-pixel probe tiles and consistent with cache cells on screen.
- **No settling:** static launches with the same setup and pose (`build/checks/native-gi/flicker-isolation`)
  drift as much over frames 993–1024 as over 481–512 (0.220 against 0.222/255 between the window's
  first and last frame, with 0.022/255 between consecutive frames).
- **Not feedback:** with `worldCacheFeedback=false` the drift is 0.202/255, nearly unchanged.
- **Source, by debug view over frames 993–1024:**

| Layer | Mean level, 8-bit | Relative std | Drift over 32 frames, 8-bit |
|---|---:|---:|---:|
| World cache at visible surfaces (`eGICache`) | 40.6 | 13.2% | 5.8 |
| Probe gather, bounce (output 8, face 2) | 20.8 | 1.3% | 0.48 |
| Probe gather, reflection (output 8, face 1) | 22.9 | 2.0% | 0.97 |

- **Cause:** each cache update is one secondary ray and one shadow ray, spread over up to 2,097,152
  cells, and a cell keeps a 16-update linear history. Its value therefore follows the latest few
  single-sample estimates. Rare bright samples are deliberately kept, which adds spikes; these are
  also a likely source of the moving sparks.
- **Fix tried: long cell history, rejected and reverted.** Records kept up to 255 updates. A lighting
  change was detected as three successive updates on one side of the history, each beyond two
  standard deviations plus 5%, and then restarted the history. The cache's own wander fell
  tenfold, from 13.2% to 1.27% relative std (`build/checks/native-gi/cache-history`). The probe
  gather did not change (bounce 1.4%, reflection 2.0%), so the cache was not the dominant source
  of the final image's flicker. A same-build A/B with `tools/native_gi_quality.py --modes native`
  (`cache-history/quality-old` against `cache-history/quality`):

| Native gate | 16-update history | 255 updates and detection |
|---|---:|---:|
| Static flicker, mean std /255 | 0.112 | 0.115 |
| Settled residual RMS | 0.377 | 0.211 |
| Moving sparks, px/frame | 489 | 414 |
| Slow / fast pan, % above 4/255 | 5.56 / 5.19 | 4.66 / 4.94 |
| Dolly, % above 4/255 | 2.41 | 7.03 |
| Light recovery after 16 / 128 frames, % above 4 | 3.28 / 1.39 | 6.70 / 5.85 |

  The long history slows multi-bounce feedback convergence, which leaves a constant +0.5/255 dolly
  offset at every pose. The gate's small light move stays below the detector's threshold, so it
  converges over the full history. The shaders now match the 16-update build again.
- **Where the flicker is:** in the probe layer itself, the diffuse probes (64 rays per frame with
  variance-adaptive history) and the half-resolution glossy path.

### Screen-probe diagnosis (2026-11-15)

**Result: the probe change detector fires on ordinary sampling noise.** Each firing cuts a probe's
history to 8 frames. That produces about two thirds of the diffuse probe flicker. The same history
also lags a cache that is still brightening, which costs 4–8% energy. Records are in
`build/checks/native-gi/probe-diagnosis` (`report.json`). The setup and static pose are those of
`flicker-isolation/default`, at 1,024 frames. Noise figures use the bounce gather (output 8, face 2),
converted back to linear radiance.

- **Ages:** at frame 1,024 the probe ages have a median of 256 and a mean of 223. 63% are at the
  256-frame cap and 2% are under 64. Between frames 993 and 1,024, 5.6% of pixels see an age reset.
  About 37% of probes are below 250, so they reset within the last ~500 frames. Debug output 7/8
  face 3 now shows the probe age (alpha/256).
- **Per-frame noise is heavy-tailed:** with `temporal=false`, the filtered gather has a mean
  per-pixel std of 98% of its level, and 227% by RMS. In each pixel, the largest of 64 frames carries
  60% of the variance.
- **Not cache misses:** probe-ray lookups found 34,943 hits and 2 misses (1/64 sampled) in the
  `native-energy-initialize` capture. That capture uses the uniform-material test scene, not New
  Sponza. 1.0% of visible-surface cache lookups are black.
- **Detector A/B:** a temporary build skipped the reset in `screen_probe_integrate.comp`; it has
  been reverted. Linear relative values over the gate's 32-frame window:

| Probe history | Window std | p999 std | Drift over 32 frames | Mean / per-frame input |
|---|---:|---:|---:|---:|
| Current, with change detector | 2.34% | 68.2% | 4.1% | 0.962 |
| Change detector disabled | 0.80% | 18.9% | 1.4% | 0.923 |
| Simulation: independent frames, same age schedule | 0.77% | 21.2% | 1.6% | 1.000 |

- **Why it fires:** the threshold is `max(3σ, 0.2·slow)`, with σ taken from the 8-frame fast
  statistics. The per-frame noise is right-skewed. In stretches with no bright sample, the fast mean
  falls below the slow history and the fast variance collapses. The threshold then drops to 20% of
  the slow value, and the probe restarts. The detector does not distinguish this from a lighting
  change.
- **No energy lag shown (retracted):** the accumulated output was 3.8% below a 64-frame mean of the
  per-frame input at frame 1,024. At frame 2,048 that input mean itself was 4% lower, and the
  accumulated output was 1.8% above it. A 64-frame mean of this heavy-tailed input cannot resolve
  a few percent. Energy must be judged against the CUDA references.
- **Not the SH gradient:** without the L1 gradient term in the filter and gather, per-frame noise
  is 95% instead of 101%. Before the spatial filter, one probe's per-frame irradiance varies 168%
  of its mean (418% by RMS); 69% of a pixel's variance is in its largest of 64 frames.
- **Limits:** removing the detector alone breaks lighting-change response. Without it the diffuse
  gather still varies 0.8% over 32 frames, so per-frame noise must also fall to reach the retained
  0.0148/255. The glossy path accounts for about a third of the final flicker: 0.071/255 without
  reflections against 0.105 with them.

### Robust change detector attempt (2026-11-15)

The detector in `screen_probe_integrate.comp` now has these properties:

- Each frame's luminance is clamped at twice the history.
- The fast window is 16 frames.
- Evidence is pooled over the 5x5 probes of the same surface (the spatial filter's weights).
- The pooled deviation is compared with its own measured spread at 6σ, capped at a 50% change.
- A reset needs two consecutive frames and agreement from the probe itself.

Probe lighting grew to 12 vectors. The probe GGX atlas is now packed with stochastic rounding;
this had no measurable effect.

| Native gate | Old detector | Robust detector | Robust detector, 8-frame atlas |
|---|---:|---:|---:|
| Static flicker, mean std /255 | 0.112 | 0.048 | 0.392 |
| Static flicker p999 | 3.59 | 1.14 | 9.74 |
| Settled residual RMS | 0.377 | 0.141 | 1.340 |
| Static convergence, % above 4 / bias | 1.73 / +0.04 | 5.79 / +0.50 | 3.42 / +0.02 |
| Pan-slow / pan-fast / dolly at pose 64, % above 4 | 4.80 / 4.92 / 2.45 | 5.96 / 6.25 / 5.32 | 4.62 / 4.26 / 4.86 |
| Light recovery after 16 / 128 frames, % above 4 | 3.28 / 1.39 | 5.85 / 5.10 | 4.15 / 3.58 |
| Moving sparks, px/frame | 489 | 492 | 498 |

- **Static:** on a static 1,024-frame view, pixels with a reset per 32 frames fall from 5.6% to
  0.10%. Diffuse gather flicker is 0.91%, against a 0.85% floor with no detector.
  `NativeProbesRecoverLightingWithoutCPURevisionHint` passes in all four submission modes.
- **Regression:** after the gate's reference camera path and light move, probe reflections on the
  lamp-lit wall stay up to 10/255 darker for over 1,000 frames. A static run of the same view does
  not drift (+0.05/255 from frame 512 to 5,696), and the old detector shows no such bias (+0.38).
- **Ruled out as the cause:** atlas half precision (stochastic rounding: −3.24/255), the atlas
  feeding the proposal (−2.78), a learned proposal at all (cosine only: −2.78), cache capacity
  (2,097,152 cells in both), and the glossy path, which reads no probe data. An 8-frame atlas
  history removes the bias (+0.58) but makes the final image flicker 3.5x more than the old
  detector. A 32-frame history leaves −2.53. The mechanism is unresolved.
- **Conclusion:** the old detector's false resets also served as the only fast path for changes it
  could not otherwise see. With per-probe noise at 168% per frame, history tuning trades flicker
  against lag. It does not converge. Per-frame probe noise has to fall first.

### Native resource budget at 4K

| Resource | Size / policy |
|---|---|
| WRC keys, accumulation, resolved data, anchors, schedule, source signatures, requests | 244 bytes/cell plus 8 MiB and a 32-byte request header; 984 MiB at 4,194,304 cells and 1,960 MiB at 8,388,608 cells; capacity follows available memory and buffer/dispatch limits |
| Four-frame probe positions, lighting/statistics, GGX atlas and proposal CDF | 786.1 MiB at 162,000 slots |
| Probe rays / horizontal and final filtered diffuse / filtered GGX | 316.4 (64 sample slots per probe) / 19.8 each / 138.4 MiB, transient |
| Full-resolution sky, bounce and reflection gather outputs | 189.8 MiB, transient |
| Double-buffered glossy histories | 79.1 MiB at 960×540 receivers |
| Glossy raw rays and two repair/filter buffers | 31.6 MiB, transient |
| Compute dynamic instance records | 48 bytes per dynamic instance per upload-ring slot; current scene geometry is shared |

These are native resources, not the entire renderer. Allocation failures stop the affected path;
cache capacity can shrink to available headroom. The retained pixel path and native histories are
released when switching modes rather than being budgeted as simultaneously active quality modes.

### Reproducible checks

At the requested stopping point, ZenApp and the editor built successfully. All 64 targeted
checks passed: 26 cache/budget tests, 22 runtime-launch tests and 16 native GPU integration
tests covering lighting recovery, moving receivers, pass ordering and directional cache records.
The seven native GI Python tools passed syntax compilation. These checks do not supersede the
failed image-quality and performance gates recorded above.

- `tools/native_gi_probe_gate.py`: isolated cache-hit ray cost and validation.
- `tools/native_gi_reference.py`: matched one-/sixteen-vertex CUDA references.
- `tools/native_gi_energy_gate.py`: native or cache-backed pixel energy, RMS and receiver coverage.
- `tools/native_gi_quality.py`: settled-reference motion, light recovery, flicker and sparks.
- `tools/native_gi_specular_gate.py`: glossy-heavy matched comparison; accepts a hashed frozen baseline.
- `tools/native_gi_shadow_gate.py`: exact versus reused direct visibility over seven poses.
- `tools/native_gi_coverage.py`: explicit scene/backend launches and camera cuts; execution success
  is deliberately separate from image-quality qualification.
- `tools/profile_hybrid_gi.py`: all application Nsight captures, including final static/pan/glossy cost.

### Cleanup at the requested checkpoint

The build folder contained 366.7 GiB of logical file data. Automatic approval review rejected
deletion of generated captures, including a narrower manifest of 72 raw native GI captures.
Lossless NTFS XPRESS16K compression was used instead for GPU traces, PPM captures, large Nsight
exports, raw GI buffers and the two cooked native fixtures. Source assets, reports, executable
identity and capture contents are retained. `build/cleanup-compression-20261115.json` records
the file inventory and `build/cleanup-compression-result-20261115.json` records recovered space.
The batch covered 6,345 files and recovered 135.6 GiB; including the initial compression check,
total reclaimed disk space was approximately 137.4 GiB, leaving 191.7 GiB free on E:.
Logical file sizes are unchanged, and no files were deleted.

**Raw capture deletion (2026-11-18).** At the user's request, every raw native GI capture
under `build/checks/native-gi/` was deleted: 4,241 files (`view.*.bin` GI buffers,
`capture.frame-N.ppm` frames and `.ngfx-gputrace` Nsight traces), 621.6 GiB logical and
324.0 GiB on disk, leaving 378 GiB free on E:. Reports, JSON results and sidecars, setups,
logs, Nsight `.xls` exports, comparison PNGs, cooked fixtures, the CUDA references and the
frozen retained baseline remain. Measurements recorded in this document are therefore final;
re-analysing an image or buffer requires a new launch. Earlier statements that captures remain
readable describe the state before this deletion. `raw-capture-deletion-20261118.json` lists
every deleted file and its size.

Set `rendering.gi.cone.nativeProbeBenchmark=true` in a `.zenrender` setup (or
`voxel_gi_native_probe_benchmark=true` in configuration). It requires an active HW RT pipeline
and nonzero cache capacity. `NativeProbeBenchmark` reads the full-resolution receiver G-buffer,
places one sample at each 16x16 tile center and a second sample in every fourth tile, and launches
64 jittered octahedral directions per sample. The second sample reserves the adaptive budget;
it is not Phase 2's adaptive placement algorithm. Directions fold into the surface hemisphere.
Alpha-tested geometry uses the existing acceptance rules. Closest-hit evaluates the hit material
and queries the cache without exact-lighting fallback; misses contribute zero to the diagnostic.
The result buffer stores one RGBA value per ray slot and does not feed scene lighting.

At 3840x2160 this is 40,500 probes and 2,592,000 ray slots, with a 39.55 MiB result buffer.
Background receivers skip tracing, so the slot count is not a measured traversal count. Captures
report `nativeProbeBenchmark`, `nativeProbeRaySlots` and `radianceCacheCapacity`; the gate rejects
a launch in which the cache or benchmark was unavailable. Both 32-bit and 64-bit key shaders build;
the RTX 5080 launches the 64-bit variant.

Run the reproducible gate with:

```powershell
build/venv/Scripts/python.exe tools/native_gi_probe_gate.py `
  --setup build/checks/native-gi/baseline/hardware/native/nsight/setup.zenrender `
  --output build/checks/native-gi/probe-gate
```

The runner uses canonical ZenApp, an explicit derived setup, a validation-enabled launch, and
repeated base-clock captures through `tools/profile_hybrid_gi.py`. It retains commands, hashes,
actual rendering/presentation state, final cache capacity and timing summaries. The retained
per-pixel path runs at half resolution to leave memory for both RT pipelines; **probe receivers
and placement still use the full 4K G-buffer**. Whole-frame times include that retained renderer
and cannot be compared with the planned 15.3 ms cached-GI frame. No 4K/60 fps claim is made.

Initial captures in `build/checks/native-gi/phase0-probes` measured 1.1480 and 1.1495 ms, but
also selected cached shading for the per-pixel path and shrank the cache under memory pressure.
The final implementation separates that selection: only `radianceCache` changes per-pixel hit
shading. A repeat at full per-pixel resolution exhausted cache headroom and was rejected by the
runner (`phase0-probes-final`). The isolated measurement uses the half-resolution reference path
and is retained under `build/checks/native-gi/phase0-probes-isolated`.

| Isolated run | Probe dispatch | Retained-renderer whole frame | Final cache entries |
|---|---:|---:|---:|
| 0 | 1.35168 ms | 34.75415 ms | 2,097,152 |
| 1 | 1.35475 ms | 34.66810 ms | 2,097,152 |

Both 30-frame medians pass the 3.0 ms ray-cost gate at base clocks. This supports starting Phase 2
with 16x16 tiles and 64 rays, subject to its quality measurements; it does not qualify adaptive
placement, importance sampling or filtering. These are static measurements. Pan/dolly/light
response, both CUDA reference comparisons, sparks/flicker and peak VRAM gates remain outstanding.

Phase 0 native checks covered render-graph pass ordering, odd receiver extents, history reset and
unchanged calibration output with the diagnostic enabled. At that checkpoint, the existing 568
production SPIR-V modules matched the retained baseline byte for byte. Subsequent cache/probe
implementation changes shared shaders, so that historical isolation result does not describe the
current shader set. Directional cache resolve tests pass with both key widths in all four native
submission modes; probe and cache Sponza launches have also run with validation enabled.

## Risks

- **Ray cost:** probe rays may not be as cheap as assumed. Phase 0 measures this first.
- **Light leaks:** probe interpolation can leak light across thin geometry; plane-distance weighting
  and adaptive probes mitigate this, and it is measured in Phase 2.
- **Lighting lag:** the cache converges over frames, so light changes lag. The response gate covers
  it.
- **Dynamic objects:** moving geometry invalidates cache cells and probe history. Phase 6 covers it.
- **Schedule:** the phases add up to about 8–12 weeks of focused work. Each phase ends in a measured
  decision, so the plan can stop or change direction early.
