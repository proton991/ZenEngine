# M7 quality validation

Date: 2026-09-25. **Current status: all 56 image comparisons pass after the [quality corrections](DynamicVoxelGIM7QualityFixVerification.md).** The sections below preserve the original failing baseline.

**Original validation: overall quality acceptance failed on Sponza.** The new suite passes 48 of 56 image comparisons, all four stationary-image sequences, 11 CPU reference tests and 28 native filtering tests. The eight failures are Sponza at both grid sizes, with both voxelizers and both spatial-filter settings. This supersedes the earlier blanket M7 acceptance claim. It does not invalidate the earlier functional tests, and it does not establish M8 performance acceptance.

## Reference and acceptance contract

[`validate_dynamic_voxel_quality.py`](../tools/validate_dynamic_voxel_quality.py) runs seven scenes: an emissive panel, thin wall, slanted wall, alpha cutout, reversed-winding blocker, colored room and Sponza. Each runs at 64³/128³ with compute/geometry voxelization, averaged reflectance, raw/spatial-filtered output and forced `voxel_dda`. Native RT is disabled; Vulkan and synchronization validation are enabled. The executable is the MSVC Release preset. RHI recording is threaded and async compute is enabled for these image captures; the native filtering tests cover all four submission combinations.

The actual exported viewport is **152×96**, with a **128×128 G-buffer**. Windows clamps the requested 128-pixel window width. Capture dimensions, rather than requested dimensions, drive every image comparison. Static runs warm for 64 frames and capture one additional frame; cache readiness and zero fallback flags are mandatory. Configured GI budgets are 6144 MiB at 64³ and 12288 MiB at 128³, not measurements of peak device memory. Sponza has 22,997 / 118,827 occupied cells respectively.

[`voxel_gi_triangle_reference.py`](../tools/voxel_gi_triangle_reference.py) performs CPU triangle intersections using Embree. It loads exported **original vertices, indices and instance transforms**, plus original glTF materials/textures. It does not read voxel ownership, irradiance, hit caches or light masks to calculate reference radiance. The separate coverage and diagnostic checks do read those exports.

The reference uses the captured primary surface position, shading/geometric normals, albedo, metallic value and AO. This deliberately matches rasterized primary surfaces and isolates secondary transport; it is not an independent primary-camera renderer. It evaluates one diffuse bounce, two-sided accepted triangle intersections, alpha continuation, source emission, the engine's analytic light units and sender diffuse factor, and receiver Fresnel/metallic/AO weighting. Sender normal mapping is omitted to match the current sender contract. Rays below the primary geometric hemisphere contribute zero. Texture color is decoded from sRGB before bilinear interpolation; linear channels remain linear. Source geometry, textures, reference implementation and numerical contract are hashed before reusing a reference.

Environment lighting is disabled **in both** reference and engine. The room combines one point light with an emissive panel. Sponza preserves the user's four white lights at `(±.55, -.25, ±.13)`, intensity 5, range 1000, and freezes the blue fifth light at `(1,1,0)`, intensity 2, range 4. The comparison reads only linear HDR `diffuse_outgoing`, excludes background and directly visible emission, and excludes direct/specular lighting and markers. Component-sum checks still validate the exported capture. Existing M6 environment/lighting-extension tests retain their separate scope.

Each reference uses two independently scrambled, cosine-weighted integrations of 2048 samples per pixel. The replica-disagreement estimate is 0.50% for the room, 1.88% for Sponza and at most 3.10% across these fixtures, below the locked 4% reference threshold. The runner increases samples up to 8192 per replica if necessary and fails the reference gate if it cannot converge. This estimate measures sampling disagreement, not a rigorous bound on systematic reference error. Primary and secondary offsets are fixed at 0.0006 / 0.00001 world units, independent of voxel resolution; the primary offset accommodates the captured FP16 position precision.

[`voxel_gi_quality_limits.json`](../tools/voxel_gi_quality_limits.json) was frozen **before the first new GPU capture**, with SHA-256 `0a5dee38ffa6c46a4c232ffc1c7bcb43e5c483e53e0c7521ac2818e0a6d4abb0`. This closes the missing tolerance definition now; it was not completed retrospectively in M0. Limits were not relaxed after the Sponza failures.

