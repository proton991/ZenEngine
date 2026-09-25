# Voxelization calibration — V0 implementation record

Date: 2026-09-23. **V0 passed for the calibrated contract and device below.** This is the V0-stage record; subsequent progress is tracked in the [implementation plan](DynamicVoxelGIImplementationPlan.md). No DDA provider or RHI ray-tracing support was added by V0. This report certifies boundary voxelization and the explicitly limited material sampling rule, not triangle-accurate visibility or exact alpha-footprint coverage.

## Corrections

The initial raw comparison reproduced **384 non-ambiguous missing cells** in geometry voxelization at 64³; compute had none on that fixture. Geometry now rasterizes a bounded projected rectangle and tests the original triangle against candidate cell boxes. Geometry and compute share dominant-axis selection, candidate bounds, triangle-box SAT, depth enumeration, and cell-center surface sampling.

A dominant-axis plane varies by at most two depth units across a projected cell. Its closed interval can touch four depth cells at integer endpoints. Both producers enumerate the depth interval, replacing compute's fixed three candidates. Endpoint guard candidates prevent rounding from dropping exact touches before SAT; the original unexpanded SAT rejects non-intersecting candidates. Projected ranges include exact boundary neighbors and reject wholly outside ranges. There is no occupancy dilation or solid filling. Triangles whose largest grid-space cross-product component is at most `1e-12` are rejected.

Two additional failures were isolated during the expanded calibration:

- Shared-edge SAT dot evaluation produced eight owner disagreements between shader stages in the 64³ material atlas, four affecting attributes. Explicit multiply/add order with `precise` removed these disagreements; marking only a built-in `dot` result precise did not. The change follows GLSL's [precise arithmetic rules](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.4.60.html). Evidence: `materials-precise-probe/`, `materials-dot-probe/`.
- At 256³, five endpoint-touch cells on the long diagonal triangle were absent from geometry's depth candidates. The endpoint guards fixed this; both backends now produce identical records. Evidence: `final-geometry-256/` before this correction and `depth-guard256/` after it. Only `verified-*` directories below represent the final implementation.

Owner clearing no longer depends on a non-null triangle buffer. A zero-triangle input clears every surface output without binding absent scene inputs. Geometry revisions are pending during recording and publish on successful graph handoff; failed handoff requests a retry. Same-graph consumers use the pending revision. Compute visualization invalidates its cached revision on failure. Publication follows the existing graph-handoff contract; it is **not a GPU-completion fence**. Native failure after threaded handoff retains the device's existing blocked-submission behavior.

## Coverage contract and independent oracle

[validate_voxelization.py](../tools/validate_voxelization.py) decodes the actual GPU vertex, index, node, and triangle-record buffers. Double-precision Sutherland–Hodgman clipping against cell boxes is independent of the shader SAT. Automatic normalization is included in captured transforms; comparisons use identical inputs, grid origin, size, resolution, and orientation.

Opaque occupancy means intersection with **closed cell boxes**, including face/edge/corner touches. The numerical band is `1e-5` grid units: shrunk boxes establish certain cells, expanded boxes establish possible cells. Missing means certain-but-unwritten; extra means written-but-not-possible. Nominal unexpanded differences are reported separately. This band classifies floating-point boundary uncertainty; it does not enlarge GPU occupancy. Lowest triangle record wins ownership. Owner checks account for each candidate's geometric and alpha uncertainty and verify the actual owner's own intersection/visibility.

The final matrix has three fixture families at 64³/128³/256³, each across eight combinations of `geom/comp`, RHI thread `0/1`, and async compute `0/1`: **72 captures**. All eight raw volumes at each fixture/size are byte-identical, including owners, attributes, and ambiguous cells. Every run has zero missing/extra cells outside its declared uncertainty sets, zero invalid owners, and zero attribute failures; contract precision and recall are both 1.0.

| Fixture | Grid | Occupied | Nominal oracle | Nominal missing / extra | Possible-minus-certain cells |
| --- | ---: | ---: | ---: | ---: | ---: |
| Analytic | 64³ | 555 | 555 | 0 / 0 | 116 |
| Analytic | 128³ | 577 | 577 | 0 / 0 | 116 |
| Analytic | 256³ | 590 | 562 | 0 / 28 | 116 |
| Extended geometry | 64³ | 4,841 | 4,841 | 0 / 0 | 4,558 |
| Extended geometry | 128³ | 11,619 | 11,617 | 0 / 2 | 10,928 |
| Extended geometry | 256³ | 37,945 | 37,946 | 1 / 0 | 35,953 |
| Material atlas | 64³ | 2,549 | 2,544 | 1 / 6 | 48 |
| Material atlas | 128³ | 9,083 | 9,075 | 6 / 14 | 141 |
| Material atlas | 256³ | 35,578 | 35,581 | 25 / 22 | 508 |

