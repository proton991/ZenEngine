# P4 gap execution, 2026-10-10

**P4 is not accepted.** This is the implementation and measurement record for [P4Gaps.md](P4Gaps.md). Error limits, the five shipping-gate environments, the nine ground-truth environments, camera matrices, four diffuse rays, 32 history frames and five edge-stopping iterations are unchanged. Failed estimator probes were restored; none is a shipping implementation.

Final sweep: maximum full-image/floor shipping bias is **0.46%**. After the review's [flat default normal fix](#flat-default-normal-fix-review-2026-10-10), converged references pass 17/18 views; only the hotel hall (**3.13%**) exceeds the 3% excess limit (before the fix: 16/18, hotel 3.87% and qwantani 4.62%). Seven of ten gating camera/environment pairs fail shipping quality, including all five halls. Provider-switch settling and large-coordinate rendering also fail. Detailed numerical evidence is saved in [p4-gap-results.json](p4-gap-results.json).

## Changes retained

- **G2, filter bias:** seven coarse residual passes recover the geometry-safe, demodulated difference between temporal sky and the five-pass filtered result. The negative correction is bounded to preserve resolved positive samples. The correction never feeds back into history. The first, unbounded probe introduced floor zeros and was rejected.
- **G3, receiver positions:** hardware rays now start from interpolated RGBA32F raster positions instead of depth-unprojected positions with a two-depth-step displacement. Voxel receivers retain depth reconstruction. The shared hybrid G-buffer grows from 48 to 64 bytes/pixel. This follows the pre-experiment rationale in P4Gaps.md; it improves measured distant shadow agreement but does not close G3 by itself.
- **Independent reference fixes:** rejected alpha candidates continue along the incoming ray instead of moving along the rejected surface normal. A transparent sheet separated from an opaque sheet by 20 micro-units is a regression. Primary rays now derive their eye and directions from the same inverse of the captured float32 projection-view matrix. All twelve base fixtures are required, rather than silently skipping missing captures.
- **Reference throughput:** the alpha visibility loop runs on the device. One million Sponza visibility queries matched the previous scalar loop bit for bit; warmed query time was 0.03465 s versus 0.09176 s. This is a query microbenchmark, not a full-render speedup claim.
- **G6 diagnostics:** native provider-switch, world-origin and motion/cut/deformation/alpha-edit sequences; capture-only voxel sky-cache fallback counts; translated-origin ray-query tests.
- **G7 reporting:** reference-vs-truth and shipping-vs-tier errors on primary-surface mismatch pixels are reported separately, along with missing primary hits. Level-zero ray alpha acceptance remains the contract.
- **G8 normal maps:** `--shading-normals engine` uses captured shading normals while keeping primary geometry, alpha, origins and visibility independent. Normal-mapped datasets also fingerprint those normals; diagnostic engine-receiver renders cannot be registered as independent truth.

## Estimator probes rejected

Global soft-normal filtering and a filter restricted to isolated fine geometry reduced zeros but worsened other errors. Directional bright-source collapse changed finite-source penumbrae. Finite-source quadrature, residual-only importance sampling and separate reconstruction of source and residual contributions improved some halls but did not pass. Separate reconstruction is by source group, not an independently reconstructed signal per light.

The final source-group probe (32 quadrature directions, residual proposal, separate reconstruction) measured:

| Hall | RMS | P99 | Unexpected zeros |
| --- | ---: | ---: | ---: |
| Qwantani | 8.03% | 11.99% | 1 |
| Hotel | 7.86% | 26.57% | 5 |
| Carpentry | 8.85% | 37.00% | 4 |
| Studio, tracked | 27.83% | 77.33% | 35 |

The reservoir fallback used two fresh candidates and two revalidated previous reservoirs within four visibility queries, with positive proposal support and bounded represented history. It failed all six tested halls, including substantial finite-capture bias and much higher RMS. It is not adopted. Its source snapshots, restoration build and results are in `build/p4-gaps/reuse/`; the reusable explicit-source experiment is [prototype_environment_sources.py](../../tools/prototype_environment_sources.py). No 8-ray or 64-frame-history preset was adopted or claimed performance-qualified.

## Independent arbitration and precision

The initial Embree MIS tiebreaker found the alpha-continuation defect and implicated receiver placement. Vulkan and Embree then agreed on all 619,804 fixed bright-source visibility queries from identical origins in the hotel and qwantani halls. The receiver-plane probe did not close the image gate and was reverted before the FP32 position target was tested.

