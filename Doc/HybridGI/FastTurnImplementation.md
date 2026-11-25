# Fast-turn GI implementation results

All motion counts use standalone ZenApp, native 3840×2160, New Sponza Optimal, HDR, normal maps, hardware RT and 162,000 screen probes on the RTX 5080. The initial captures used 4,194,304 world-cache cells. Later comparisons give their measured capacity. An image level is one step on the 8-bit display scale. Shader values quoted below are linear. The GPU sample counter in the diagnostic logs is one less than the scene frame for these launches.

## Result against the revised 0.1% target

**Passed on all 326 scored frames of seven rapid 180° replays.** Five independent sampling phases used the six-frame sweep, one sweep turned in the opposite direction, and one run used an instant cut. Each run also tested the return and recovery. All scored frames and their references had 1,048,576 world-cache cells. The retained lamp and placement changes are local commits `2aacda85` and `2f022f6c`. [Per-run results and capture hashes](../../build/fast-turn-implementation/acceptance180.json)

| Native 4K, worst single frame | Pixels out of 8,294,400 | Percentage |
|---|---:|---:|
| Isolated bright or dark sparkles | 307 | 0.00370% |
| All pixels with a large channel error, including contiguous patches | 2,453 | 0.02957% |
| Allowed by the strict 0.1% budget | 8,294 | <0.1% |

For each frame, compare RGB to the mean of five settled images at the identical camera pose. The conservative gate counts each pixel once if any channel differs by more than 24 display levels in either direction. The sparkle subset also requires over 16 levels of local contrast against the surrounding ring. The worst sparkle count occurred on the second frame of the mirrored sweep. The largest high-error area occurred on the fourth frame of a forward sweep. At 180° landing the five forward phases had 61–98 isolated sparkles and 740–843 high-error pixels. The instant cut had 156 and 909 respectively. These measurements quantify transient differences from settled lighting. [Mirrored sweep](../../build/fast-turn-implementation/rapid180-mirrored/report.json) · [Instant cut](../../build/fast-turn-implementation/rapid180-cut/report.json) · [Native-pixel comparison clip](../../build/fast-turn-implementation/180-placement-comparison.webp)

On the matched phase-zero sweep alone, the worst isolated count fell from 423 to 184 and the worst high-error count fell from 2,856 to 2,130. The baseline also met the percentage target. The physical sampler checks passed in 40 configurations covering lamp count and direction, cold and warm probes, learned guides and environment sampling. The original detector still reproduced exactly 33 clusters and 53 bright pixels from the archived landing. [Baseline replay](../../build/fast-turn-implementation/base180-s0/report.json) · [Retained changes](../../build/fast-turn-implementation/rapid180-s0/report.json) · [Proposal quadrature](../../build/fast-turn-implementation/proposal-quadrature-final.json) · [Archive parity](../../build/fast-turn-implementation/parity-final.json)

Reproduce one sweep with `tools/native_gi_fast_motion.py --production-turn sweep --degrees 180 --phase 0 --warmup 1024 --hold 1024 --cache-cells 1048576 --max-noise-pct 0.1 --setup <setup.zenrender> --output <new-directory>`. The command reports the worst frame, writes `noise_gate` to `report.json`, and exits with a failure when the limit is reached. Each launch records its executable, shader and input hashes. The 4,194,304-cell path has the earlier stage comparisons below. A five-phase qualification of the retained shaders at that capacity remains open.

## Reproduction and instrumentation

The tracked centered-ring detector recovered 33 clusters covering 53 pixels at the original six-frame landing, 16 clusters at +4, two at +64 and eight on the cut. The largest channel excess and the local contrast are measured independently. Adjacent marked pixels belong to one 8-connected component. [Rescore of the original captures](../../build/fast-turn-implementation/archive-parity.json)

Production replay is available through `tools/native_gi_fast_motion.py --production-turn sweep --degrees 180 --setup <setup.zenrender> --output <new-directory> --hold 1024`. A negative angle mirrors the path. Use `cut` and ±90, ±120 or ±180 for isolated turns. Each invocation starts a fresh scene, holds the base pose, visits the destination, then returns. It scores every sweep frame and recovery frames +1 through +16, +32 and +64 against five same-build settled frames per pose. Six further frames per pose are reserved for the floor. Use `--phase 0` through `--phase 4` for independent GI scrambles at the same scene and camera times. Phase zero reproduces the original sample index. Any change of cache capacity, framebuffer, GI extent, camera matrix or measured inputs invalidates the comparison. [Replay tool](../../tools/gi_turn_replay.py)

The camera regression exposed rounding in interpolated holds. With New Sponza's authored decimal coordinates, the previous vector interpolation changed the fixed eye at scene frame 1040. Each coordinate now uses endpoint-preserving interpolation. The regression failed before the change and all 22 runtime launch tests passed after it. The two largest lantern flashes remained present with the corrected camera. [Failing regression](../../build/fast-turn-implementation/camera-before.log) · [Passing tests](../../build/fast-turn-implementation/camera-after.log) · [Corrected-camera baseline](../../build/fast-turn-implementation/fixed-replay/result.json)