The geometry fixture intentionally places large planes exactly on cell boundaries, explaining its large ambiguity set. Its nominal differences are listed rather than concealed by that set. Material uncertainty also includes the alpha band defined below. Some fixture coordinates depend on resolution, so totals across sizes are not convergence measurements of identical world geometry.

The original 62-record analytic fixture includes the missing-center subpixel case, slopes, slivers, exact planes, all three orientations, both windings, duplicate overlaps, and 30 fixed-seed random triangles. The 94-record extended fixture adds tessellated shared edges, axis ties, a closed box, thin walls, a long diagonal, near-degenerate/collinear triangles, outer contacts, and separate nonuniform/negative-scale instances. Two degenerate records establish bounds. Explicit inspection confirms 560 strictly interior box cells remain empty at each size. A separate eight-run 75%-extent fixed-grid test exercises clipping and outside geometry: 2,838 occupied cells, zero errors, identical raw records.

Before/after slices at z=22 are retained as [baseline geometry](../build/voxelization-calibration/baseline/geom-64-thread0-async0.z22.png) and [corrected geometry](../build/voxelization-calibration/verified-analytic-64/geom-64-thread0-async0.z22.png). These small native-resolution panels are oracle | engine | reference when available; white is occupied, red missing, yellow extra, purple ambiguity.

## Materials, G-buffer comparison, and deliberate approximations

[voxelization_fixtures.py](../tools/voxelization_fixtures.py) generates a 24-record material atlas. [voxelization_materials.py](../tools/voxelization_materials.py) independently evaluates its source material definitions, texture pixels, captured vertex UVs/colors, and signed-area barycentrics. It applies repeat/bilinear LOD-0 sampling, sRGB conversion for color/emission before filtering, linear metallic sampling, texture factors, UV0/UV1 selection, vertex color/alpha, cutoff, and HDR emissive strength. One image is used simultaneously in sRGB and linear roles.

Both producers and resolve use a projected cell-center barycentric point, clamped and renormalized. It is a representative sample, not an exact closest-point projection. The masked visibility contract uses that point at LOD 0. The alpha cutoff uncertainty band is **0.01**, separately reported from geometric uncertainty to allow finite texture-filter fractional precision. There are 51/147/516 alpha-ambiguous cells at 64³/128³/256³, including cells outside the final occupancy uncertainty union because another surface may establish certain occupancy.

Tolerances were fixed for the final matrix:

| Signal | Independent CPU expectation | Equivalent G-buffer sample |
| --- | --- | --- |
| Constant albedo / metallic | One UNORM8 step | Three UNORM8 steps |
| Textured albedo / metallic | Three UNORM8 steps | Three UNORM8 steps |
| Encoded mesh-normal components | `1.5/255`; independent inverse-transpose/cofactor transform | Two UNORM8 steps with the default neutral normal texture |
| Constant HDR emission | `0.004` absolute | `0.016` absolute |
| Textured HDR emission | Per channel `max(0.004, abs(emissionFactor)/128 + abs(expected)*0.001)` | `0.016` absolute |

Occupied alpha must be one; empty surface outputs and emission alpha must be zero; HDR values must be finite. Voxel normals remain interpolated mesh normals. Arbitrary normal-map agreement with the G-buffer is **not** claimed.

The real `GBufferSP` shaders render the atlas at 9× resolution per axis with an orthographic calibration camera. The center of each 9×9 footprint matches the voxel XY center; derivatives keep these atlas textures at LOD 0. At 64³ all eight runs passed **1,634 equivalent surface samples**, including 189 masked and 241 black samples. Twenty-five alpha-ambiguous center samples were classified separately. Conservative edge representatives outside the raster surface and intentional mixed surfaces are not treated as equivalent samples.

The high-resolution image has **2,818 visible footprint columns; 270 have no occupied voxel under the point-alpha rule**. This is a measured sampling limitation, not a conservative masked-footprint guarantee. Stage A inherits it; voxel resolution can remove thin/cutout details.

