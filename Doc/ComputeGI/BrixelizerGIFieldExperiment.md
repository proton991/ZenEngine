# Native distance-field tracing experiment

2026-10-27. Milestone 2 experiment; **no field preset is qualified for production GI**. The native GPU tracer passes its analytic and availability checks. The initial coarse sweep cannot preserve the tested openings while blocking every opaque ray; a targeted 4.464 mm sweep now provides a local-detail candidate on four sampled phase pairs. This is an isolated test path: production GI still uses the migration implementation described in [the execution record](BrixelizerGIExecution.md).

## Implementation and reference

The reference is FidelityFX SDK `v1.1.4`, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. [The source map](BrixelizerGISourceMap.md) records the pin, normalization derivation, and construction requirements. [distance_field_trace.glsl](../../Data/Shaders/VoxelGI/distance_field_trace.glsl) preserves AMD's copyright and MIT permission notice for adapted atlas addressing, distance stepping, and gradients.

The experiment uses eight samples per brick axis, spanning **seven intervals**. For brick extent `B`, sample spacing is `h = B / 7`. Atlas values are unsigned distance divided by `B`, rounded to R8 UNORM. Local coordinates `[0,1]` map to texel centers `0.5..7.5`; interpolation never crosses into an adjacent atlas tile. As in the pinned tracer, the march subtracts `epsilon` from the sampled distance and accepts a hit when the adjusted distance is below `epsilon`. Its effective acceptance threshold is therefore `2 * epsilon * B`, not one quantization step.

Zen's deliberate deviations are a fixed bitmask mip hierarchy instead of AMD's cascade AABB stamps, explicit slab entry for outside origins, and an **unavailable** result when data or either iteration limit prevents completion. The inward one-sided gradient uses AMD's `0.25 / 7` offset. After the initial synthetic runs below, the GPU-built slanted-plane test exposed a nearly tangent gradient at an unsigned-distance cusp. The current tracer probes one sample interval toward the incoming side, then half an interval and the hit point if necessary, resolving each probe's own brick. It accepts only a measured incoming-facing gradient and otherwise returns zero; it never flips or invents a normal. The original slanted-plane test now passes in all four submission modes; broader phase/orientation coverage remains a separate check. The isolated fixtures below use one fixed visibility level and CPU-authored exact samples; production GPU construction and material-coverage qualification are separate gates.

The reusable shader ABI is in [distance_field_common.glsl](../../Data/Shaders/VoxelGI/distance_field_common.glsl):

| Set 4 binding | Resource | Contract |
| --- | --- | --- |
| 0 | `uDistanceFieldData`, 64 bytes | Grid minimum and brick extent; power-of-two cells per axis, atlas capacity, hierarchy level count and default trace budget; atlas texel dimensions; tolerance and per-brick sample limit. |
| 1 | `DistanceFieldCells` | `0` is completed empty, `UINT_MAX` is unavailable, other values are atlas brick index plus one. |
| 2 | `DistanceFieldHierarchy` | Leaves first, then successively halved dimensions through the root; each parent ORs eight children. Occupied bit `1`, unavailable bit `2`. |
| 3 | `distanceFieldAtlas` | Linearly sampled R8 UNORM 3D atlas, eight-texel tiles. |

`DistanceFieldTrace(origin, normalizedDirection, tMin, tMax, budgetOverride)` returns hit, covered miss, or unavailable, plus world hit distance/position, gradient normal, brick index, work count, and distance-sample count. It traces `[tMin,tMax)`. A zero budget override selects the uniform budget. Current work counts include hierarchy loads, map loads, and every distance texture sample, including the normal probes. Each evaluated normal probe costs one map load and four texture samples; invalid map entries cost only the lookup. If no valid normal fits within the remaining budget, an established geometry hit retains a zero normal. Fixture limits are 2,048 total operations and 64 primary samples per brick; they are experimental settings, not measured production defaults. Historical tables below predate this normal/counter correction: their iteration count includes hierarchy loads and primary samples, and their distance count includes exactly four normal samples per hit.

A published field certifies full committed scene coverage independently of the bounded radiance cache. Space outside that complete coverage can be empty; an unknown leaf inside it cannot be treated as sky. The isolated box fixture checks an external blocker from a receiver in a smaller cache volume. **Actual sky-cache/local-light consumers, cache invalidation during coverage changes, and out-of-cache radiance rejection remain production integration work.**

## Test method and evidence

[DistanceFieldIntegrationTests.inl](../../ZenSamples/CommonTest/DistanceFieldIntegrationTests.inl) uploads exact analytic unsigned plane/box distances from the CPU to isolate native traversal. CPU distance construction exists only in this fixture. It bypasses GPU distance emission, propagation/Eikonal, alpha coverage, large-triangle/reference limits, allocation pressure, and scene update/lifetime behavior. It is an optimistic input-quality experiment, not a renderer-quality comparison.

The five tests cover planes from both sides, one-sided normals, finite segments, oblique rays, outside-origin slab entry, six box faces, an external-cache blocker, empty scenes, unknown leaves, the per-ray iteration limit, and the per-brick sample limit. The latter two must report unavailable rather than a miss. Separate quality experiments record failures against analytic geometry without turning those measurements into a passing preset decision.

Release ran on the NVIDIA GeForce RTX 5080 with hardware ray-query support disabled by the fixture. All **20/20 tests** passed over inline/threaded submission and disabled/automatic async compute. All **240 recorded quality properties per mode are identical** across modes 0–3. The validation log contains no `VUID-` or `Validation Error` messages and reports no tracked memory leaks. Total suite wall time was 11.9 seconds; this is not a GPU frame-time measurement.

