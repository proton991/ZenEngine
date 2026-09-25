# M7 Stage A functional acceptance

Date: 2026-09-25. **M7 functionality and the locked quality matrix pass.** Stage A functional tests accept approximate compute visibility at 64³/128³ and budget rejection/fallback at 256³. The [initial quality validation](DynamicVoxelGIM7QualityVerification.md) exposed eight Sponza failures; the [quality corrections and rerun](DynamicVoxelGIM7QualityFixVerification.md) now pass 56/56 image profiles and all four stationary sequences. M8 performance acceptance remains open. [Implementation plan](DynamicVoxelGIImplementationPlan.md), [M6 baseline](DynamicVoxelGIM6Verification.md).

**Follow-up audit:** three missing contracts were subsequently found: unchecked cone allocation after directional budget rejection, no runtime material publication, and automatic voxel-grid expansion. The [gap verification](DynamicVoxelGIGapVerification.md) records their fixes and additional acceptance checks. Budget fallback is now cone only when its retained resources fit, otherwise PBR; runtime material and opacity edits propagate; leaving the fixed grid selects PBR until return or an explicit bounds rebuild. Counts below describe the original M7 run, not the follow-up.

## Delivered behavior

Explicit `dynamic_voxel` now runs the existing directional pipeline at 64³ and 128³. The renderer and active resource preflight use the configured shared grid instead of a hard-coded 64. If `voxel_resolution` is absent, the shared loader defaults to 64 for explicit directional GI and 256 for cone/auto. This fixes the previous mismatch between the directional settings default and the voxelizers' default. Explicit 64/128/256 requests remain unchanged.

`RendererServer::SetVoxelGIMethod` selects `cone`, `dynamic_voxel` or `auto` between frame recordings. Changes invalidate filter history while retaining compatible static intersections. The existing all-scene voxelizer prepares cone output from the current scene generation, including changes while directional GI was active. The grid remains fixed at startup; this stage adds no runtime grid-resize framework. Compatible resources remain resident for switching and fallback and are included in the active budget. This is a deliberate retention policy rather than retiring/reallocating them on every method toggle. `auto` remains cone until the M8 profiling decision.

Static DDA cache identity now also includes static coverage, traversal limits and the static reflectance policy. Changing these query settings invalidates cached visibility even when the static geometry generation is unchanged; ordinary dynamic motion still preserves static intersections. A complete/incomplete/truncated/complete native sequence verifies invalid data and subsequent recovery without stale hits or visible-sky assumptions.

The active preflight accepts an explicit byte count for older allocations awaiting retirement. It includes those bytes with class/cone resources, frame storage and receiver attachments before reserving any static hit records. Checked addition prevents overflow even when class resources plus retirement fit but adding frame storage would overflow. CPU boundaries cover all three resolutions, the exact one-receiver minimum, one byte below it, descriptor/RHI limits and overflow. A native test queues the previous GI graph, rejects a replacement before any completion wait or allocation, and then reads the retained previous output successfully.

Diagnostic reference-response indices use checked 32-bit arithmetic. Wide-grid logical query ranges that cannot be represented produce `Unknown` instead of wrapping into unrelated responses. DDA does not use those response indices. The combined raw-volume capture rejects unsupported widths before calculating its byte offsets: 64/128 fit the current readback layout; 256 does not fit the RHI's 32-bit buffer-size type.

The sample adds `--disable-rt`, using the existing RHI startup switch before device creation. All 111 M7 scene captures use it and reject logs enabling acceleration-structure, ray-query or RT-pipeline extensions. Native GI tests already disable RT. All 30 directional shader modules validate for Vulkan 1.1 without native RT constructs. This stage adds no acceleration structures, hardware ray queries or native RT commands.

## Verification

RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, MSVC 14.51.36231; Debug builds. Native/scene execution uses Vulkan validation and synchronization validation. The affected six targets build successfully. Changed C++ follows the global rules and project clang-format configuration.

| Gate | Result |
| --- | --- |
| CPU suites | 573 passed; 7 existing disabled RenderCore tests |
| Native GI | 104 passed, including 12 new cases across four submission modes |
| Omitted-resolution default | 1 capture passed at the actual shared 64³ grid |
| Wider grids and budget fallback | 22 captures passed |
| Runtime method switching | 72 captures passed |
| Isolated triangle-reference fixtures | 12 captures passed the rendering checks; geometric errors below |
| Sponza | 4 captures passed, both voxelizers, raw/full filtering |
| Static scene regression | 35 captures passed |
| V0/V1 regressions | 128 class/lifecycle captures and 2 fixed-grid 128³ material/G-buffer captures passed |
| Legacy images | 158 GPU cases and image assertions passed |
| Directional SPIR-V | 30 modules validated, no RT constructs |