| Gate | 64³ | 128³ |
| --- | ---: | ---: |
| Fixture image relative RMSE | ≤40% | ≤30% |
| Room/Sponza image relative RMSE | ≤60% | ≤50% |
| Absolute relative mean RGB bias | ≤35% | ≤25% |
| Analytical umbra mean / maximum | ≤1% / 3% of fullscale | Same |
| Added mean umbra brightness from spatial filtering | ≤0.5% of fullscale | Same |
| Stationary relative RMS / final convergence error | ≤0.1% | Native filtering also tested |
| Temporal decay error | ≤0.3% of initial fullscale | Native filtering also tested |

These are declared engineering regression limits for coarse voxel visibility. The fixture allowance accounts for the previously recorded 22.89% panel baseline. The broader scene allowance admits occupied-cell and finite-direction error; it is not a claim of perceptual equivalence to triangle visibility. Relative RMSE is `||actual-reference||₂ / ||reference||₂`; signed mean bias is `sum(actual-reference) / sum(reference)` over selected RGB pixels.

## Original baseline image results

Compute and geometry voxelizers produce byte-identical diffuse captures for every matched profile below. The reference masks are identical across resolutions. Full per-region statistics, including floor/ceiling and axis-facing walls, are retained in each `.quality.json`.

| Scene | Grid | Raw relative RMSE | Spatial relative RMSE | Raw mean bias | Spatial mean bias | Gate |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Emitter | 64 | 29.76% | 28.04% | +27.41% | +27.41% | Pass |
| Emitter | 128 | 24.61% | 23.22% | +18.79% | +18.70% | Pass |
| Thin wall | 64 | 26.08% | 24.77% | +22.43% | +22.36% | Pass |
| Thin wall | 128 | 22.92% | 21.67% | +15.10% | +15.52% | Pass |
| Slanted wall | 64 | 25.06% | 24.09% | +19.55% | +20.24% | Pass |
| Slanted wall | 128 | 22.70% | 21.60% | +14.37% | +14.95% | Pass |
| Alpha cutout | 64 | 26.00% | 24.43% | +16.99% | +16.85% | Pass |
| Alpha cutout | 128 | 24.75% | 22.94% | +12.91% | +13.51% | Pass |
| Backface blocker | 64 | 25.45% | 23.80% | +18.89% | +18.92% | Pass |
| Backface blocker | 128 | 22.14% | 20.85% | +12.65% | +13.55% | Pass |
| Colored room | 64 | 52.73% | 53.14% | −18.73% | −18.93% | Pass |
| Colored room | 128 | 15.96% | 15.94% | −14.47% | −14.62% | Pass |
| Sponza | 64 | **99.65%** | **99.80%** | **−99.91%** | **−99.94%** | **Fail** |
| Sponza | 128 | **63.77%** | **63.47%** | **−63.44%** | **−64.25%** | **Fail** |

The independent V0 polygon-clipping and material-alpha oracle checks all five panel fixtures at both resolutions/producers. There are **zero missing, extra or wrongly owned cells outside its established precision bands**. It retains the original `1e-5` grid-coordinate boundary allowance and `.01` alpha-cutoff ambiguity band; these are not fitted to the GI images. At 128³ the cutout has 104 alpha-ambiguous candidate cells, explicitly recorded. Correct cell coverage does not imply correct triangle visibility through the occupied cells.

The thin/slanted/backface fixtures expose an analytically fully occluded receiver region: every segment to every emitter corner crosses the opaque blocker interior. No Monte Carlo “zero-hit” assumption defines this region. Fullscale is `4 × .7 × .96 = 2.688`. Across these cases, maximum umbra brightness is **0.01635% of fullscale** and maximum added mean leakage from spatial filtering is **0.002312%**. Both pass their separate gates. This does not establish absence of leakage in arbitrary geometry.

## Original Sponza failure diagnosis

All four static light positions fall in occupied cells at 64³. Their cached visibility bits are set for only **one occupied cell per light**, out of 22,997. Independent triangle segments from visible primary surfaces reach those lights from **4,792 / 7,340 / 4,848 / 7,769 pixels**, respectively. Those segments are a labeled direct-visibility diagnostic, not a substitute for the one-bounce reference above.

