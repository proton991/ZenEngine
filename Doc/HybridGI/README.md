# Hybrid GI P0–P3 implementation record

2026-10-07. Implements the receiver, voxel-ray sky, reconstruction and specular-occlusion path in [the plan](../HardwareRayQueryEnvironmentLightingPlan.md). **The phase acceptance gates remain open.** The table below distinguishes working code and measured results from the remaining requirements.

## Delivered behavior

| Phase | Implementation |
| --- | --- |
| P0 | Frozen limits, top/hall camera matrices and platform matrix in `baseline.json`; generated fixture catalog; independent Embree sky, uniform-albedo one-bounce and GGX visibility references; linear receiver/signal/environment captures; reproducible Sponza runner and error comparison. Since 2026-10-09, a full-image Mitsuba 3 sky ground truth (`tools/ground_truth_mitsuba.py`), stored since 2026-10-10 as a dataset that `tools/ground_truth_sweep.py` compares with fresh engine captures; see the plan's [Ground truth](../HardwareRayQueryEnvironmentLightingPlan.md#ground-truth) and [P4 results](P4.md#ground-truth-comparison-2026-10-09). |
| P1 | Geometric normal and node/facing identity, previous clip position (since 2026-10-08 stored as NDC motion; see [P4](P4.md#review-fixes-2026-10-08)) and node transforms, opaque/mask forward receiver prepass, persistent histories per logical `RenderView::historyId`, resize/scene/failure invalidation, successful-execution publication, and existing render-graph submission-history ordering for imported textures. Geometry/deformation/opacity revisions invalidate all history. |
| P2 | Decorrelated cosine sampling with an explicit PDF, base-level voxel traversal including occupied starts and entry from outside the volume, level-zero environment lookup, separate sky/cone-bounce composition, and 64-sample owner-surface voxel sky injection. |
| P3 | Bilinear history reprojection with identity/plane/normal rejection; unclamped sky moments; roughness/view-dependent specular history; five 5×5 à-trous iterations with propagated variance; one GGX visible-normal ray for environment specular occlusion, drawn from the lobe `f * cos` above the shading normal (see [Specular occlusion horizon fix](#specular-occlusion-horizon-fix)). |

`auto`, `voxel`, and an explicit `hardware` request currently resolve to the compute provider. `hardware` logs its P4 fallback. No acceleration structures or ray-query shader variants are introduced. `legacy` remains available because the complete P3 acceptance gate has not passed.

The default is **four diffuse rays, one specular ray, 32 history frames and five filter iterations**. Two diffuse rays missed the top-floor 99th-percentile limit (20.94% versus 20%); four passed. This is an explicit sample-count change, not a relaxed tolerance or a P8 performance-qualified preset. Metadata labels it `custom`.

Cone bounce remains deterministic and is evaluated without its sky branch for covered receivers. It is not temporally reconstructed. `H` is zero. Bounce-history/moment allocation and the responsive lighting policy therefore await the stochastic bounce/reflection signals in P5/P6. Forward clearcoat, sheen and other environment lobes share the base-lobe `S`; this is an approximation. Translucent, transmissive, scattering and unsupported-topology layers retain the existing per-surface path.

## Resources and invalidation

PBR/legacy retain the 24-byte G-buffer with D32 depth. Hybrid adds RG32_UINT packed geometric normal/identity (8 bytes) and RGBA32F motion (16 bytes; NDC motion and previous w since 2026-10-08). RGBA32F raster position adds another 16 bytes for the G3 precision fix (2026-10-10), for **64 bytes/pixel**. Hardware rays use this position; the voxel tier retains depth reconstruction, although both hybrid tiers allocate the shared layout. The initial forward prepass shares this complete layout, including albedo/emission targets not needed by reconstruction.

Each logical view owns two sides of seven histories: sky/nu RGBA32F, H/S RGBA16F, moments RGBA32F, length/rejection/roughness RGBA16F, position RGBA32F, normals RGBA16F and receiver RG32_UINT. This is **160 bytes/pixel**, or 316.4 MiB at 1080p, excluding transient textures, allocator overhead and retired resources. The raw stage, the sky-filter guide and five filter stages declare another 144 bytes/pixel before render-graph reuse. The hardware tier now declares seven additional RGBA32F bias-correction stages (112 bytes/pixel before reuse); this recovers a coarse geometry-safe temporal-minus-filtered residual without feeding it back into history. Diagnostic capture adds 208 bytes/pixel plus readback only when requested. These are format-based storage figures, not measured peak residency.

Imported histories use the existing stable-resource submission history, which orders against outstanding readers as well as writers. Publication does not use swapchain image indices or CPU frame parity. `historyId=0` preserves the single-view API; additional independent views must use distinct stable IDs. Scene rebinding retires every view's histories, including when the new scene occupies the same CPU address. Failed execution retires unpublished resources so a canceled initial clear is never reused as initialized data. Allocation failure selects and reports the minimum PBR path.

Camera projection changes, displacement above 0.25 normalized world units, or rotation above 60 degrees reset history. Smaller movement uses reprojection; `S` also rejects roughness changes and view changes. Environment generation, enablement, provider, sample count, reference mode, bias, temporal controls and history-limit changes invalidate incompatible history. Analytic-light and albedo-only changes retain unmaterialed sky history; existing radiance injection responds independently. Any geometry/opacity/deformation revision resets all receiver history, including static surfaces while an object animates.

The filter additionally preserves the boundary between occupied and empty biased start cells. Without this guide, filtering smears lit floor values into the occupied starts at wall bases. This deliberately retains D2 voxel-geometry error; P4 must replace this provider-specific guide when triangle visibility is used.

## Evidence

Windows, RTX 5080, MSVC 19.51 Debug, driver raw version `2588114944`, Vulkan SDK 1.4.357.0. Native tests ran with synchronization validation and implicit layers disabled. Reports and binaries are in `build/hybrid-p3-final/` and `build/hybrid-*.log`; these generated archives are ignored by Git. Small result summaries are retained in `verification.json`.

| Check | Result |
| --- | --- |
| CommonTest / ConfigLoaderTest / RenderCoreTest | 112 / 21 / 570 passed. Expanded history regression additionally passes resize, independent view ownership, failed execution, sample changes, environment rotation/intensity and same-address scene rebinding. |
| VulkanRHITest | 48 passed. |
| ConeVoxelGIIntegrationTest | 16 passed across inline/threaded and graphics/async modes, including four hybrid estimator cases. No native offscreen validation messages. |
| Shader validation | All 112 SceneRenderer/VoxelGI SPIR-V variants validated for Vulkan 1.2. |
| CPU reference analytic tests | Open plane, half-wall, closed hemisphere, empty/uniform bounce, GGX normal incidence/lobe fraction, open/closed specular occlusion, and continuing past a rejected alpha candidate pass. |
| Deferred and forward constant-environment open plane | Mean sky 0.998884 versus 1; mean reconstructed `S` 0.998877 versus 1; geometric normals match the plane. |
| Deferred and forward closed box | Raw/reconstructed sky and reconstructed `S` exactly zero. |
| Static receiver reprojection | Maximum 0.00166 pixel on the open/closed fixtures, below 0.01. This does not establish the moving-camera requirement. |
| Lighting composition | Combined equals direct + diffuse + specular + emission; sky + bounce equals diffuse within 2.99e-8 on these fixtures. |
| Legacy top camera | Entire seven-component float capture byte-identical to the pre-change baseline. |
| Environment on/off/on | Separate frozen top-camera runs: sky exactly zero when disabled; both enabled captures byte-identical. Live environment invalidation is separately covered by the history regression. |
| Explicit hardware request, RTX 5080 RT enabled | Reports voxel fallback and renders successfully; this is not hardware-query validation. |
| Moving occluder/light fixture, 64 frames | Geometry generation advances; every covered receiver has history length 1 and reset reason `geometry_or_opacity`. The scene remains finite and composition error is below 1.2e-7. |
| Profiling tools | All 15 verifier tests and native hybrid/legacy capture verification pass. |

The shipping comparison uses exactly **64 successfully executed frames**, four diffuse rays, 32-frame history, 960×540, 64³, no analytic lights and zero indirect intensity. The floor region is fixed by captured geometric normal Y > 0.99 and normalized position Y < -0.12. RGB errors are normalized by the region's reference RGB mean; no display-image thresholds are used.

| Region | Mean bias | RMS | P99 absolute error | Unexpected zeros | Result |
| --- | ---: | ---: | ---: | ---: | --- |
| Top floor, 52,404 pixels | -0.089% | 5.263% | 18.041% | 0 | Pass |
| Hall floor, 110,437 pixels | -0.233% | 3.860% | 13.922% | 0 | Pass |
| Top, all receivers | -0.162% | 4.575% | 14.451% | 0 | Pass |
| Hall, all receivers | -0.561% | 14.555% | 58.483% | 8 | **Fail** |

The voxel oracle averages 1024 rays/pixel/frame over 65 static frames, with same-pixel FP32 accumulation and no reprojection or spatial filter. The diagnostic output retains each frame's raw estimate as well as the running mean. A single 1024-ray floor reference differed from a 4096-ray estimate by 11.84% RMS, so it was too noisy for the 8% shipping test. Additional reference frames improve the oracle without changing acceptance limits. The reference is the same voxel provider; the independent CPU triangle oracle is a separate comparison and includes D2 geometry differences.

At 1920×1080, 64³, the frozen top camera, environment only and no bounce, 63 measured static frames with validation enabled: median GPU frame was 6.484 ms hybrid versus 4.403 ms legacy; P95 was 6.928 versus 4.779 ms. G-buffer pass median rose from 0.257 to 0.393 ms. Per-pass intervals include barriers and may overlap; their sums are not a GI-exclusive duration. This is a measured comparison, **not** evidence that the P0 GI-exclusive performance or peak-memory targets pass.

## Specular occlusion horizon fix

2026-10-07, after the P3 runs above. The first P3 trace counted every visible-normal direction below the surface as blocked. The prefiltered map and split-sum terms integrate only above the shading normal, so an unoccluded rough surface lost that fraction of environment specular: 0.5 at roughness 1 and normal incidence, about 0.07 at roughness 0.5. The open-plane fixture showed mean `S` 0.9266 with nothing overhead. The CPU reference used the same definition, so comparing against it could not find this.

`HybridSpecularDirection` in [hybrid_sampling.glsl](../../Data/Shaders/VoxelGI/hybrid_sampling.glsl) now samples `f * cos` above the shading normal. It rejects below-horizon candidates and accepts the rest with probability `G1(l)`, because the visible-normal density is `f * cos / G1(l)` under separable Smith masking. At most 16 candidates are drawn; the fallback is the first above-horizon candidate, used for at most 0.3% of samples (roughness 1, normal incidence). Directions above the shading normal but below `n_g` remain blocked. `environment_reference.py` computes the same `G1`-weighted fraction.

| Check | Before | After |
| --- | --- | --- |
| GPU estimator, empty volume, roughness 1 at normal incidence | 0.500 | 1 exactly |
| GPU estimator, empty volume, roughness 0.5 at a 60° view | 0.930 | 1 exactly |
| Closed shell | 0 | 0 |
| Open-plane fixture, deferred and forward, mean reconstructed `S` | 0.9266 | 0.9989 |
| Closed-box fixtures, deferred and forward | 0 | 0 |

The GPU values come from `HybridEstimatorNormalizesAndClosedGeometryBlocksSkyAndSpecular`, which passes in all four submission modes and fails on the previous estimator with the values in the *Before* column. `validate_hybrid_gi.py` now requires the open-plane mean `S` to lie within 0.5% of 1, the analytic-fixture limit. The remaining 0.11% deficit is not from the estimator: a 1024-sample reference capture gives mean raw `S` 0.999987. It comes from rare grazing rays that fall between the encoded shading and geometric normal horizons (normal dot product 0.99998 on the plane), which the spatial filter spreads; with the filter off the reconstructed mean is 0.99972. The sky channel shows the same small effect. The Sponza comparisons above measure sky only and are unchanged.

## P4 continuation

The hardware provider is now implemented. [P4 implementation and verification](P4.md) supersedes the earlier hardware-fallback and RTX compute-only status above. Both frozen Sponza floor regions pass the shipping sky comparison with no unexpected zeros; the hall's full-image reconstruction still fails. Earlier P0–P3 measurements remain historical results, not hardware-tier acceptance. Open P4 gaps and their fix steps are listed in [P4 gaps](P4Gaps.md). The 2026-10-10 execution, including corrected independent references and unsuccessful estimator probes, is recorded in [P4 execution results](P4Execution.md).

The [hotel-room noise follow-up](P4.md#hotel-room-noise-2026-10-08) adds environment importance sampling to both active visibility providers and the voxel sky cache. It supersedes pure-cosine sky sampling above. The [hall reconstruction follow-up](P4.md#hall-full-image-reconstruction-2026-10-09) changes the proposal split to three environment samples per cosine sample, stratifies per-pixel rotations as blue noise, and filters sky as a ratio to the unoccluded irradiance. The Papermill floor checks still pass; the more demanding hotel-room environment has substantially lower noise but still exceeds some shipping error limits.

## Remaining acceptance work

- Native Sponza motion/cut, deformation and alpha-edit sequences now have frame 1/4/8/32 captures and matching references; several 32-frame regions fail. Continuous deforming animation, every-frame moving-shadow coverage, thin-wall filtering and multi-view native image validation remain incomplete. See [P4 execution results](P4Execution.md).
- The hall's non-floor receivers fail the shipping limits. Voxel occupied-start artifacts remain by design; triangle geometry in P4 is needed for D2. `legacy` cannot yet be removed.
- The original reported camera has only a saved position. Its exact rotation/projection cannot be recovered; top/hall are reproducible alternatives, not a claim to reproduce that exact view.
- CPU triangle references support static uncompressed glTF/GLB triangle geometry, alpha masks and texture transforms. Sparse/deformed/compressed geometry fails explicitly. Cubemap edge filtering clamps within a face. The one-bounce oracle uses uniform albedo and one explicit point light; it is not a general material path tracer. Full-resolution CPU references and the complete 64/128/256, 540p/1080p matrix have not all been run.
- The existing voxel coverage contract excludes blend/transmission occluders. Full-scene opaque treatment from the new lighting contract is not supplied by this compute volume. Ownerless sky-cache fallback still uses the voxel center/normal. Capture metadata now exports evaluated and center-fallback counts; the 64³ Sponza measurement is 0 / 22,997 on both providers. This measures this scene, not the absence of fallback in all geometry.
- `voxel_gi_quality`, ray-hit bounce/reflections/shadows, and a total GI memory preflight cap are not implemented. Quality tables/default memory budgets depend on P8; current reflectance/shadow preflights do not constitute the planned total cap. The forward prepass and history footprint need budget/performance work before general promotion.
- RX 7900 XT, Apple Silicon/MoltenVK and a non-RT device were unavailable. No result for those matrix entries is claimed. RTX 5080 hardware-provider results and the remaining P4 gates are recorded separately in [P4](P4.md).
- Presentation captures report the previously recorded `SYNC-HAZARD-PRESENT-AFTER-WRITE`, reproduced in `legacy` too. The fixture runner fails it by default; `--allow-present-baseline` reports only that exact known hazard separately. The broad Vulkan integration run reported nine presentation tests plus one window-size expectation failure, then terminated before a complete suite result. It is not a passing validation run.

## Reproduction

Build `scene_renderer_demo`, `RenderCoreTest`, `CommonTest`, `ConfigLoaderTest`, `ConeVoxelGIIntegrationTest` and the Vulkan tests using the normal CMake configuration. Install `tools/requirements-gi-quality.txt` for the Python tools. All runners restore `Data/engine.cfg` unless another process edited it; run config-mutating runners sequentially.

```powershell
python tools/environment_reference.py --self-test
python tools/validate_hybrid_gi.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --output build/hybrid-fixtures --fixtures open_plane closed_box forward_open_plane forward_closed_box
python tools/capture_hybrid_gi.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --scene PATH/TO/Sponza/glTF/Sponza.gltf --output build/hybrid/top --camera top
python tools/capture_hybrid_gi.py --exe build/x64-windows-msvc-debug/bin/scene_renderer_demo.exe --scene PATH/TO/Sponza/glTF/Sponza.gltf --output build/hybrid/top-reference --camera top --reference-samples 1024
python tools/compare_hybrid_gi.py build/hybrid/top build/hybrid/top-reference --output build/hybrid/top-error.json
python tools/environment_reference.py --scene build/hybrid/top.gltf --capture build/hybrid/top --samples 4096 --stride 64 --output build/hybrid/top-cpu.npz
```

Use `--stride 1` for every captured receiver. `--bounce --bounce-albedo .5 --light-position X Y Z --light-intensity R G B` adds the CPU uniform-albedo bounce reference. The generated fixture catalog includes point-light, glossy-floor and moving-object/light inputs for subsequent phases. The outside-volume fixture still requires an explicitly smaller voxel bound to test out-of-grid occluders; the native provider test separately covers a ray entering the volume from outside.

Capture controls implemented now: `voxel_gi_ray_provider`, `voxel_gi_samples=1|2|4`, `voxel_gi_history_frames=1..256`, `voxel_gi_temporal`, `voxel_gi_filter`, `voxel_gi_specular_occlusion`, and diagnostic `voxel_gi_reference_samples=0|1024|4096`. Reference mode averages up to 256 static frames and resets on any camera-matrix change. Existing lighting, bias and cone-bounce settings remain applicable.

The seven-component `.lighting.bin` format remains 112 bytes/pixel; metadata version is 2. `.hybrid.bin` contains thirteen float4 components (208 bytes/pixel), named in metadata. Identity in component 12 is uint bits carried in a float slot, not a numerically converted float. `.environment.bin` stores the captured prefiltered level-zero cubemap in +X, -X, +Y, -Y, +Z, -Z face order. `.inputs.json` fingerprints the source glTF, external buffers/textures, executable, effective configuration and every SPIR-V shader. `.profile.json` records device, driver, build and timing conditions.