The 64/128 matrix covers geometry/compute voxelization, inline/threaded recording and async compute off/on. Raw HDR is byte-identical across those combinations at each resolution. Additional filtered captures cover both voxelizers. The 256³ request with a 3072 MiB cap is rejected before directional allocation; its output is byte-identical to explicit cone at 256³. Successful directional rendering at 256³ is **not verified**.

Each method-switch run captures initial, moved, deformed, teleported, removed, restored, camera-changed, resized and returned states. Dynamic/cone/auto selections alternate, output metadata agrees with the scene geometry generation, and compatible static-cache batch counts remain unchanged. Initial/restored/returned HDR matches exactly; each state matches across all eight voxelizer/submission combinations. The existing static/legacy suites retain direct-shadow, marker, mode 1/2/3, normal-map, mirrored geometry, odd/upscaled extent and minimize/restore coverage.

New native 128³ coverage places receivers/senders near the volume boundary, verifies averaged-material emissive and environment energy with an independent occupied-box oracle, enables filtering, then removes the dynamic sender while retaining the static cache. All earlier provider, gather/composition, dispatch, filter/history, light-mask, environment, emission and failure cases remain in the native suite. The M7 scenes contain 389,844 ready directional pixels, 14,923 independent diffuse composition probes and 8,442 padding checks. They use the existing numeric precision bounds; no rendering tolerance was relaxed.

The V0/V1 rerun covers geometry and mixed-material class/union outputs through eight lifecycle states and eight voxelizer/submission combinations, then 128³ materials, averaged reflectance and G-buffer attributes for both voxelizers. An additional run accidentally omitted the documented `--grid-percent 100`: raw coverage, owner attributes and averaged reflectance passed, but seven padded-grid G-buffer samples per producer exceeded the 0.016 emission comparison threshold (0.01758–0.01953). This is the same extra-probe limitation already recorded in [V1 verification](VoxelReflectanceVerification.md). It remains in `materials128/` and is not counted as passing. The prescribed fixed-grid run in `materials128-fixed-grid/` passes without code or tolerance changes.

### Resource observations and configuration

The active layout still uses six decoded 96-byte hit buffers: 72 KiB per reserved static receiver at 128 rays per face. Following [light-mask reuse](DynamicVoxelGILightMaskReuse.md), frame storage is `456*N³ + 48 + 32*ceil(N³/64)` bytes; the extra 16 bytes retain per-light unknown-query status across reused frames. Class/cone transition storage, V1 resources and receiver attachments are counted separately. M0's proposed packed-layout estimator is not the allocation model used by this renderer. Packing and measured device-memory profiling remain M8 work. The capture table below records the original M7 run.

| Capture | Grid | Cap (MiB) | Occupied static | Selected static | Reserved receivers | Cache batches |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Four-mesh fixture | 64³ | 3072 | 2500 | 1824 | 41478 | 11 |
| Four-mesh fixture | 128³ | 3072 | 10000 | 7296 | 26290 | 7 |
| Sponza | 64³ | 6144 | 22997 | 5023 | 85169 | 21 |

These are fixture caps and allocation/counter observations, not measured total GPU memory or shipping presets. The renderer currently initializes the reserved cache capacity in batches even when occupancy is smaller. Captures wait until the cache is ready; no cold-start performance claim follows from them. Sponza uses a 256² G-buffer, 320×180 output, camera `(0.55,-0.15,0)`, five frozen point lights and environment lighting. Raw/full-filter captures have 55,108 ready directional pixels each. Their maximum diffuse composition error is 0.003277, inside the existing interpolation/format bound. The 24-frame Sponza captures test valid filtering, not convergence; M5's controlled elapsed/fixed-time step-response tests establish temporal behavior.

Use the controls in [engine.example.cfg](../Data/engine.example.cfg). The verification runners own `Data/engine.cfg` serially and restore its exact bytes. Owner reflectance remains the compatibility default. Explicit averaging, directional GI and a positive total budget remain opt-in. Unsupported profiles and unready/invalid data use the checked cone reserve; exhausted budgets or incomplete scene coverage select PBR. Direct analytic shadows in GI mode remain mesh shadow maps independent of voxel resolution. Background sky visibility remains independent of environment illumination.

### Triangle-reference error

The runner traces the same 128 cosine-weighted directions from each sampled +Z receiver-face origin against original triangles using independent, double-precision, two-sided Möller–Trumbore intersections. It compares raw red irradiance against `pi * mean(first-hit emission)` for an emissive slanted panel, with no blocker, a thin blocker, or a slanted blocker. Analytic/environment terms and filtering are off; sender emission is 4. This is an isolated one-bounce triangle reference, not a full-scene reference renderer or a reproduction of the author's hardware implementation.