At 128³ the four light cells are empty; their visibility bits cover 975 / 950 / 1,143 / 1,112 occupied cells. The substantial image error remains. The 64³ evidence, together with `static_light.glsl` tracing to each finite light position and DDA treating occupied cells as solid, identifies false occlusion at light endpoints as a significant contributor. It does not prove that every 128³ error has the same cause. All captures have matching scene/voxel generations, fully ready caches and zero fallback flags; temporal stability also passes. Neither validation overhead nor a cone fallback explains this lighting failure.

**Work identified by the baseline (now addressed in the linked correction report):** improve finite-light visibility near occupied cells and quantify the remaining 128³ occlusion/material/directional error. Any endpoint treatment must preserve genuine blockers, cutouts, backfaces and finite-segment correctness. Simply ignoring every light's destination cell could introduce leakage and has not been adopted. Re-run the locked suite after a correction. Until Sponza meets its limits, keep M7 quality acceptance and automatic promotion of directional GI open. No production GI shader behavior or user lighting configuration was changed by this validation task.

## Temporal validation

[`QualityConvergenceStationaryVarianceAndLightRemoval`](../ZenSamples/CommonTest/DynamicVoxelGIFilterTests.inl) adds a native GPU test with fixed and elapsed temporal modes at a controlled 60 Hz, spatial filtering enabled, and all four RHI/queue combinations. It starts from valid black history, enables a constant source, records startup at frames 1/32/128/320, then measures 32 stationary samples. It removes the light and checks frames 1/10/32/64/128/320 against the independent `0.97^frames` response. Existing rate-dependence, padding, motion/cooldown and revealed-static tests run alongside it: **28/28 pass**.

At frame 320, normalized startup energy is approximately `0.99994147`; normalized residual energy 320 frames after removal is approximately `0.000058472`. Stationary RMS is `1.03e-5` of fullscale, below `0.001`. A three-frame image is not used as evidence of convergence.

For both voxelizers, the room and Sponza also have frozen full-image captures after 320/352/384 frames with fixed temporal and spatial filtering enabled. Each capture includes one additional render frame. Their primary surfaces match exactly, their diffuse images are identical across the three samples, and their final images match the spatial-only baseline exactly. These checks establish stability, not accurate Sponza lighting. Dynamic/deforming camera lifecycle evidence remains in the existing M4/M5 reports; these new image sequences are stationary controls.

## Reproduction and evidence

Install the pinned dependencies in an isolated environment, build the Release demo and native test, then run:

```powershell
python -m pip install -r tools/requirements-gi-quality.txt
python -m unittest discover -s tools -p test_voxel_gi_triangle_reference.py -v
python tools/validate_dynamic_voxel_quality.py --temporal-scenes
```

The original quality run returned **exit code 1** for the eight Sponza failures; the corrected full run returns **0**. `--compare-only` evaluates saved images; `--refresh-reference` explicitly rebuilds references after source changes; `--temporal-only` reuses existing spatial baselines. The runner preserves `Data/engine.cfg` byte-for-byte and refuses to overwrite external configuration changes.

Run the native suite with `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~`, and `DISABLE_RTSS_LAYER=1`:

```powershell
build/x64-windows-msvc-release/bin/DynamicVoxelGIIntegrationTest.exe '--gtest_filter=*QualityConvergence*:*FilteringPreserves*:*TemporalStep*:*GaussianUses*:*FilteredProvider*:*DynamicCooldown*:*RevealedStatic*'
```

Artifacts: `build/dynamic-voxel-m7-quality-20260925/`. `scenes/results.json` contains 56 image profiles and four temporal summaries; 68 scene HDR captures are retained. Each reference stores linear `.rgb.f32`, its `.mask.u8`, settings/hash metadata and per-region results. Each quality PNG shows reference / engine / absolute error on the same display scale; linear data drives acceptance. Original and corrected reference runs are retained separately. `native-filter-tests.json`, `oracle-tests.log`, `final-quality-checks.log`, `final-temporal-checks.log`, `sponza-diagnostics.log` and `final-verification.json` record the checks. No application, VUID, synchronization or CPU-leak failures were observed in the captures/native suite.

Scope remains one GPU (RTX 5080), two grid sizes, one Sponza camera, averaged reflectance and an environment-off triangle comparison. No author executable was reproduced, no full environment-enabled scene accuracy is claimed, and no performance/promotion result is inferred from these runs.