The gathered irradiance debug view (output 7, face 4) displays valid receivers whose total compatible probe weight is zero. Sky, material darkness and separately added environmental sources cannot create a white pixel. At arrival the mask contained 4,196 pixels, including 846 absent from the settled mask. The settled mask contained 3,633 pixels (0.044% of the framebuffer). The earlier estimate based on black GI pixels measured a different quantity. [Coverage data](../../build/fast-turn-implementation/coverage-base/result.json)

## Measured producers

**Ledge.** At (2316, 1366), adaptive probe 150072 supplies essentially the entire gather weight. At GPU sample 1032, age four, its filtered bounce rose from about 0.004 to 0.134. Hemisphere lane 0, batch 0 hit material 11 near scene lamp 8 with a cosine of 0.0452 and mixture density 0.0647 per steradian. Its projected luminance contribution before normalization was 7.68, accounting for 89.8% of that probe's bounced observation. Emission was zero. The cache returned red irradiance/pi of 32.04 versus 31.78 from an independent all-light visibility evaluation at that hit. [Ray-level records and neighboring probes](../../build/fast-turn-implementation/rays-omm/ray-records.json) · [Independent lighting and directional density](../../build/fast-turn-implementation/exact-hit/stdout.log)

The bright hit direction was 1.44° from lamp 8. The lamp had the second highest unprojected power at the probe, but its geometric cosine was only 0.0203, placing it outside the selected eight. The fitted mixture at that direction had a density of 0.0191 per steradian. The cold batch allocated two hemisphere, four learned, eight environment, two CDF and zero explicit lamp lanes. Changing the lobe permutation in those absent lanes cannot correct this observation.

**Lantern.** At (2452, 1016) and (2453, 1002), the opaque surface, depth and normals stayed stable. Direct lighting and emission were zero. Both pixels had zero diffuse and glossy gather support at arrival. The uncovered-reflection pass then traced a mirror ray for each rough receiver. Its red radiance was 10.70 and 7.17, producing red display levels of 129 and 104. One frame later the first pixel's reflected red was 0.0045. The measured error was in the fallback for rough reflections. [G-buffer observations](../../build/fast-turn-implementation/lantern-before/stdout.log) · [Gather and composition terms](../../build/fast-turn-implementation/lantern-gather/stdout.log)

The following frame accepted weights of 9.05 × 10⁻³⁵ and 9.28 × 10⁻¹⁵ from one probe. Its normal had dot products of 0.212 and 0.246 with the two receivers. Normalizing either weight made that probe responsible for the entire reflection. At (2427, 1031), a settled probe contributed through the spatial tail at a weight of 3.85 × 10⁻²⁰. This explains how a fixed camera could alternate between tracing a local reflection and displaying a different surface's estimate. [Per-tap weights](../../build/fast-turn-implementation/lantern-weights/stdout.log)

## Native 180° experiments

The first table uses the original turn-and-hold recording with the corrected camera and a 4,194,304-cell cache throughout. Each variant has its own five-frame settled reference. Counts are whole-frame bright clusters. The ledge column is the highest RGB RMS in its 25 × 4 crop among the eight sampled landing and recovery frames. A single launch measures an example of the error, and needs additional phases to establish repeatability.

| Change under test | Landing | +1 | +4 | +16 | +64 | Instant 180° cut | Peak ledge RMS /255 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Corrected-camera baseline | 21 | 9 | 19 | 3 | 0 | 8 | 15.94 |
| Grazing-lamp projection and two retained cold lamp lanes | 20 | 6 | 6 | 2 | 1 | 9 | 3.70 |
| Add adaptive placement at competing surface IDs | 19 | 3 | 6 | 2 | 1 | 6 | 2.55 |
| Also trace eight rough reflection samples on uncovered pixels | 22 | 5 | 6 | 0 | 1 | 4 | 3.26 |
| Also reject glossy probe weights at or below 0.001 | 53 | 16 | 14 | 10 | 11 | 8 | 3.71 |

At +4 the lamp change reduced the ledge's red level from 38 to 11, against settled values of 8 and 7 respectively. The corresponding crop RMS fell from 11.23 to 1.60. In the surface-ID placement experiment, settled holes fell from 3,633 to 3,399. It filled 254 holes and created 20 others. At landing it filled 306 and created 65. [Baseline](../../build/fast-turn-implementation/fixed-replay/result.json) · [Lamp](../../build/fast-turn-implementation/lamp-projection/result.json) · [Placement](../../build/fast-turn-implementation/placement-color/result.json) · [Placement coverage](../../build/fast-turn-implementation/placement-coverage/result.json)

