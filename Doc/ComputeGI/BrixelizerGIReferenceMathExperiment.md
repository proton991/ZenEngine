# Brixelizer field sampling experiment

2026-10-27. This numerical experiment identifies a local-detail candidate and a concrete quality/memory tradeoff. It does **not** qualify the production field builder, native GPU traversal, New Sponza image quality, alpha coverage, or GPU performance. Those remain gates in [the implementation plan](BrixelizerGIImplementationPlan.md).

At a hit threshold of half a sample spacing, the synthetic 1 cm slot needs roughly 0.1 cm sampling to preserve every tested core-opening ray while blocking the tested walls. For 2 cm and 4 cm slots, 0.5 cm sampling also preserves every tested core-opening ray. Dense rays near aperture edges still lose transmission or exhaust the local iteration budget. A uniform field at those pitches across the full New Sponza extent exceeds the initial 256 MiB persistent-field budget even before atlas allocation. Local or nested detail therefore needs its own measured coverage, memory, and image-quality experiment; these results do not establish that such a solution is impossible or qualified.

## Reference math and model boundary

The source is SDK v1.1.4 commit [`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/commit/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55), pinned in [the source map](BrixelizerGISourceMap.md).

- A brick of side `B` has eight samples spanning seven intervals: `h = B/7`. The field stores distance normalized by `B`, rather than `h`.
- Exact nearest-triangle distances are evaluated at every sample in float64. The reference seed/compression conversion is reproduced algebraically: `sqrt((d/h)^2/16) * 4/7 = d/B`. Values are clipped to `[0,1]` and rounded to an 8-bit integer, then trilinearly interpolated. This is an unsigned triangle-distance field, not an occupancy-distance transform. [Compression source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_cascade_ops.h#L1434)
- The reference inner march samples `distance - EPS`, declares a hit when that result is `< EPS`, and steps by the result times `B`. Thus the effective hit threshold is `2 * EPS * B`. The experiment expresses this threshold as a multiple of `h`, avoiding ambiguity between normalized tolerance and world distance. There are eight distance evaluations per brick. [Traversal source](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55/sdk/include/FidelityFX/gpu/brixelizer/ffx_brixelizer_trace_ops.h#L239)
- Local eight-sample exhaustion is reported as **unavailable**, in accordance with the native plan. Upstream's boolean tracer instead advances to the next cell after that loop; this experiment does not silently classify exhausted rays as covered misses.
- The numerical traversal uses conservative whole-cell bounds, clips a crossing step at cell exit, and advances by `B/1024`. It omits upstream's `0.001` normalized brick-exit margin, packed tight AABBs, the outer 32-iteration limit, cascade fallback, and arbitrary neighbouring atlas tiles. It is not a byte-for-byte replay of the complete AMD traversal.
- Construction is idealized: sparse emission, small-triangle centroid substitution, half-packed edges, Eikonal approximation, allocation failures, and material alpha are excluded. Complete exact-distance fields make the geometry representation ideal, while conservative bounds can consume more march iterations than tight bounds. The overall experiment is consequently neither a guaranteed upper nor lower bound on production quality.
- R8 conversion uses nearest rounding with ties to even. Float32 shader arithmetic, actual storage-image rounding, and device filtering precision are not measured here. Tight threshold equalities can differ on the GPU.

## Fixture and measurement

The fixture is a 1 cm thick, 60 cm wide wall, with a closed-wall control and 1/2/4 cm wide, 60 cm tall slots. Walls are closed triangle meshes. Each slot is formed by four boxes; an independent Möller–Trumbore segment oracle determines whether each ray is geometrically blocked. The field-distance implementation is checked against an analytic box-distance function at 1,000 deterministic random points, including faces, edges, corners, and interior points. The segment oracle is separately checked on known hit, miss, and outward-contact cases.

The sweep uses:

| Variable | Values |
| --- | --- |
| Sample spacing | 0.1, 0.25, 0.5, 1, 2, 4, 7 cm |
| Independent aperture-axis and wall-depth grid phase | 0, 0.25, 0.5, 0.75 samples: 16 alignments |
| Ray angle from wall normal | 0°, 30°, 60° |
| Effective hit threshold | 0.125, 0.25, 0.5, 0.75, 1 sample spacing |
| Transmission rays | 49 dense aperture positions plus 50 opaque controls per configuration |
| Separate outward-surface rays | 34 positions per angle, with outward normal bias 0, 0.25, 0.5, 1 sample spacing |

All rays lie in the fixture's symmetry plane. The two neighbouring y sample planes have identical exact distances, so their common values produce the same trilinear samples without materializing an unnecessary full 3D volume. This symmetry is checked numerically. The run evaluated **665,280 transmission rays** and **228,480 outward-surface rays**.

“Open survival” counts geometrically open rays that finish as covered misses. An unavailable ray is not counted as successful transmission or as a wall leak. “Core” is an additional diagnostic subset with minimum clearance from the slot sides of at least 20% of slot width, measured across the wall thickness. It is not a replacement image-quality gate. Oblique rays that intersect a side wall are excluded by the independent oracle; notably, the 1 cm slot has no geometrically open 60° rays.

## Results

At an effective threshold of `0.5 h`, open survival is:

| Sample spacing (cm) | 1 cm slot, all open / core (%) | 2 cm slot, all open / core (%) | 4 cm slot, all open / core (%) |
| --- | ---: | ---: | ---: |
| 0.1 | 77.86 / 100 | 86.47 / 100 | 97.54 / 100 |
| 0.25 | 54.64 / 97.08 | 74.86 / 100 | 90.44 / 100 |
| 0.5 | 11.16 / 23.75 | 62.64 / 100 | 81.89 / 100 |
| 1 | 0.62 / 0 | 25.82 / 49.57 | 64.85 / 96.11 |
| 2 | 0 / 0 | 2.27 / 4.69 | 38.14 / 66.80 |
| 4 | 0 / 0 | 0 / 0 | 8.49 / 13.32 |
| 7 | 0 / 0 | 0 / 0 | 2.99 / 4.10 |

Each entry aggregates the same 16 alignments and three angles. Per spacing/threshold, the exact-open populations are 1,120/1,456/1,872 rays for the 1/2/4 cm slots; their core subsets contain 480/704/976 rays. These are deliberately sampled ray populations, not an estimate of a rendered pixel or light-energy percentage.

At `0.5 h`, all closed-wall controls are blocked without unavailability. There are zero observed wall leaks in the 2 cm and 4 cm slot cases. The 1 cm slot at 1 cm spacing has two leaking blocked rays out of 3,632; the other spacing cases have none at this threshold. The 2 cm slot has unavailable rates of 1.75%, 2.44%, 2.80%, 3.89%, 0.13%, 0%, and 0% of all tested rays as spacing increases through the table.

Reducing tolerance does not solve the tradeoff by itself. For example, the 2 cm slot at 0.1 cm spacing improves open survival from 86.47% at `0.5 h` to 91.21% at `0.25 h`, but the latter leaks 25.76% of geometrically blocked rays and leaves 34.39% of all rays unavailable. Grid phase matters even when the wall spans many samples: an unsigned interpolated field can have a positive minimum across a surface positioned between sample planes.

The separate self-occlusion sweep at `0.5 h` falsely blocks every outward ray with zero or `0.25 h` normal bias. At `0.5 h` bias the rate is 50% for spacings through 2 cm, and zero for 4/7 cm; at `1 h` bias it is zero throughout this fixture. This does not recommend a universal one-sample bias: moving origins that far can skip nearby occluders, which this outward-only test does not measure.

Plots from the recorded result are available in [slot-tradeoff.png](E:/Dev/ZenEngine/build/compute-gi/brixelizer-experiment/slot-tradeoff.png) and [surface-bias.png](E:/Dev/ZenEngine/build/compute-gi/brixelizer-experiment/surface-bias.png). Both were rendered and visually inspected.

## Uniform coverage cost estimate

The retained New Sponza reference records normalization scale `0.027588581738687685`. The current [reference loader](E:/Dev/ZenEngine/tools/ground_truth_mitsuba.py:165) divides positions by the longest referenced-geometry extent and returns `scale = 1/extent`, so the source-asset extent is **36.24687957763672 m**, not twice that value. This estimate covers a cube of that side; it does not infer the current radiance-cache padding or extrapolate historical occupied-brick counts.

For a hypothetical flat uniform brick map at spacing `h`, `N = ceil(36.24687957763672 / (7h))` and the 32-bit map alone costs `4N³` bytes. The current native **experiment** has a separate occupancy/availability pyramid including its leaf copy: its asymptotic map-plus-pyramid cost is `4N³ * (1 + 8/7)`. This is distinct from AMD's fixed 64³ AABB tree, whose 17,944 bytes add about 1.71% to each 1 MiB map.

| Spacing (cm) | Brick cells per axis | Flat map alone (GiB) | Map plus current experimental pyramid, approximate (GiB) |
| --- | ---: | ---: | ---: |
| 0.1 | 5,179 | 517.4851 | 1108.8967 |
| 0.25 | 2,072 | 33.1382 | 71.0105 |
| 0.5 | 1,036 | 4.1423 | 8.8763 |
| 1 | 518 | 0.5178 | 1.1095 |
| 2 | 259 | 0.0647 | 0.1387 |
| 4 | 130 | 0.0082 | 0.0175 |
| 7 | 74 | 0.0015 | 0.0032 |

These analytical estimates exclude atlas, brick metadata, construction scratch, retirement, and lighting allocations. They also exclude power-of-two or fixed-tile padding, which can increase the actual layout cost. A fixed 128 MiB atlas does not guarantee that all required surface bricks fit at finer pitch. The table is a flat-layout lower-bound comparison, not a memory measurement of AMD's nested cascades or a proposed production allocation.

The finest rows are incompatible with the initial 256 MiB budget when applied uniformly. A local 64³ map at 0.1 cm spacing spans 44.8 cm; one at 0.5 cm spans 2.24 m. Local fields of this kind are worth investigating around detailed lamps, with coarser complete-scene coverage, but the number of required regions, total active bricks, overlap, transition behaviour, moving geometry, and arbitrary-origin visibility remain unmeasured. A native GPU builder and the existing New Sponza image gates must decide feasibility.

## Reproduction and retained evidence

The durable reproduction is [tools/experiment_brixelizer_reference_math.py](E:/Dev/ZenEngine/tools/experiment_brixelizer_reference_math.py). It has no engine import, production CPU builder, or renderer registration. From the repository root:

```powershell
$env:OPENBLAS_NUM_THREADS = '1'
$env:OMP_NUM_THREADS = '1'
& 'build/venv/Scripts/python.exe' 'tools/experiment_brixelizer_reference_math.py'
```

NumPy is required. `--output-dir` changes the generated-artifact directory. `--plot-only` renders the saved JSON with a Python environment containing matplotlib; `--plot-libs` optionally names a local dependency directory. The numerical run used Python 3.14.7 / NumPy 2.5.3. Plots used the bundled Python 3.12.14 / NumPy 2.3.5 and local matplotlib 3.11.2, reading the same saved numerical results.

The ignored [results.json](E:/Dev/ZenEngine/build/compute-gi/brixelizer-experiment/results.json) contains all 6,720 transmission and 6,720 surface configurations, aggregate counts, model limitations, and script SHA-256 `7f36ff3df6923751de786cc97a32b5a032628cd86dffd8cf26b002bfab90177d`. The same directory contains `transmission.csv`, `surface.csv`, a completion checkpoint, and both plots. The recorded CPU run duration is reproduction information only; it says nothing about GPU construction or tracing performance.

The source extent was obtained from `build/sponza-gi-investigation/bounce/cuda/bounce-reference.json`, SHA-256 `a86a2ea3fadbe563fac7c96477001e16ec8542e892f5c7a15503c59501492b27`, and checked against the current normalization code. Tables in this document retain the key evidence even when ignored local artifacts are absent. Earlier exploratory runs used a different brick-grid origin; the completed run above supersedes their preliminary percentages.