The sub-degree source/occluder-edge fixture has near, middle and far receivers. All three agree with the analytic shadow test on 513 rays each. The far engine-reference excess fell from 11.99% with depth reconstruction to 1.68% with FP32 positions and corrected primary rays. This isolates a real precision improvement; it does not establish all Sponza receivers as correct.

Final targeted arbitration (128 worst far pixels, 65,536 samples per proposal per half) reproduces the engine at its own receivers: normalized RGB RMS 2.78% for hotel and 1.67% for qwantani, with Embree noise around 1.2%. At independent primary hits the disagreement with the engine is 34.77% and 22.60%. These deliberately selected pixels are not image-wide gate statistics. Swapping only origin policies does not remove the discrepancy. Their raster positions reproject about 0.001 pixel away from the pixel center despite lying close to the same tangent plane; a derivative-based pixel-center correction reduced this reprojection error but barely changed lighting (hotel 3.85%, qwantani 4.52% excess), so it was reverted. Its four reference captures and source snapshot are retained in `build/p4-gaps/pixel-center/`.

The corrected immutable dataset is `build/ground-truth/dataset-p4-corrected/`. The original dataset and intermediate alpha-continuation dataset remain available for historical comparisons. Every final render uses 32,768 samples, direction-preserving alpha continuation and the corrected primary-ray construction. The two imported corridor renders use the equivalent scalar visibility loop; the other sixteen use the device loop. The fixture-gate report is archived with the dataset.

## Native acceptance checks

Both RTX 5080 and AMD Radeon iGPU pass 13 hardware and 10 voxel native fixtures with the FP32 layout (46 cases total). The extended ray-query suite passes 12 cases on each GPU, including translations of 100 and 10,000 units. This establishes coarse query behavior, not full-image large-coordinate accuracy.

The Sponza origin sequence translates every instance and the camera together, preserving the view direction. Relative to its zero-offset capture, at 100 units the floor RMS is 0.59% with no floor zeros, but the full image has 420 unexpected zeros. At 10,000 units full-image bias is -49.65%, RMS 75.53%, and there are 177,691 unexpected zeros; floor RMS is 13.17% with 247 zeros. All images remain finite and fully covered. **Large-coordinate rendering fails.** A camera-relative/rebased rendering design is still needed; passing AS queries does not replace that work.

Provider switching captures hardware -> voxel -> hardware at frames 1 and 32. Both actual transitions reset to history length 1 with `provider_changed`; frame-32 lengths reach approximately 32. Image-quality results are reported below separately from reset correctness.

The stability sequence moves the camera sideways by 0.02 normalized units over sixteen frames, makes a forward cut of 0.3 units, deforms vertex heights with a sine displacement of 1% of the source X extent, then halves masked-material alpha. Each event starts from a settled original scene, captures frames 1/4/8/32, and gets a fresh 1024-sample x 64-frame reference of the altered scene. Geometry/opacity edits and cuts reset history. These tests do not claim continuous deforming animation or every-frame moving-shadow coverage.

Seven alpha fixtures cover a second UV set, texture transforms, vertex alpha, specular-glossiness alpha, and repeat/clamp/mirror samplers. Native captures and independent comparisons pass; the largest primary mismatch is approximately 0.027%. The tilted-normal fixture has analytic expected irradiance 0.85355079 and independent observed 0.85340196 (0.017% relative difference, 8,192 samples). Captured half-precision normals are renormalized before cosine sampling. Full normal-mapped independent renders cover Papermill top and hall at 32,768 samples (converged full-image excess 0.37% and 1.67%, floor excess 0.34% and 0.33%); the other eight environments retain the vertex-normal independent gate and normal-mapped same-tier shipping gate. The immutable normal-mapped dataset is `build/ground-truth/dataset-p4-normal-mapped/`.

The diagnostic cache counter reports **0 center fallbacks / 22,997 evaluated occupied voxels** on 64-cubed Sponza for both providers. Cache RGB is unchanged; its existing alpha marks unevaluated/owner/fallback states, and a counter buffer is allocated only during capture. D5's sampling and measurement are implemented; the ownerless center fallback remains a policy to assess on other geometry.

## Verification and cost