**Rejected reflection repairs.** Eight stable rough samples and the 0.001 cutoff made two conspicuous lantern pixels substantially more stable. They increased the whole-frame landing count to 53. On the held-out lamp arrangement the landing count rose from 14 to 32 and seven clusters remained at +64 against zero on baseline. An interleaved 80-frame trace put the uncovered-reflection pass at 2.334 ms against 0.181 ms on baseline. The frame median rose from 21.58 to 23.78 ms and p95 from 21.95 to 24.32 ms. These timing runs used ordinary panning before testing narrowed to 180° turns. [Wide-cutoff capture](../../build/fast-turn-implementation/support/result.json) · [Alternate baseline](../../build/fast-turn-implementation/alternate-b/result.json) · [Alternate candidate](../../build/fast-turn-implementation/alternate-c/result.json) · [Baseline trace](../../build/fast-turn-implementation/trace-b1/summary.json) · [Candidate trace](../../build/fast-turn-implementation/trace-c1/summary.json)

An independent 40 × 76 crop used a 4 × 4 primary grid per native pixel and 256 paths per primary sample. Mean linear red indirect specular was 0.0704, 0.0642 and 0.1054 at the three lantern pixels. In a vertex-normal 3,072-frame engine capture the wide repair yielded 0.0883, 0.0300 and 0.5879. The third pixel remained too bright. The crop integrates 16 subpixel locations, whereas ZenApp shades the pixel center. Motion captures also enable material normal maps. [Reference settings](../../build/fast-turn-implementation/lantern-reference.json) · [Reference arrays](../../build/fast-turn-implementation/lantern-reference.npz)

The driver budget subsequently reduced some launches to 1,048,576 cells. With that capacity, lowering the cutoff to 10⁻¹² and tracing eight rough samples kept the first two lantern pixels at red 64 and 48 through +64 in one replay. Two samples produced red 2 and 0 and left them there. A 16-sample variant for rough metallic receivers reduced the two original flashes but produced new ones on the same lantern at (2454, 996) and (2429, 1032). It had 27 landing clusters against 18 for its matched baseline. [Matched baseline](../../build/fast-turn-implementation/base-lower/result.json) · [Eight samples](../../build/fast-turn-implementation/support-tiny/result.json) · [Two samples](../../build/fast-turn-implementation/support-pair/result.json) · [Metallic receivers](../../build/fast-turn-implementation/metal16/result.json)

## Following a whole 180° turn

The production sweep turns 30° per rendered frame for six frames. The angles shown below are frames of one turn. At every frame the reference is settled at that exact camera pose. Both launches below used 1,048,576 cells in every scored frame. Stationary and moving references with different cache capacities are rejected by the replay.

| Whole-frame clusters | Frame 1 | Frame 2 | Frame 3 | Frame 4 | Frame 5 | Frame 6 |
|---|---:|---:|---:|---:|---:|---:|
| Outbound baseline | 7 | 34 | 24 | 35 | 20 | 14 |
| Outbound lamp change | 15 | 37 | 25 | 31 | 12 | 22 |
| Return baseline | 16 | 34 | 33 | 20 | 22 | 38 |
| Return lamp change | 21 | 30 | 37 | 20 | 19 | 35 |

The lamp change recovered to zero landing-pose clusters at +16, +32 and +64 on the outbound trip. On the return, two remained at +64. Its six independent held frames ranged from zero to one cluster at 180° and one to two at the starting pose. It exceeded the original settled-envelope target. [Baseline](../../build/fast-turn-implementation/base180-s0/report.json) · [Lamp change](../../build/fast-turn-implementation/lamp180-s0/report.json) · [Native-pixel crop playback, baseline left and candidate right](../../build/fast-turn-implementation/180-before-lamp.webp)

## Probe placement during a rapid turn

An anchor can reproject onto the right surface yet represent a different part of the probe footprint. Previously it could keep its displaced position on the landing frame and return to the current receiver one frame later. That switched the reflection estimator on narrow geometry with a stationary camera. When the screen motion of the anchor exceeds one probe spacing, placement now uses the current receiver immediately and carries forward compatible lighting history. This uses the existing probe and ray slots.

On the corrected-camera 180° recording with 1,048,576 cache cells, the original shaders gave 18 landing clusters and 19 at +4. Changing placement alone gave 20 and 14. Combining the lamp proposal and placement changes gave nine and three. The maximum landing excess fell from 129 display levels to 33. At the two lantern pixels the combined change selected the same dark estimate at landing and during the hold. A correct settled reflection on thin metal remains an accuracy issue. The supersampled crop above gives a reference for that separate problem. [Placement alone](../../build/fast-turn-implementation/rapid180-only/result.json) · [Combined](../../build/fast-turn-implementation/rapid180/result.json) · [Matched native-pixel playback](../../build/fast-turn-implementation/180-placement-comparison.webp)

The extra placement search across normals and surface IDs reduced the late reference mask to 3,256 holes. It still produced the original two lantern flashes. The alternative that traced eight rough rays with deterministic placement moved the strongest persistent error to (2454, 996), where the pixel stayed 89 levels above reference through +64. Both experiments were retired. [Surface coverage](../../build/fast-turn-implementation/geometry180-mask/result.json) · [Rough reflection experiment](../../build/fast-turn-implementation/canonical180-rough/result.json)