Mixed surfaces retain deterministic lowest-record ownership. Reversing atlas record/draw order preserves occupancy but changes **324 cell colors**; both producers still agree and pass their elected-owner oracle. At 256 deliberately overlapping red/blue G-buffer samples, raster last-draw color and voxel lowest-record color differ by mean absolute RGB `(1,0,1)`. The paper's concurrent reflectance averaging is intentionally not implemented or GPU-reproduced. These measurements quantify the retained representative-surface policy; they do not claim equivalence to averaging.

## Native lifecycle and failed work

Eight backend/thread/queue combinations each capture initial, rebuild, mode-switch, moved, removed, and restored states: **48 native volumes**. The fixed 75% grid remains unchanged throughout. Translation updates the real node SSBO; removal/restoration rebuild the existing render-scene buffer snapshot through `PrepareBuffers`. This tests GPU scene-data replacement, not a new file-reload or general dynamic-scene API.

| State | Published revision | Occupied cells | Required result |
| --- | ---: | ---: | --- |
| Initial | 1 | 958 | Baseline |
| Rebuild | 2 | 958 | Byte-identical baseline |
| Modes 2 → 3 → 1 | 2 | 958 | No geometry revision change; identical baseline |
| Translation `(0.13,-0.07,0.09)` | 3 | 832 | Updated oracle match; old cells removed |
| Remove all renderable nodes | 4 | 0 | All owners/surface outputs cleared |
| Restore nodes and buffers | 5 | 958 | Byte-identical baseline |

Each state passes the independent oracle and matches across all eight execution combinations. RenderCore tests additionally exercise empty/null-input and failed-handoff retry/revision behavior. Native removal uses zero triangle count with existing scene allocations; this does not certify fresh initialization of an all-null empty asset. A deliberately failed native device submission was not injected.

## Pinned reference harness

`--voxel-reference` runs an **isolated source-equivalent occupancy harness**, not the authors' full executable. The pinned [dvbgi commit](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/) is `581c116061b1294dad2a8eca6afb45b49cc95a7e`. Projection and fragment-cell mapping are expressed in canonical grid coordinates using the same captured inputs and bounds. The original unpadded automatic bounds are replaced explicitly for meaningful cell comparisons.

The harness emits original projected triangles, prefers X on dominant-axis ties, uses an eight-sample RGBA8 attachment, disables depth/culling and sample shading, and enables the full sample mask. The reference material sets `minSampleShading=1`, but `PipelineData` leaves `sampleShadingEnable=VK_FALSE`; this does not force eight fragment invocations. No custom locations are used. The tested device reports standard sample locations and 8x color support. The reference has no ZenEngine alpha-cutoff behavior, so comparisons use opaque inputs.

| Reference workload | Occupied | Missing | Extra | Precision | Recall |
| --- | ---: | ---: | ---: | ---: | ---: |
| Analytic 64³ | 266 | 281 | 5 | 0.981203 | 0.451172 |
| Extended geometry 64³ | 3,114 | 136 | 0 | 1.000000 | 0.519435 |
| Opaque Sponza 128³ | 114,699 | 4,502 | 360 | 0.996861 | 0.962116 |

Counts exclude the geometric uncertainty band. The two small fixtures ran all eight submission combinations; Sponza ran each producer's matching inputs. The reference's one-cell-per-fragment mapping is not conservative. Its omissions are comparison findings, not the engine's target contract. The harness captures no author reflectance aggregation or static/dynamic GI passes and makes no binary-equivalence claim about the full renderer.

## Sponza regions

Asset: `E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf`, 262,267 triangles. A generated copy changes alpha modes to opaque for the independent geometry oracle; the original asset is untouched. At 128³ the fixed padded grid origin is `(-1.21472263,-1.21472263,-1.21472263)`, cell size `0.0189800411`. Geometry and compute both produce **118,849 occupied cells**, equal to the nominal oracle, with zero missing/extra/owner errors and 15 geometric uncertainty cells. Sponza attribute checks cover finite HDR, occupied alpha, and encoded unit-normal length; they are not an independent oracle for every Sponza texture.

The original masked asset runs all eight submission combinations: **118,827 occupied cells and byte-identical complete records**. Its aggregate alpha coverage is a paired-backend comparison, not an independent exact texture-footprint result.