MSVC Debug builds succeed; all 120 SceneRenderer/VoxelGI SPIR-V modules validate for Vulkan 1.2. CommonTest 112, ConfigLoaderTest 21, RenderCoreTest 576 and VulkanRHITest 51 pass. The 76 final sweep/native logs contain no validation errors or presentation hazards; all numerical capture channels are finite (identity bit patterns are excluded from that check). Earlier presentation failures remain documented separately in the README. RX 7900 XT, a non-RT GPU and Apple Silicon/MoltenVK remain unverified under decision 3; no unavailable platform is marked passed.

The position target adds 16 bytes/pixel. The seven RGBA32F correction targets declare 112 bytes/pixel before render-graph reuse, with no added persistent history. Bias-correction probe profiles are archived under `bias/` and `bias-bounded/`. The final native captures ran concurrently with independent GPU rendering, so their timings cannot establish isolated GI cost. Controlled Release-build GI timing and peak-memory qualification remain P8 work.

## Reproduction and artifacts

Use the Python environment from `tools/requirements-gi-quality.txt`; system Python may lack NumPy, Mitsuba and Embree. Capture commands mutate `Data/engine.cfg` transactionally, so run them serially. Offline truth rendering may run independently.

```powershell
python tools/ground_truth_sweep.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --scene PATH/TO/Sponza/glTF/Sponza.gltf --dataset build/ground-truth/dataset-p4-corrected --output build/p4-gaps/acceptance-verified
python tools/capture_hybrid_gi.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --scene PATH/TO/Sponza/glTF/Sponza.gltf --provider hardware --rt --camera hall --output build/p4-gaps/check-hall --provider-switching --origin-stress --stability
python tools/report_hybrid_gi_flagpoles.py --captures build/p4-gaps/acceptance-verified --dataset build/ground-truth/dataset-p4-corrected --output build/p4-gaps/flagpoles
```

`--reuse-captures` checks executable, shader and source hashes and rejects stale/incomplete files. Native sequence captures are diagnostics of runtime mutations; use their matching in-process references, not the original glTF as independent truth for those mutations.

Generated archives are ignored by Git: `build/p4-gaps/acceptance-verified/`, `final-native/`, `fp32-native/`, `arbitrate-final/`, `edges-final/`, `truth-fixtures-primary/`, `material-truth/`, `normal-final/`, `normal-unit-fixture/`, `flagpoles/`, and `sources-separated/`. The frozen nine-environment report follows.

## Settling and provider-switch results

All errors are bias / RMS / P99 / unexpected zeros. Each event is compared with a reference of its current geometry and camera.

| View and event, frame 32 | All receivers | Floor |
| --- | --- | --- |
| top, motion | 0.06% / 2.25% / 7.71% / 0 | 0.11% / 3.87% / 12.66% / 1 **Fail** |
| top, cut | 0.03% / 6.98% / 24.52% / 2 **Fail** | 0.01% / 6.35% / 18.96% / 0 |
| top, deform | 0.02% / 3.00% / 9.71% / 0 | 0.01% / 7.41% / 23.24% / 0 **Fail** |
| top, alpha | 0.02% / 2.90% / 9.45% / 0 | 0.07% / 7.10% / 22.40% / 0 **Fail** |
| hall, motion | -0.01% / 7.16% / 26.54% / 4 **Fail** | 0.05% / 2.76% / 9.28% / 0 |
| hall, cut | -0.07% / 10.17% / 36.85% / 3 **Fail** | 0.04% / 6.64% / 19.51% / 0 |
| hall, deform | -0.05% / 11.51% / 45.45% / 9 **Fail** | 0.05% / 6.24% / 19.35% / 0 |
| hall, alpha | -0.13% / 10.62% / 41.56% / 12 **Fail** | -0.03% / 6.04% / 19.43% / 0 |

| Provider transition, frame 32 | All receivers | Floor |
| --- | --- | --- |
| top, to voxel | -0.14% / 2.60% / 9.13% / 0 | 0.27% / 10.99% / 37.85% / 0 **Fail** |
| top, to hardware | 0.02% / 2.91% / 9.51% / 0 | 0.14% / 7.12% / 22.26% / 2 **Fail** |
| hall, to voxel | 0.01% / 17.03% / 70.45% / 6 **Fail** | 0.29% / 10.29% / 35.43% / 0 **Fail** |
| hall, to hardware | -0.08% / 11.06% / 43.05% / 20 **Fail** | 0.02% / 6.20% / 19.87% / 0 |