The original five-test suite also passed **20/20 in Debug**, in 82.658 seconds. Every recorded quality property matches Release in every mode; the Debug log also has no `VUID-` or `Validation Error` messages and reports no tracked leaks. This Debug run precedes the separate finer-detail experiment. The Debug build used the repository's `_CL_=/Z7` workaround for the local MSVC debug-information tool mismatch.

- [All-mode XML](../../build/compute-gi/brixelizer-execution/field-gpu.xml) and [log](../../build/compute-gi/brixelizer-execution/field-gpu.log).
- [Original Debug all-mode XML](../../build/compute-gi/brixelizer-execution/field-gpu-debug.xml), [log](../../build/compute-gi/brixelizer-execution/field-gpu-debug.log), and [build command](../../build/compute-gi/brixelizer-execution/build-field-tests-debug.cmd).
- [Initial mode-0 XML](../../build/compute-gi/brixelizer-execution/field-gpu-mode0.xml), [log](../../build/compute-gi/brixelizer-execution/field-gpu-mode0.log), and [aggregated numbers](../../build/compute-gi/brixelizer-execution/field-gpu-mode0-summary.json).
- [Release build command](../../build/compute-gi/brixelizer-execution/build-field-tests.cmd) and [successful build log](../../build/compute-gi/brixelizer-execution/build-field-tests-retry.log). An [initial compile failure](../../build/compute-gi/brixelizer-execution/build-field-tests.log) used the unsupported two-argument `HeapVector::resize`; it was corrected before these runs.

Reproduce from the repository root after initializing the Visual Studio x64 build environment:

```powershell
cmake --build build/x64-windows-msvc-release --target ConeVoxelGIIntegrationTest -j 2
& build/x64-windows-msvc-release/bin/ConeVoxelGIIntegrationTest.exe `
    '--gtest_filter=*DistanceFieldIntegrationTest.*-*FinerLocalSlitExperiment*' `
    '--gtest_output=xml:build/compute-gi/brixelizer-execution/field-gpu.xml' `
    *> build/compute-gi/brixelizer-execution/field-gpu.log