| Fixture | Grid | Probes | Irradiance RMSE | Relative RMSE | Mean signed error |
| --- | --- | ---: | ---: | ---: | ---: |
| Emissive panel | 64³ | 69 | 0.182003 | 22.89% | +0.141917 |
| Emissive panel | 128³ | 280 | 0.136965 | 18.92% | +0.096829 |
| Thin blocker | 64³ | 67 | 0.149171 | 21.61% | +0.068622 |
| Thin blocker | 128³ | 274 | 0.116786 | 17.74% | +0.072491 |
| Slanted blocker | 64³ | 67 | 0.144316 | 21.17% | +0.027609 |
| Slanted blocker | 128³ | 274 | 0.111119 | 16.92% | +0.061396 |

Geometry and compute give identical values. Relative RMSE is `sqrt(sum(error²)/sum(reference²))`. Wider grids reduce RMSE for these fixtures, but the probes change with resolution; these are neither a universal quality bound nor evidence of triangle-equivalent visibility. There are no exactly dark triangle-reference probes in this set, so its dark-region maximum is recorded as null, not zero. Filtering leakage is measured separately in [M5 verification](DynamicVoxelGIM5Verification.md); the Gaussian filter remains capable of cross-surface leakage.

## Acceptance scope and remaining work

The Stage A checklist draws on V0/V1 coverage/material oracles, M0 HDR/packing/dispatch boundaries, M1 provider contracts, M2 class and failure/lifetime tests, M3 interpolation/composition, M4 dynamic transport/receiver selection, M5 filtering/history, M6 lighting/material energy, and this stage's wider-grid/switch/default/retirement acceptance. Earlier verification reports preserve their original scope and historical results; this report records the additional M7 runs.

DDA retains cell-shaped indirect occlusion, sub-voxel geometry/material loss, owner normals/emission, point-sampled alpha coverage and explicit `Unknown` for incomplete traversal. Gaussian filtering can leak. Current specular IBL remains separate and unoccluded. Only one GPU/driver is verified. Grid resolution is fixed at startup; accepted large-budget 256³ directional rendering and full-volume capture are unverified/unsupported respectively. Native allocation-failure recovery remains deferred. No multi-bounce or glossy GI is claimed.

M8 profiling has subsequently started, but automatic directional-GI promotion remains blocked by the failed Sponza quality gates as well as the remaining performance/memory gates. The [quality follow-up](DynamicVoxelGIM7QualityVerification.md) identifies severe false occlusion around occupied light cells at 64³ and substantial remaining image error at 128³. Its new triangle-image, coverage, leakage, stationary and light-removal checks supersede the limited panel-probe quality evidence in this original report. H0/H1/H2 remain deferred RHI hardware-RT support, triangle-provider completion and hardware profiling.

## Evidence and reproduction

Artifacts are under `build/dynamic-voxel-m7/`: immutable pre-stage `before/`, build and CPU/native logs/JSON, `scenes/*-results.json`, raw HDR/surface/volume exports and saved configs, per-fixture triangle references, regression outputs, and `spirv-validation.json`. Initial diagnostic attempts remain separately logged. Final verification hashes and the patch against the pre-M7 worktree are recorded in `final-verification.json`, `changed-files.json` and `changes.patch` for the accepted stage.

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `ConfigLoaderTest`, `CommonTest` and `VulkanRHITest` in the initialized x64 MSVC developer environment. Run native GI from `bin` with `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~` and `DISABLE_RTSS_LAYER=1`. Run scene/config-owning checks serially from the repository root:

```powershell
python tools/validate_dynamic_voxel_m7.py
python tools/validate_static_voxel_gi.py --output build/dynamic-voxel-m7/static-scenes
python tools/validate_voxel_classes.py --fixture geometry --policy averaged --matrix --output build/dynamic-voxel-m7/classes-geometry
python tools/validate_voxel_classes.py --fixture mixtures --policy averaged --matrix --output build/dynamic-voxel-m7/classes-mixtures
python tools/validate_voxelization.py --fixture materials --resolution 128 --reflectance-policy averaged --gbuffer --grid-percent 100 --output build/dynamic-voxel-m7/materials128-fixed-grid
python tools/validate_voxel_gi.py
```

M7 was run in five serial phases (`defaults`, `triangles`, `sponza`, `grids`, `switches`); `--phase` reproduces them separately. The legacy runner's output directory was redirected to `build/dynamic-voxel-m7/legacy` for this run. Its normal command uses the existing default directory. Do not run config-owning scene tools concurrently with each other or with tests that consume the local config.