## Flagpole crops

Three fixed rectangles cover the poles at y=166/320/477 in the 960x540 top view. Crop statistics are diagnostic; the frozen image/floor regions remain the acceptance gates. PNGs use a common scale within each environment and show all three crops.

All nine crop sheets were visually inspected. Converged shadow placement and structure agree; maximum crop excess is 2.03% and absolute bias 0.28%. The D2 start-cell zero-square artifact is absent. Shipping has one isolated zero in the kloofendal crop and one in studio under the crop-specific threshold; the frozen full-floor regions report none. Shipping variance remains visible and fails several crop RMS/P99 values. This closes the specific D2 artifact check without declaring shipping reconstruction accepted.

| Environment | Worst crop reference bias / excess | Worst crop shipping RMS / P99 | Shipping crop zeros |
| --- | --- | --- | --- |
| papermill-top | 0.19% / 0.58% | 4.31% / 14.22% | 0 |
| hotel-top | 0.26% / 0.87% | 6.10% / 18.81% | 0 |
| kloppenheim-top | 0.28% / 0.68% | 6.20% / 20.28% | 0 |
| kloofendal-top | 0.21% / 0.76% | 9.37% / 27.60% | 1 |
| qwantani-top | 0.26% / 0.92% | 7.99% / 25.25% | 0 |
| studio-top | 0.24% / 1.85% | 5.63% / 22.24% | 1 |
| emptyroom-top | 0.24% / 0.58% | 6.19% / 18.40% | 0 |
| corridor-top | 0.25% / 0.84% | 7.04% / 23.00% | 0 |
| carpentry-top | 0.21% / 2.03% | 10.70% / 35.72% | 0 |

## Flat default normal fix (review, 2026-10-10)

Review of this execution found an engine defect outside the GI code. Every material without an authored normal map was bound to the scene's 1×1 default normal texture, {127, 127, 255}, and shaded through it. Eight bits cannot store a flat normal: 127/255 decodes to −0.0039, which tilted the shading normal 0.32° on every such surface (Sponza's flat floor shaded with (0.0039, 0.99998, −0.0039) in the normal-map-free captures). The "like-for-like" vertex-normal comparisons were therefore not like-for-like, and the tilt is a large relative error wherever a small bright source lights a surface at a grazing angle.

The default normal texture is now marked `flatNormal`, and a material bound to it publishes no normal map (`normalTexIndex = -1`), so shading uses the vertex normal through the existing no-map path ([Texture.h](../../ZenCore/Include/SceneGraph/Texture.h), [Material.h](../../ZenCore/Include/SceneGraph/Material.h), [Scene.cpp](../../ZenCore/Source/SceneGraph/Scene.cpp)). Authored normal maps, including authored flat ones, are unchanged: 24 of Sponza's 25 materials have one, and the normal-mapped shipping captures are bit-identical before and after. Regressions: an importer test checks that a material without a normal map publishes −1, and the open-plane fixtures now require the shading normal's tangential components below 1e-4 (0.0039 before the fix, 1.5e-5 after, the G-buffer encoding limit).

Against the unchanged corrected dataset, every one of the 18 views moves closer to the ground truth, and 8×8-block excess falls two- to eightfold. Qwantani hall now passes; hotel hall is the only remaining converged failure, at 3.13% against the 3% limit (its 8×8 excess is 0.51%, and its own ground-truth noise is 4.14%). Shipping-gate results are unchanged because they compare normal-mapped captures, and the maximum shipping bias stays 0.46%.