[voxelization_report.py](../tools/voxelization_report.py) publishes explicit half-open region bounds. These are inspected spatial bands containing the named features, not semantic segmentation. Every region has zero engine missing/extra cells against the opaque oracle:

| Band | Minimum inclusive → maximum exclusive | Engine occupied | Reference occupied | Reference missing / extra |
| --- | --- | ---: | ---: | ---: |
| Columns | `(29,43,54)` → `(33,55,61)` | 152 | 142 | 20 / 10 |
| Arches | `(29,54,54)` → `(46,61,61)` | 400 | 371 | 30 / 1 |
| Fabrics | `(30,44,68)` → `(104,60,78)` | 3,810 | 3,454 | 390 / 34 |
| Upper gallery rails | `(30,71,54)` → `(100,78,62)` | 1,191 | 1,126 | 78 / 13 |
| Floor/wall contacts | `(20,38,42)` → `(108,44,56)` | 2,749 | 2,736 | 21 / 8 |

Inspected masked-asset slices include [arches and gallery at z=57](../build/voxelization-calibration/verified-sponza128/geom-128-thread0-async0.inspect-axis2-57.png), [fabrics at z=72](../build/voxelization-calibration/verified-sponza128/geom-128-thread0-async0.inspect-axis2-72.png), and [floor at y=41](../build/voxelization-calibration/verified-sponza128/geom-128-thread0-async0.inspect-axis1-41.png). Enlarged color slices use display gamma 2.2 only. Their reference-difference overlays use red for engine-only and yellow for reference-only occupancy; these overlays are pairwise differences, not oracle-error classifications.

## Capture format and execution

`scene_renderer_demo --frames=2 --mode=1 --capture-voxels=<prefix>` enables diagnostic surface inputs and captures base mip 0, unaffected by derived lighting/mips. Readback is opt-in and waits for GPU completion. Scene storage inputs lacking transfer-source usage are copied through diagnostic compute before transfer readback; production buffer flags are unchanged.

`<prefix>.json` records grid, resolution, revision, triangle count, and strides. `<prefix>.voxels.bin` has 32 bytes per cell in `x + N*(y + N*z)` order:

| Offset | Value |
| --- | --- |
| 0 | `uint32` owner; `0xffffffff` empty |
| 4 | Packed UNORM8 RGBA albedo |
| 8 | Packed UNORM8 normal XYZ encoding and metallic |
| 12 | `uint32` owner-derived binary occupancy |
| 16 | Four `float32` emission values decoded from RGBA16F |

Other files contain actual vertices, indices, node matrices, triangle records, and materials. Only metadata's triangle count is meaningful, not allocation padding. `--voxel-grid-percent=N` replaces diagnostic bounds after normal scene normalization; 100 gives a centered unit cube, not automatic asset fitting. `--voxel-lifecycle`, `--voxel-gbuffer`, and `--voxel-reference` require a capture prefix. G-buffer capture uses 48 bytes per XY cell: position float4, packed albedo/normal/metallic, 9×9 visible sample count, emission/occlusion float4.

## Evidence and reproduction

Final evidence root: `build/voxelization-calibration/`.

| Evidence | Location/result |
| --- | --- |
| Starting tree | `before/manifest.json`, `before/head.txt`; base HEAD `9950899250a3919deacc1f8e8f9d7a456d43e936` plus preserved uncommitted changes |
| Start of completion increment | `continue-before/manifest.json` |
| Final source hashes | `implementation-files.json` |
| Reference provenance | `reference-provenance.json`; archived source also under `build/dynamic-voxel-paper/reference/` |
| Three-size, three-fixture matrix | `verified-{analytic,geometry,materials}-{64,128,256}/` |
| Lifecycle, clipping, reversed order | `verified-lifecycle/`, `verified-clipping/`, `verified-order-reversal/` |
| Sponza | `verified-sponza-opaque128/`, `verified-sponza128/`; `.regions.json` and `.paired.json` |
| Raw equality and input hashes | Each final directory's `volume-hashes.json` and `data-hashes.json` |
| Native calibration | **140 volume captures from 100 processes**, plus 18 reference and 10 G-buffer outputs; zero application errors, Vulkan VUIDs, or synchronization hazards |
| Build | `verified-build.log`, all five requested targets built |
| Unit tests | `verified-{RenderCoreTest,VulkanRHITest,ConfigLoaderTest,CommonTest}.log`: **465 + 39 + 15 + 36 = 555 passed** |
| Existing GI regressions | `verified-gi-regression.log`, `verified-gi-regression/results.json`: **158 GPU cases and image assertions passed** |
| Shader validation | Nine affected/new modules passed `spirv-val --target-env vulkan1.2` |
| Device | `vulkaninfo.txt` |