```

The negative filter preserves the original five-test scope after the finer test was added. Use `*DistanceFieldIntegrationTest.*/0-*FinerLocalSlitExperiment*` for its mode 0 only. The retained runs disabled the injected Nsight GPU Trace layer through `VK_LOADER_LAYERS_DISABLE`; they used the test's Vulkan validation messenger. The commands overwrite the indicated evidence files, so choose another output name when retaining a comparison run.

## Slit measurements

The fixture is a 5 cm slit in a 3.571 cm thick wall inside a four-metre field. Each resolution tests independent wall-depth and aperture phases of `0`, `0.25`, `0.5`, and `0.75` sample intervals: 16 phase pairs. Each pair traces 108 rays: both approaches; 0°, 30°, and 60° incidence; nine aperture offsets from -4 cm to +4 cm in 1 cm increments; and 0/2 mm origin offsets along the ray. Analytic box intersections independently classify the finite segments.

Each table row aggregates one submission mode across all 16 phases: **1,728 rays, of which 1,216 are blocked and 512 are open analytically**. A leak is a covered miss on a blocked ray; an overblock is a hit on an open ray. The four submission modes reproduce the same counts, so they are not counted as extra independent geometric samples.

| Sample spacing `h` | Epsilon in brick units | Threshold / `h` | Leaks / 1,216 | Overblocks / 512 | Unavailable / 1,728 |
| --- | --- | --- | --- | --- | --- |
| 7.143 cm | `1/255` | 0.0549 | 914 (75.16%) | 80 (15.62%) | 0 |
| 7.143 cm | `0.125/7` | 0.25 | 234 (19.24%) | 320 (62.50%) | 0 |
| 7.143 cm | `0.25/7` | 0.5 | 0 | 512 (100%) | 0 |
| 7.143 cm | `0.5/7` | 1.0 | 0 | 512 (100%) | 0 |
| 3.571 cm | `1/255` | 0.0549 | 1,004 (82.57%) | 0 | 0 |
| 3.571 cm | `0.125/7` | 0.25 | 308 (25.33%) | 216 (42.19%) | 0 |
| 3.571 cm | `0.25/7` | 0.5 | 0 | 432 (84.38%) | 0 |
| 3.571 cm | `0.5/7` | 1.0 | 0 | 512 (100%) | 0 |
| 1.786 cm | `1/255` | 0.0549 | 948 (77.96%) | 0 | 0 |
| 1.786 cm | `0.125/7` | 0.25 | 410 (33.72%) | 0 | 0 |
| 1.786 cm | `0.25/7` | 0.5 | 0 | 200 (39.06%) | 0 |
| 1.786 cm | `0.5/7` | 1.0 | 0 | 464 (90.62%) | 0 |

The finest tested zero-leak setting still blocks 39.06% of genuinely open rays. Reducing its tolerance to preserve all open rays leaks 33.72% of blocked rays. No unavailable results occur in this matrix, so these errors cannot be attributed to the tracer honestly declining incomplete traversal. Higher spatial detail or another qualified field representation is still required; changing only tolerance does not satisfy the current geometry gate.

The separate [reference-math CPU study](BrixelizerGIReferenceMathExperiment.md) also finds a detail/tolerance tradeoff and explores finer spacing. Its slot widths, wall thickness, local sample limit, and ray metrics differ, so its percentages are not direct GPU comparisons and are not pooled into this table.

## Shifted-plane measurements

At each resolution, a plane moves through four sample phases. Two axial approaches and one oblique ray yield 12 analytically blocked rays per row. Counts are the same at all three resolutions. Error below is the maximum absolute world-distance error **among reported hits only**; it excludes misses and must not be read as accuracy for all rays.

| Epsilon | Hits / 12 at each resolution | Covered misses / 12 | Max hit error at `h=7.143 cm` | At `h=3.571 cm` | At `h=1.786 cm` |
| --- | --- | --- | --- | --- | --- |
| `1/255` | 3 | 9 | 0.2242 cm | 0.1121 cm | 0.0560 cm |
| `0.125/7` | 6 | 6 | 1.8211 cm | 0.9105 cm | 0.4553 cm |
| `0.25/7` | 12 | 0 | 3.6422 cm | 1.8211 cm | 0.9105 cm |
| `0.5/7` | 12 | 0 | 6.3827 cm | 3.1913 cm | 1.5957 cm |

There are no unavailable plane results. The `1/255` setting only reliably hits the grid-aligned phase in this experiment. Treating R8 quantization error as the whole tolerance requirement misses interpolation error and causes opaque-plane leaks.

## Targeted finer native experiment

`FinerLocalSlitExperiment` reuses the same analytic geometry, ray oracle, and 108 rays per phase. It narrows the matrix to two resolutions and two tolerances, with four crossed wall/aperture phase pairs: `(0,0)`, `(0.25,0.5)`, `(0.5,0.25)`, and `(0.75,0.75)` sample intervals. Each row therefore contains **432 rays: 304 blocked and 128 open**. This subset does not replace the original 16-pair experiment or establish behavior at every phase/orientation.

| Sample spacing | Epsilon | Leaks / 304 | Overblocks / 128 | Unavailable / 432 | Distance samples |
| --- | --- | --- | --- | --- | --- |
| 8.929 mm | `0.125/7` | 120 (39.47%) | 0 | 0 | 8,228 |
| 8.929 mm | `0.25/7` | 0 | 4 (3.125%) | 0 | 5,752 |
| 4.464 mm | `0.125/7` | 92 (30.26%) | 0 | 0 | 8,373 |
| 4.464 mm | `0.25/7` | 0 | 0 | 0 | 5,888 |

The 4.464 mm / `0.25/7` pair correctly classifies all sampled rays. This is evidence that sufficient local detail can resolve this particular wall/slit fixture with the native tracer. It is not a production preset, a proof for arbitrary rotated surfaces, or a lantern/alpha/lighting acceptance result. Finer rays near aperture edges and the remaining phase combinations still require qualification.

The initial finer run allocated the following resource content sizes; counts are identical across the four chosen phases. That run used a cubic power-of-two tile allocation, so unused tiles count toward its size. The later compact-packing experiment changes that allocation without changing sample spacing.

| Grid cells per axis | Allocated bricks | R8 atlas bytes | Cell-map bytes | Hierarchy bytes | Total field bytes |
| --- | --- | --- | --- | --- | --- |
| 64 | 16,384 | 16,777,216 | 1,048,576 | 1,198,372 | 19,024,164 (18.143 MiB) |
| 128 | 81,920 | 134,217,728 | 8,388,608 | 9,586,980 | 152,193,316 (145.143 MiB) |

These are explicit image/buffer sizes for an ideal four-metre fixture, not measured Vulkan heap residency or a full-scene memory budget. They exclude allocation alignment, the CPU distance array, its equally sized upload buffer, small ray/result buffers, production builder scratch, nested levels, retirement, and frames in flight. They do not qualify the plan's 256 MiB persistent / 128 MiB scratch targets for Sponza.

The fixture now releases each dispatch's graph references and resources after its existing readback wait, and collects completed retirements. It preserves any resources owned before that dispatch; no production waits were added. This avoids retaining all 128 MiB atlases until test teardown. The finer Release mode-0 test passed in 3.589 seconds with no `VUID-` or `Validation Error` messages and a clean tracked-memory report. That duration includes CPU exact-distance generation and uploads and is not a GPU tracing benchmark.

Evidence: [finer XML](../../build/compute-gi/brixelizer-execution/field-gpu-fine.xml), [log](../../build/compute-gi/brixelizer-execution/field-gpu-fine.log), [aggregate/storage JSON](../../build/compute-gi/brixelizer-execution/field-gpu-fine-summary.json), and [build log](../../build/compute-gi/brixelizer-execution/build-field-fine.log). Reproduce after building Release with `--gtest_filter=*DistanceFieldIntegrationTest.FinerLocalSlitExperiment/0`, and use separate output filenames to retain the original coarse evidence.

## Compact atlas packing

The next run replaces power-of-two tile-side rounding with the smallest cubic tile side whose capacity contains the allocated brick count. Atlas texel dimensions remain multiples of eight. Sample coordinates, R8 contents, field spacing, and the tracing ABI are unchanged. Each finer fixture records both its current storage and the previous power-of-two atlas size.

| Grid cells per axis | Atlas texel side | R8 atlas bytes | Cell-map bytes | Hierarchy bytes | Compact total | Previous total |
| --- | --- | --- | --- | --- | --- | --- |
| 64 | 208 | 8,998,912 | 1,048,576 | 1,198,372 | 11,245,860 (10.725 MiB) | 19,024,164 (18.143 MiB) |
| 128 | 352 | 43,614,208 | 8,388,608 | 9,586,980 | 61,589,796 (58.737 MiB) | 152,193,316 (145.143 MiB) |

This saves 7.418 MiB and 86.406 MiB respectively in the standalone fixture's field resource sizes. The same exclusions from the previous memory table apply; these are not full-scene residency or production scratch measurements.

All **24/24 Release tests** pass with compact atlases across all four submission modes in 21.804 seconds, including the finer experiment and the resource-release change. Every property's value matches between submission modes. The original coarse hit/miss/unavailable counts and plane errors are unchanged. One coarse row (`side=16`, wall phase `0.25`, aperture phase `0.75`, epsilon `1/255`) takes two extra distance samples per mode, changing its sample count from 2,568 to 2,570; no quality classification changes. All finer quality properties, including sample counts, match the initial power-of-two run exactly. The 4.464 mm / `0.25/7` pair therefore retains zero sampled leaks, overblocks, and unavailable results with the 58.737 MiB compact fixture field.

The compact validation log has no `VUID-` or `Validation Error` messages and reports no tracked memory leaks. Evidence: [24-test XML](../../build/compute-gi/brixelizer-execution/field-gpu-compact.xml), [log](../../build/compute-gi/brixelizer-execution/field-gpu-compact.log), and [comparison/storage JSON](../../build/compute-gi/brixelizer-execution/field-gpu-compact-summary.json). Reproduce all six tests with `--gtest_filter=*DistanceFieldIntegrationTest.*` after building Release, writing a separate XML/log. Debug evidence above applies to the original five-test, power-of-two-atlas run; the compact/finer extension has only been qualified in Release so far.

## Normal and work-counter correction

The first GPU-built slanted plane produced hits at the expected distances but a normal whose dot product with the ray was `+0.0118225`, failing the incoming-facing normal assertion in all four modes. The approach-side gradient correction described above makes those four cases pass without changing the assertion. The accompanying builder run still contains four unrelated publication-budget failures after the owner began reporting actual native image allocation sizes; it is not recorded as a fully passing builder suite. Evidence: [builder XML](../../build/compute-gi/brixelizer-execution/m2-gpu-normal.xml).

All **24/24 isolated trace tests** pass after this correction in **23.336 seconds**. Across the four modes, 204 of 1,056 recorded properties changed relative to compact-atlas results, exclusively through distance-sample counts. Statuses, hit-position errors, leaks, overblocks, unavailable counts, and storage properties are unchanged, and every property still agrees between modes. The 4.464 mm / `0.25/7` candidate remains at zero sampled leaks, overblocks, and unavailable results, using 5,888 distance samples. Evidence: [trace XML](../../build/compute-gi/brixelizer-execution/field-gpu-normal.xml), [log](../../build/compute-gi/brixelizer-execution/field-gpu-normal.log), and [comparison JSON](../../build/compute-gi/brixelizer-execution/field-gpu-normal-summary.json). These are fixture measurements, not a whole-scene normal or performance qualification.

## Actual New Sponza asset inspection

The inspected source is [NewSponza_Main_glTF_003.gltf](E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf), with source SHA-256 `e04c4c540c74bdddcbd3f590a85c14119bfd5839702246a23b3a737c0cce2400` and binary-buffer SHA-256 `903d443b46b51c8397c25b65286d9f9e111121f12a3e0e303f19019982568d6b`. The inspection uses `load_gltf_primitives` from the independent reference tool and direct point-to-triangle distances; it does not run a GPU renderer or add a runtime CPU triangle structure.

### Deterministic lantern selection

Use authored point light **8**, node **133**, at `(-9.82472, 4.17139149, -4.806306)` in source metres. Evaluate actual triangle proximity rather than selecting by a mesh name. Its three closest primitive surfaces all belong to **mesh 31 / node 34**, with primitive indices `2`, `1`, and `0` respectively:

| Primitive | Material | Triangles | Nearest distance from light |
| --- | --- | --- | --- |
| 2 | 12, `light_bulb` | 368 | 2.920 cm |
| 1 | 11, `lamp_glass_01` | 8 | 8.666 cm |
| 0 | 2, `metal_door` | 5,586 | 9.479 cm |

The selected mesh has **5,962 triangles** and authored bounds `(-10.045914718, 3.838130569, -5.015070417)` to `(-9.604859308, 4.823771121, -4.574013621)`: approximately **0.4411 × 0.9856 × 0.4411 m**. The closest other opaque scene triangle is ceiling material 4, **64.232 cm** from the light. This separation was established by exact distances for every primitive with an AABB lower bound below one metre; all remaining primitives have a lower bound above that threshold. A 25 cm sphere around the light therefore isolates the lantern's nearby surfaces, while the full selected mesh must be retained for complete fixture coverage.

The glazing is authored as **OPAQUE and double-sided**, despite its name. It comprises four single-layer quads, each two nearly coplanar triangles, not a volumetric transparent pane. Their normals have vertical component about `-0.11648` and substantial components in both horizontal axes; this is actual oblique, effectively zero-thickness geometry. It must remain an occluder under the existing ray-query contract. The bulb is also opaque and emissive. Do not remove either part when measuring aperture visibility. This inspection establishes robust geometry/region selection and material semantics; it does **not** claim a measured global minimum opening width or metal-wall thickness.

The full scene normalizes with center `(1.877865, 8.813047205, 2.480922530)` and scale `0.0275885817387` (longest authored extent `36.246879578 m`). The selected light becomes `(-0.322857723, -0.128056698, -0.201044300)`, agreeing with the retained `compute-probes.lighting.json` light 8 to less than `1.6e-8` normalized units. Normalizing the **isolated** lantern instead uses center `(-9.825387013, 4.330950845, -4.794542019)` and scale `1.014568645669`; its canonical local light is approximately `(0.00067673, -0.16188392, -0.01193537)`. Tests must use the matching normalization rather than inserting the full-scene light coordinates into the isolated fixture.

Reproducible, ignored inspection artifacts are [inspection JSON](../../build/compute-gi/brixelizer-execution/newsponza-lantern/inspection.json), [selected triangle NPZ](../../build/compute-gi/brixelizer-execution/newsponza-lantern/lantern-triangles.npz), and [uniform lantern glTF](../../build/compute-gi/brixelizer-execution/newsponza-lantern/lantern-uniform.gltf). The latter retains the source geometry/world transform and the three materials' occlusion semantics, strips irrelevant texture loading, and references the original local binary buffer; it is not a portable redistributed asset. The durable extractor [tools/extract_newsponza_lantern.py](../../tools/extract_newsponza_lantern.py) verifies the source and binary hashes, performs proximity selection, and verifies the canonical native-fixture hash. It uses NumPy and Pillow through the existing glTF reference loader, with no renderer or GPU execution:

```powershell
build/venv/Scripts/python.exe tools/extract_newsponza_lantern.py E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf
```

An independent rerun to `--output build/compute-gi/brixelizer-execution/newsponza-lantern-reproduced` reproduced the native binary hash exactly. The extractor requires the original external asset and writes only test inputs and inspection evidence, never production field samples.

### Actual alpha coverage and required partial-mask behavior

New Sponza has **28 materials: 27 OPAQUE, one BLEND, and no MASK materials**. The only nonopaque material is index 21, `dirt_decal`, referenced by four primitive instances totaling 8,140 triangles. Its base factor alpha is `0.350000024`, every vertex alpha is exactly `1`, and its 4096×4096 texture alpha spans `0..1`. Its maximum possible coverage is therefore `0.350000024`, strictly below the blend visibility cutoff `0.5`. It is fully nonoccluding under the current GI contract, even though the texture itself varies. The uniform reference scene preserves this factor after removing textures, so it remains nonoccluding there too.

`MaterialOccludes` treats opaque materials as blockers and tests MASK/BLEND coverage for the others. The general software-query baseline calls `RayCandidateAccepted`, including transformed level-zero texture alpha and interpolated vertex alpha; the legacy near-lamp opaque retrace deliberately bypasses that alpha test. There are **no partially alpha-covered occluders in this New Sponza source** to qualify a general partial-mask replacement. Its fully transparent decals can be rejected by a proven upper bound, while a separate checkerboard/cutout fixture must test partial coverage, occluded/off-screen cards, transforms, and sampler addressing.

A safe field-construction strategy is bounded GPU subdivision of a triangle into barycentric patches, retaining the union of covered microtriangles as field sources:

1. Bound vertex alpha over each patch using its barycentric vertices. Bound texture alpha over every level-zero bilinear footprint touched by the transformed patch UV region, including repeat/mirror/clamp behavior and the material's spatial-atlas mapping.
2. Discard a patch only when its **maximum** possible coverage is below the cutoff; accept it without further coverage subdivision only when its **minimum** is above the cutoff. Sampling alpha only at the nearest point and dropping the entire triangle is unsafe: it can overestimate distance past a nearby opaque texel.
3. Subdivide uncertain patches within bounded jobs. Conservatively retain an uncertain terminal patch only once its world diameter meets the chosen coverage-error bound, such as a fraction of field sample spacing, **and an actual covered witness bounds its distance to covered geometry**. A witness inside the patch suffices. A nearby witness on the original triangle also suffices when the patch radius plus center-to-witness distance remains within the same error budget. A small patch whose broad texture bounds merely overlap an opaque texel has no proven distance to covered geometry; its small diameter alone does not bound dilation. Without a witness, refine the bounds/subdivision or report unavailable when the work limit is reached. Qualify the resulting dilation with actual cutout tests. Never silently publish uncertain geometry empty.
4. Emit distance to the retained microgeometry with the ordinary field-distance path. This changes field construction, not the runtime tracer into a local triangle fallback. Coverage-changing texture/material edits must invalidate the associated field regions.

The existing bindless texture/sampler indices support `textureGather(..., 3)` for raw four-texel alpha bounds while retaining sampler addressing. Gather uses the base image level and selects texels using the linear footprint; it avoids assuming a filtered sample at a texel center is an exact extremum. [GLSL 4.60, texture gather functions](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html#texture-gather-functions). Raw texel bounds alone can remain uncertain just outside a coverage edge: linear filtering therefore needs tighter bounds evaluated on each patch-rectangle/bilinear-cell intersection, while nearest filtering needs its own support rule. Oversized UV footprints can remain uncertain and subdivide instead of performing unbounded texture scans. This is the construction design being coordinated with the builder work, not a completed alpha qualification result.

The native lantern input is [lantern-native.bin](../../build/compute-gi/brixelizer-execution/newsponza-lantern/lantern-native.bin), 214,736 bytes, SHA-256 `2c3132aa5ce7a9f6de9cbd73f7a89444051e72deafb297b85a2bc4af7fb6342d`. It records the original material alpha modes and expanded canonical triangle positions; it contains no field samples. `DistanceFieldActualLanternVisibilityExperiment` uses this input through `ZEN_FIELD_LANTERN_PATH`, computes its direct double-precision triangle oracle before uniform material remapping, and asks the GPU builder for each field. Its four poses combine identity/a 23-degree yaw plus 17-degree roll with zero/a fractional-sample translation. Rays include 1,024 directions from the canonical light, both approaches to sampled metal/bulb triangles, and both approaches to all eight opaque glazing triangles. The optional `ZEN_FIELD_LANTERN_EPSILON` changes the measured trace tolerance in brick units and is recorded in XML; it does not select a production preset.

The first mode-zero lantern pilot completed all four fields, each with 1,602 analytically blocked and 64 open rays. Every pose reported zero leaks and two overblocks; unavailable counts were `2,2,1,0`, and zero-normal hits were `3,1,5,3`. Maximum first-hit distance errors were `0.153330,0.154002,0.154559,0.154565 m`. These results expose quality issues despite API execution passing. They also uncovered a fixture setup error: `RenderScene` recomputes source bounds during construction, overwriting the preconstruction cache AABB. The observed spacing was approximately 2.271 mm, and translation mostly moved the field with the geometry. This run therefore does **not** establish fixed-grid phase robustness. Evidence: [initial lantern XML](../../build/compute-gi/brixelizer-execution/m2-lantern-initial.xml).

The fixture now calls `SetVoxelBounds([-0.7,0.7])` after constructing `RenderScene`, checks the committed cache bounds, and asserts spacing `1.4/(62*7)` canonical units at side 64. This is **3.226 mm in canonical units and 3.179 mm in source metres**. It records bounded mismatch details and the worst-distance ray to distinguish grazing thick-field hits, tracing exhaustion, and incorrect construction. The oblique-normal and alpha fixtures already publish their fixed bounds after construction and now all assert the resulting spacing.

The corrected mode-zero run passes API execution and fixed-spacing assertions, but remains an unqualified quality result. Each pose has the same 1,602 blocked and 64 open oracle rays:

| Pose | Leaks | Overblocks | Unavailable | Zero-normal hits | Maximum ray-distance error |
| --- | --- | --- | --- | --- | --- |
| 0 | 0 | 3 | 2 | 5 | 155.441 mm |
| 1 | 0 | 3 | 1 | 6 | 154.875 mm |
| 2 | 0 | 2 | 1 | 4 | 154.341 mm |
| 3 | 0 | 2 | 0 | 3 | 155.357 mm |

Every field uses 128 construction graphs and reports 19,024,176 persistent allocation bytes. Evidence: [fixed-grid XML](../../build/compute-gi/brixelizer-execution/m2-lantern-fixed.xml), [log](../../build/compute-gi/brixelizer-execution/m2-lantern-fixed.log), and [per-ray geometry analysis](../../build/compute-gi/brixelizer-execution/m2-lantern-fixed.ray-analysis.json). The reported construction milliseconds include the test helper's CPU work and completion waits; they are not GPU frame-time measurements.

Reconstructing the deterministic rays from the pinned float32 triangles explains the worst-distance case. Ray 119 passes the bulb with approximately **0.122 mm** minimum clearance, but intersects metal at distance `0.201048` canonical units. Its field hit occurs near the bulb at approximately `0.043–0.044`, with the reported hit point about **1.00–1.24 mm** from the bulb. The long ray-distance discrepancy is consistent with accepting a grazing surface inside the field's distance band; it does not by itself show corrupt distance construction. The three overblocked open rays, 962/987/996, have approximately **0.466/0.637/1.531 mm** minimum clearance to metal, compared with the current effective hit band of **1.589 mm**. Lower tolerances must be measured alongside leaks. These are sensitivity measurements, not a requirement to reproduce every submillimetre grazing intersection exactly: acceptance still depends on opaque-wall leakage, viable lantern lighting, and the full-GI reference-bias limits in the plan.

The unfinished open rays have only 126–146 work operations and 9–15 texture samples, below either trace cap. Rays 1006 and 1020 clear the geometry by approximately **5.55 and 8.64 mm**. A diagnostic reason distinguishes invalid query/layout, global cap, missing data, node-progress failure, invalid storage, and brick cap through the calibration output's previously unused normal `w` lane, preserving the 48-byte buffer layout. The [scheduler/alpha run](../../build/compute-gi/brixelizer-execution/m2-alpha-scheduler.xml) confirms all four unfinished lantern rays have **reason 4, node progress**, with unchanged quality counts. Advancing the ray parameter by one ULP can still reconstruct a position behind the crossed face. The correction selects a cell using its half-open per-axis entry/exit times and advances to the exact node exit; the new synthetic regression checks both travel directions, then unknown and opaque successor cells.

After the correction, all four lantern poses have **zero unavailable results**, with zero leaks and unchanged overblocks `3,3,2,2`, zero-normal hits `5,6,4,3`, and worst-distance discrepancies. Across 6,664 rays, the run uses 80,792 distance samples and 425,071 counted work operations. The accompanying GPU scheduler changes reduce construction submissions from 128 to 31 graphs per pose; neither graph counts nor the test's completion-wait elapsed time establish GPU frame cost. Evidence: [coverage/progress XML](../../build/compute-gi/brixelizer-execution/m2-coverage-progress.xml). The isolated suite passes **28/28 in 24.622 seconds**, including successor-data preservation, and all **32/32 builder/normal cases pass in 8.873 seconds**. All four submission modes agree. Of the 1,056 common isolated properties, 64 change only through plane-distance rounding up to 0.000001 fixture units or slit sample counts; no classification changes. The finer `0.25/7` candidate remains leak/overblock/unavailable-free and uses 5,889 samples. Evidence: [isolated XML](../../build/compute-gi/brixelizer-execution/field-cell-progress.xml), [builder XML](../../build/compute-gi/brixelizer-execution/m2-gpu-progress.xml), and [property comparison](../../build/compute-gi/brixelizer-execution/field-cell-progress-summary.json).

### Fixed-grid lantern tolerance sweep

Two further mode-zero runs change only `ZEN_FIELD_LANTERN_EPSILON`, using the same side-64 grid, four poses, 6,408 blocked rays, and 256 open rays. They use the pre-diagnostic tracer and pass API execution; their recorded leakage is a quality failure, not a passing preset result.

| Epsilon / brick extent | Effective hit band | Leaks / 6,408 blocked | Overblocks / 256 open | Unavailable / 6,664 | Distance samples | Work operations |
| --- | --- | --- | --- | --- | --- | --- |
| `0.25/7` | 1.589 mm | 0 | 10 | 4 | 80,779 | 424,968 |
| `0.1875/7` | 1.192 mm | 831 (12.97%) | 4 | 4 | 97,144 | 496,405 |
| `0.125/7` | 0.795 mm | 2,686 (41.92%) | 1 | 4 | 126,647 | 625,206 |

The smaller thresholds remove only six or nine grazing overblocks while introducing many opaque-surface leaks and more sampling work. The same four unfinished rays persist at every tolerance, supporting a separate traversal-progress investigation. Retain **3.179 mm source-space spacing with `0.25/7`** as the measured local lantern candidate for subsequent lighting comparison, subject to resolving unfinished rays and completing scene-coverage, alpha, and full-GI reference checks. This does not establish a full-scene resolution preset. Evidence: [middle-tolerance XML](../../build/compute-gi/brixelizer-execution/m2-lantern-epsilon1875.xml), [lower-tolerance XML](../../build/compute-gi/brixelizer-execution/m2-lantern-epsilon125.xml), and [aggregate JSON](../../build/compute-gi/brixelizer-execution/m2-lantern-epsilon-summary.json).

`DistanceFieldShiftedObliqueNormals` adds eight GPU-built plane fields, spanning two orientations and four fractional-sample phases at fixed visibility bounds. Each field checks nine surface positions from both approaches, including an opposing-normal assertion below `-0.8`; it does not weaken the original normal gate. All four submission modes pass this test, within the **20/20 passing builder/normal run** after the publication-budget fixture was corrected for native allocation extents. Evidence: [diagnostic XML](../../build/compute-gi/brixelizer-execution/m2-gpu-diagnostic.xml).

Two explicitly disabled tests in the same native fixture exercise partial-alpha construction. `DISABLED_DistanceFieldPartialVertexAlphaExperiment` tests four transformed/shifted cards against a double-precision triangle and barycentric-alpha reference, with 578 rays per pose. `DISABLED_DistanceFieldTextureMaskExperiment` uses an 8×8 mask with a 4×4 hole, nearest/linear magnification filters, repeat/mirror/clamp addressing, affine UV transforms, and oblique geometry, with 2,178 rays per filter/address pair. It deliberately gives the sampler a different minification filter to exercise the explicit-LOD-zero contract. Both retain dense edge measurements and separate clear/opaque control assertions; failed or unavailable construction is a failed gate. They require an explicit matching filter plus `--gtest_also_run_disabled_tests`; their current native evidence is reported below.

The texture-mask CPU reference uses the Vulkan normalized-coordinate rules: nearest selects `floor(8*uv)`, linear interpolates around `8*uv-0.5`, and integer coordinates then repeat, mirror, or clamp. `RenderScene::LoadSceneTextures` creates this fixture's sampler with zero LOD bias, zero minimum/maximum LOD, normalized coordinates, and no anisotropy. Explicit shader LOD zero therefore selects `magFilter`, even when `minFilter` differs. The reference implements ideal double-precision filtering; native linear fractions have `subTexelPrecisionBits` precision, so dense samples close to alpha `0.5` are not a bit-exact hardware-alpha oracle. Core assertions avoid the cutoff and texel boundaries. These rules follow the [Vulkan sampling specification](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/chapters/textures.adoc), especially LOD operation, texel-coordinate conversion, wrapping, and texel filtering.

Peer review corrected two independent conservative-bound errors before native alpha qualification. Bounding UV arithmetic from only the final transformed coordinate missed cancellation: constant raw `u=2,000,000` with an affine offset of `-2,000,000` transforms patch corners to zero, while float32 weighted interpolation at valid interior barycentrics can produce `u=0.125` and select a different nearest texel. The builder now expands UV intervals from original UV and affine-operation magnitudes before atlas remapping. Separately, constant vertex alpha `nextafter(0.5,0)` can round to `0.5` during weighted accumulation; an unguarded early maximum could discard the entire triangle. Both the early maximum and every patch interval now use outward error based on the original vertex-alpha magnitude, including constant inputs. Cutoff-ambiguous coverage remains unresolved or fails its budget instead of being silently published empty. These are construction-proof corrections, distinct from the accepted field-distance band and the ideal CPU sampler's near-cutoff measurement limitations. The explicit partial-alpha tests use a 1,024-reference per-cell test budget and the default 1,048,576 total reference capacity; production defaults remain unchanged.

The [CPU arithmetic regression](../../tools/test_distance_field_coverage_arithmetic.py) reproduces both previously unsafe bounds with separate and fused float32 accumulation and checks 20,005 barycentrics per case/mode. `DISABLED_DistanceFieldCoverageArithmeticRegression` carries the two witnesses into a native field fixture: a ready field must hit from both approaches, while a failed field must report only unsupported coverage or coverage-refinement exhaustion (flags 32/128). Unrelated allocation/geometry failures cannot satisfy this safety regression. Both native cases pass by reporting explicit **coverage-refinement failure 128**, each after two graphs; neither publishes empty geometry. This establishes the safe failure contract, not support for those adversarial inputs.

The first partial-alpha execution failed explicitly: the vertex card exhausted the smaller fixture's 16,384 total-reference cap, and the first texture-mask case exhausted coverage refinement. With the default total cap, all four vertex-card control assertions pass at 40.816 mm sample spacing, each after eight construction graphs. Dense edge measurements remain visible:

| Vertex-card pose | Blocked | Open | Leaks | Overblocks | Unavailable |
| --- | --- | --- | --- | --- | --- |
| 0 | 152 | 426 | 0 | 10 | 0 |
| 1 | 138 | 440 | 0 | 20 | 0 |
| 2 | 162 | 416 | 1 | 0 | 0 |
| 3 | 145 | 433 | 2 | 13 | 0 |

The three dense-edge leaks lie outside the hard clear/opaque controls; passing those controls does not erase the measurements or qualify every alpha boundary. The texture construction's geometrically bounded nearby-witness search still reaches its 8,192-node work limit on the first nearest/repeat mask case (diagnostic site 7, triangle 0, patch 296232). Textured partial coverage therefore remains unqualified. Both tables and safety failures come from the [coverage/progress run](../../build/compute-gi/brixelizer-execution/m2-coverage-progress.xml), whose overall result is **four passing tests and one failing texture-mask test**, not a fully passing alpha suite.

For bounded visibility inspection, set `ZEN_FIELD_LANTERN_TRACE_PATH` to an output prefix on the native lantern test. It writes four CSVs per selected execution mode containing all 1,666 actual rays, oracle distances, field hit positions/normals, statuses, failure reasons, and work counters. This reuses existing readback and adds no GPU pass or wait. Run `tools/plot_distance_field_lantern.py <lantern-native.bin> <prefix> --mode 0 --output <plot-prefix>` with NumPy and Matplotlib to create PNG/SVG diagnostics. The plots show light-direction visibility and mismatch endpoints against authored geometry; they do not replace a production distance-atlas/normal view or rendered-GI reference comparison.

The [continuation run](../../build/compute-gi/brixelizer-execution/m2-alpha-continuation.xml) exports these actual rays. Its [PNG diagnostic](../../build/compute-gi/brixelizer-execution/lantern-visibility-diagnostic.png) and [SVG diagnostic](../../build/compute-gi/brixelizer-execution/lantern-visibility-diagnostic.svg) were rendered and visually inspected. The four rows show the canonical rotation/translation poses, with angular visibility from the light on the left and source geometry projections in the other columns. Orange rays terminate near the lower aperture: the overblock counts remain `3,3,2,2`, with no leaks or unavailable rays. Brown rings preserve the `5,6,4,3` zero-normal hits. Blue dotted segments distinguish the grazing bulb hit from the later exact metal intersection; a large first-intersection separation is not automatically a visibility leak. Coordinates use source metres about the isolated fixture origin. The CSVs, not reconstructed CPU field samples, supply every displayed field result.

The same run passes five of six tests in 12.292 seconds, including unchanged lantern and vertex-alpha measurements and the expected explicit arithmetic-coverage failures. Texture refinement continuation now advances beyond the former 8,192-node stopping point, but the first nearest/repeat mask case still fails explicitly with **per-cell reference overflow 8 after 297 graphs**. This is progress in bounded construction, not a passing textured-alpha gate. The earlier work-limit failure above describes its corresponding checkpoint; neither failure is published as empty space.

An additional opt-in, `ZEN_FIELD_LANTERN_FIELD_PATH=<prefix>`, exports reconstructed field slices after each lantern field reaches `Ready`. `Calibration/distance_field_sample.comp` samples the actual R8 atlas through `DistanceFieldSample`, with the same tile coordinates and trilinear sampler as the tracer. It exports front XY and side ZY planes through the transformed light, each at 256×512 points. Each pose writes a 12 MiB binary plus JSON metadata: actual sample positions/distances in source metres, storage state and brick ID, raw field gradients, grid extent, sample spacing, tolerance, physical normalization and the exact fixture transform. Empty cells and outside-level points have no stored distance and remain separate states; neither receives an analytic distance estimate. Raw brick-local gradients are explicitly unoriented, distinct from the approaching-side normals used for trace hits.

The hook adds no pass, allocation or wait when its environment variable is absent. Its diagnostic readback uses the existing native test completion boundary, not production waits. `DistanceFieldDiagnosticSamplesReadConstructedAtlas` separately checks actual plane samples and explicit outside-domain state. The plotting tool overlays source-triangle/slice intersections and can add the earlier ray readback as explicitly projected rays:

```powershell
build/venv/Scripts/python.exe tools/plot_distance_field_slices.py <lantern-native.bin> <field-prefix> --mode 0 --trace-prefix <trace-prefix> --gradients --output <plot-prefix>
```

This field-slice path is prepared for the next coordinated native checkpoint; a generated image is not yet claimed here. It complements the existing ray-outcome plots and does not substitute for the final rendered-GI reference comparison.

## Remaining qualification

The passing API tests establish the implemented trace/status/resource contracts for these fixtures. The quality measurements explicitly leave the implementation gate open. Next evidence must cover sufficient local detail at arbitrary geometry phases/orientations, actual lantern geometry, material-aware alpha coverage, GPU construction/propagation, and the full-scene/cache consumer contracts. Production integration and the final removal of the CPU BVH remain required by [the implementation plan](BrixelizerGIImplementationPlan.md); this experiment does not replace them or justify weakening its quality, memory, and construction budgets.