| View | Excess before → after | Beyond 0.3 units before → after | 8×8 excess before → after | Verdict after |
| --- | --- | --- | --- | --- |
| papermill-hall | 0.92% → 0.63% | 1.21% → 0.72% | 0.63% → 0.08% | Pass |
| hotel-hall | 3.87% → 3.13% | 10.58% → 8.67% | 1.95% → 0.51% | **Fail** |
| kloppenheim-hall | 1.24% → 0.71% | 2.66% → 0.64% | 0.92% → 0.10% | Pass on 8×8 blocks (per-pixel noise 3.37%) |
| kloofendal-hall | 2.94% → 1.28% | 7.78% → 3.42% | 2.28% → 0.25% | Pass |
| qwantani-hall | 4.62% → 2.36% | 12.42% → 6.57% | 3.43% → 0.42% | Pass |
| studio-hall | 2.65% → 0.70% | 7.36% → 1.61% | 1.81% → 0.49% | Pass on 8×8 blocks (per-pixel noise 11.14%) |
| emptyroom-hall | 1.22% → 0.85% | 2.17% → 0.89% | 0.79% → 0.17% | Pass on 8×8 blocks (per-pixel noise 3.11%) |
| corridor-hall | 1.49% → 1.10% | 3.52% → 2.43% | 0.84% → 0.32% | Pass on 8×8 blocks (per-pixel noise 4.35%) |
| carpentry-hall | 2.63% → 1.38% | 5.96% → 2.39% | 2.05% → 0.24% | Pass on 8×8 blocks (per-pixel noise 5.40%) |

Verification on the fixed build: CommonTest 112, RenderCoreTest 576, RayQueryIntegrationTest 12, ConeVoxelGIIntegrationTest 16, EditorModelTest 41, EditorRenderingTest 39 and EditorPreviewTest 16 pass with synchronization validation and no validation messages; the 13 hardware and 10 voxel native fixtures pass. The sweep is `build/ground-truth/runs/2026-10-10-flat-normal/` (`report.json`, `summary.md`); the tables below predate this fix.

## Nine-environment sweep (before the flat default normal fix)

Build: E:\Dev\ZenEngine\build\x64-windows-msvc-debug\bin\scene_renderer_demo.exe

## Tier reference and shipping against the ground truth

Excess is RMS error with the ground truth's own noise removed; limits for the tier reference: |bias| <= 1%, excess <= 3% per pixel and on 8x8 blocks.

| View | Ground-truth noise | Tier reference: bias / excess / 8x8 excess | Floor: bias / excess | Near / far excess | Shipping: bias / excess | Tier reference |
| --- | ---: | --- | --- | --- | --- | --- |
| papermill-top | 0.51% | -0.16% / 0.40% / 0.21% | -0.06% / 0.35% | 0.44% / 0.14% | -0.14% / 1.38% | Pass |
| papermill-hall | 2.71% | -0.36% / 0.92% / 0.63% | -0.08% / 0.35% | 0.87% / 1.21% | -0.35% / 4.99% | Pass |
| hotel-top | 0.58% | -0.12% / 0.67% / 0.27% | +0.20% / 0.80% | 0.73% / 0.26% | -0.10% / 1.93% | Pass |
| hotel-hall | 4.14% | -0.38% / 3.87% / 1.95% | -0.08% / 0.94% | 0.95% / 10.58% | -0.37% / 13.92% | **Fail** |
| kloppenheim-top | 0.39% | -0.10% / 0.40% / 0.20% | +0.01% / 0.53% | 0.45% / 0.09% | -0.07% / 1.89% | Pass |
| kloppenheim-hall | 3.37% | -0.35% / 1.24% / 0.92% | -0.15% / 0.59% | 0.85% / 2.66% | -0.32% / 11.28% | Pass on 8x8 blocks (per-pixel noise 3.37%) |
| kloofendal-top | 0.43% | -0.13% / 0.64% / 0.39% | -0.00% / 0.50% | 0.71% / 0.19% | -0.11% / 1.11% | Pass |
| kloofendal-hall | 2.50% | -0.46% / 2.94% / 2.28% | -0.08% / 0.57% | 1.04% / 7.78% | -0.46% / 7.69% | Pass |
| qwantani-top | 0.39% | -0.14% / 0.70% / 0.43% | +0.00% / 0.68% | 0.77% / 0.23% | -0.11% / 2.14% | Pass |
| qwantani-hall | 2.22% | -0.55% / 4.62% / 3.43% | -0.11% / 0.74% | 1.44% / 12.42% | -0.52% / 13.70% | **Fail** |
| studio-top | 0.77% | -0.11% / 0.50% / 0.32% | -0.03% / 1.86% | 0.56% / 0.06% | -0.12% / 2.92% | Pass |
| studio-hall | 11.14% | -0.17% / 2.65% / 1.81% | -0.10% / 2.05% | 0.44% / 7.36% | -0.14% / 30.94% | Pass on 8x8 blocks (per-pixel noise 11.14%) |
| emptyroom-top | 0.53% | -0.05% / 0.57% / 0.36% | +0.00% / 0.50% | 0.63% / 0.11% | +0.04% / 2.99% | Pass |
| emptyroom-hall | 3.11% | -0.36% / 1.22% / 0.79% | -0.11% / 0.51% | 1.00% / 2.17% | -0.29% / 11.12% | Pass on 8x8 blocks (per-pixel noise 3.11%) |
| corridor-top | 0.66% | -0.10% / 0.72% / 0.51% | -0.06% / 0.63% | 0.81% / 0.09% | -0.16% / 2.48% | Pass |
| corridor-hall | 4.35% | -0.30% / 1.49% / 0.84% | -0.14% / 0.62% | 0.86% / 3.52% | -0.26% / 12.91% | Pass on 8x8 blocks (per-pixel noise 4.35%) |
| carpentry-top | 0.68% | -0.03% / 0.59% / 0.40% | +0.11% / 0.50% | 0.65% / 0.17% | +0.00% / 2.18% | Pass |
| carpentry-hall | 5.40% | -0.25% / 2.63% / 2.05% | +0.08% / 0.86% | 1.67% / 5.96% | -0.22% / 13.09% | Pass on 8x8 blocks (per-pixel noise 5.40%) |