Windows, NVIDIA GeForce RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, glslang 16.4.0, SPIR-V 1.3 build target, MSVC 14.51.36231 Debug. Native calibration enables synchronization validation and disables implicit layers/RTSS. Original `Data/engine.cfg` bytes are restored after each runner. No source was staged or committed.

Reproduce from the repository root in the Visual Studio x64 developer shell:

```powershell
cmake -S . -B build/x64-windows-msvc-debug
cmake --build build/x64-windows-msvc-debug --target scene_renderer_demo RenderCoreTest VulkanRHITest ConfigLoaderTest CommonTest -j 6
foreach ($n in 64,128,256) {
    python -B tools/validate_voxelization.py --matrix --resolution $n --output build/voxelization-calibration/recheck-analytic-$n
    python -B tools/validate_voxelization.py --fixture geometry --matrix --grid-percent 100 --resolution $n --output build/voxelization-calibration/recheck-geometry-$n
    python -B tools/validate_voxelization.py --fixture materials --matrix --grid-percent 100 --resolution $n --output build/voxelization-calibration/recheck-materials-$n
}
python -B tools/validate_voxelization.py --matrix --reference --output build/voxelization-calibration/recheck-reference
python -B tools/validate_voxelization.py --fixture geometry --matrix --grid-percent 100 --reference --output build/voxelization-calibration/recheck-geometry-reference
python -B tools/validate_voxelization.py --fixture materials --matrix --grid-percent 100 --gbuffer --output build/voxelization-calibration/recheck-gbuffer
python -B tools/validate_voxelization.py --fixture materials --grid-percent 100 --gbuffer --reverse-order --output build/voxelization-calibration/recheck-reversed
python -B tools/validate_voxelization.py --lifecycle --matrix --grid-percent 75 --output build/voxelization-calibration/recheck-lifecycle
python -B tools/validate_voxelization.py --fixture geometry --matrix --grid-percent 75 --output build/voxelization-calibration/recheck-clipping
python -B tools/validate_voxelization.py --model E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf --opaque-model --resolution 128 --reference --output build/voxelization-calibration/recheck-sponza-opaque
python -B tools/validate_voxelization.py --model E:/Dev/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf --capture-only --resolution 128 --matrix --output build/voxelization-calibration/recheck-sponza
python -B tools/voxelization_report.py build/voxelization-calibration/recheck-sponza-opaque/geom-128-thread0-async0 --other build/voxelization-calibration/recheck-sponza-opaque/comp-128-thread0-async0 --sponza-regions
bin/RenderCoreTest.exe
bin/VulkanRHITest.exe
bin/ConfigLoaderTest.exe
bin/CommonTest.exe
python -B tools/validate_voxel_gi.py
```

Run config-mutating GPU scripts serially. `--compare <existing-prefix>` repeats the oracle on generated fixtures or opaque-model captures; it is not a general textured-scene oracle. Input contracts and generated assets stay beside captures.

## Exit decision and remaining limits

V0 meets Section 2.3 of the implementation plan: independent coverage evidence, matching producers, material/G-buffer checks, lifecycle/failure checks, reference comparison, and inspected Sponza regions. The allowed approximations are explicitly bounded: numeric boundary uncertainty, point-alpha coverage, representative owner attributes, mesh normals, and an occupancy-only reference harness. After M2 changes static/dynamic production, rerun the relevant suite for each class and their union.

This was verified on one GPU/driver. The broader `VulkanRHIIntegrationTest` suite was not rerun; its previously documented [suite-order failure](VoxelGIVerification.md#broader-vulkan-suite-limitation) is not resolved by these results. No GPU performance or rectangle-rasterization optimization claim is made. **V1 has now passed** in the separate [reflectance verification record](VoxelReflectanceVerification.md), preserving this V0 contract and the owner compatibility path. M0 is next. Stage A remains a compute DDA approximation and hardware triangle queries remain deferred to Stage B.