## Shipping gate (normal maps on, against the tier reference)

Bias / RMS / P99 / exact zeros; limits |bias| <= 2%, RMS <= 8%, P99 <= 20%, no zeros.

| View | All receivers | Floor | Temporal bias / RMS | Filtered bias / RMS | Largest errors on fine geometry |
| --- | --- | --- | --- | --- | --- |
| papermill-top | +0.02% / 1.88% / 6.28% / 0 | +0.13% / 3.16% / 10.96% / 0 | +0.00% / 4.29% | +0.02% / 1.82% | 1.77% |
| papermill-hall | **-0.09% / 5.90% / 21.56% / 4** | +0.04% / 2.32% / 8.23% / 0 | -0.01% / 25.54% | -0.09% / 5.73% | 13.56% |
| hotel-top | +0.04% / 2.78% / 8.95% / 0 | **+0.07% / 6.36% / 21.40% / 0** | -0.00% / 7.91% | +0.02% / 2.50% | 2.87% |
| hotel-hall | **-0.01% / 13.93% / 51.54% / 6** | -0.01% / 7.91% / 18.45% / 0 | -0.00% / 37.42% | -0.02% / 12.99% | 11.52% |
| kloppenheim-top | +0.04% / 2.55% / 8.48% / 0 | +0.17% / 5.80% / 19.10% / 0 | +0.00% / 5.13% | +0.04% / 2.61% | 1.89% |
| kloppenheim-hall | **-0.00% / 11.90% / 48.26% / 44** | +0.01% / 5.02% / 17.15% / 0 | -0.03% / 43.57% | +0.00% / 11.80% | 14.68% |
| kloofendal-top | +0.03% / 1.47% / 4.81% / 0 | +0.16% / 5.89% / 18.02% / 0 | -0.00% / 3.28% | +0.04% / 1.45% | 1.91% |
| kloofendal-hall | **-0.03% / 7.81% / 27.95% / 16** | +0.04% / 6.67% / 18.64% / 0 | -0.02% / 26.12% | -0.03% / 7.55% | 13.56% |
| qwantani-top | +0.04% / 2.99% / 10.25% / 0 | **+0.19% / 6.94% / 23.40% / 0** | -0.00% / 8.18% | +0.04% / 3.02% | 3.76% |
| qwantani-hall | **+0.02% / 13.51% / 49.14% / 6** | **+0.03% / 6.42% / 23.10% / 0** | -0.01% / 45.33% | +0.02% / 13.63% | 7.75% |
| studio-top | -0.02% / 3.48% / 10.81% / 0 | **+0.46% / 5.75% / 21.68% / 0** | -0.01% / 5.11% | -0.02% / 3.47% | 1.85% |
| studio-hall | **+0.04% / 31.35% / 88.73% / 30** | +0.25% / 4.35% / 13.83% / 0 | +0.08% / 82.64% | +0.04% / 31.33% | 4.28% |
| emptyroom-top | +0.26% / 4.49% / 15.60% / 0 | +0.15% / 5.44% / 16.92% / 0 | +0.00% / 6.96% | +0.26% / 4.44% | 1.14% |
| emptyroom-hall | **+0.10% / 12.13% / 45.30% / 39** | +0.04% / 5.22% / 15.94% / 0 | -0.03% / 49.45% | +0.10% / 12.05% | 16.63% |
| corridor-top | -0.00% / 3.12% / 10.43% / 0 | **+0.18% / 6.90% / 23.52% / 0** | +0.00% / 4.49% | +0.01% / 3.03% | 1.97% |
| corridor-hall | **+0.07% / 13.67% / 48.66% / 26** | +0.04% / 5.50% / 18.76% / 0 | +0.00% / 47.09% | +0.07% / 12.80% | 10.78% |
| carpentry-top | +0.06% / 2.79% / 9.52% / 0 | +0.10% / 3.29% / 12.52% / 0 | -0.00% / 4.38% | +0.04% / 2.48% | 2.14% |
| carpentry-hall | **+0.01% / 13.75% / 57.23% / 7** | **+0.01% / 6.44% / 27.41% / 0** | -0.01% / 41.51% | +0.01% / 13.31% | 6.64% |

## Primary-surface mismatches

Different primary surfaces are excluded from the agreement gate above and reported here separately. Truth errors compare different surfaces; shipping errors use the same raster receiver and tier reference. Rays retain level-zero alpha acceptance.

These pixels are 0.19% of the top image and 0.88% of the hall image. Although their local normalized errors are large, they contribute at most 3.74% of whole-image shipping squared RGB error across the sweep. Local zeros use each region's own reference-mean threshold, so counts cannot be added or compared directly with the full-image count. Texture LOD matching remains an open investigation; this measurement does not establish that LOD changes would resolve the reconstruction failures.

| View | Mismatches / covered | Missing truth hits | Reference vs truth: bias / excess | Shipping vs tier: bias / RMS / P99 / zeros |
| --- | --- | --- | --- | --- |
| papermill-top | 984 / 518400 | 0 | -33.26% / 111.33% | +5.90% / 26.21% / 95.25% / 2 |
| papermill-hall | 4563 / 518400 | 0 | -5.66% / 218.62% | +0.69% / 29.23% / 131.64% / 1 |
| hotel-top | 984 / 518400 | 0 | -23.55% / 169.67% | +2.10% / 16.10% / 67.22% / 0 |
| hotel-hall | 4563 / 518400 | 0 | -7.56% / 385.26% | -0.05% / 25.33% / 100.46% / 0 |
| kloppenheim-top | 984 / 518400 | 0 | -25.79% / 96.78% | +4.91% / 32.89% / 112.75% / 8 |
| kloppenheim-hall | 4563 / 518400 | 0 | -5.45% / 201.04% | -0.48% / 56.09% / 233.88% / 26 |
| kloofendal-top | 984 / 518400 | 0 | -25.86% / 206.71% | +4.56% / 28.72% / 98.59% / 7 |
| kloofendal-hall | 4563 / 518400 | 0 | -5.77% / 203.12% | -0.32% / 53.05% / 231.79% / 15 |
| qwantani-top | 984 / 518400 | 0 | -13.66% / 466.17% | +4.54% / 39.99% / 103.19% / 4 |
| qwantani-hall | 4563 / 518400 | 0 | -5.75% / 208.98% | +2.20% / 71.41% / 285.59% / 31 |
| studio-top | 984 / 518400 | 0 | -6.57% / 163.85% | +0.80% / 24.02% / 157.38% / 0 |
| studio-hall | 4563 / 518400 | 0 | -6.23% / 199.97% | +1.68% / 72.53% / 290.50% / 50 |
| emptyroom-top | 984 / 518400 | 0 | -27.56% / 81.81% | +6.07% / 31.12% / 108.86% / 6 |
| emptyroom-hall | 4563 / 518400 | 0 | -6.76% / 198.40% | +1.27% / 50.78% / 218.52% / 22 |
| corridor-top | 984 / 518400 | 0 | -20.13% / 121.05% | +3.76% / 26.17% / 98.31% / 10 |
| corridor-hall | 4563 / 518400 | 0 | -5.75% / 223.52% | +0.91% / 60.25% / 258.55% / 30 |
| carpentry-top | 984 / 518400 | 0 | -40.63% / 133.26% | +3.64% / 22.85% / 86.50% / 0 |
| carpentry-hall | 4563 / 518400 | 0 | -18.03% / 376.89% | -0.13% / 31.46% / 146.33% / 5 |
