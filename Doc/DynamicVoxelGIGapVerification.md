# Dynamic voxel GI: follow-up contract fixes

2026-09-25. The implementation audit found three gaps in the M7 acceptance claim: cone fallback could exceed the GI cap, runtime material edits had no publication/invalidation path, and geometry updates silently expanded the voxel grid. These contracts are now implemented. [Implementation plan](DynamicVoxelGIImplementationPlan.md), [original M7 evidence](DynamicVoxelGIM7Verification.md).

## Corrected behavior

### Budgeted fallback

`ValidateVoxelConeResources` checks the complete cone reserve before `VoxelGIRenderer::Init`: owner, normal, emission, sky, albedo/radiance mip chains and optional averaged-reflectance scratch/output. Class planning reuses that calculation. With `C=N^3` and `M=sum(mipSide^3)`, cone costs `24*C + 12*M` bytes for owner or `44*C + 12*M` for averaged reflectance, plus supplied retiring bytes. Storage-buffer limits apply to averaged scratch; owner cone has no corresponding per-cell storage buffer.

`RendererServer` checks the retained directional/class reserve when switching methods, or the cone-only reserve on a cold fallback. A rejected directional request selects cone only if this reserve fits the configured cap. Otherwise mode 3 resolves to PBR before allocating cone volumes. Compatible allocations remain resident, as in M7; ordinary updates do not create retiring GI generations. Legacy cone/auto without an explicit total cap retain their existing configuration behavior.

At 64³, averaged cone reserves 14.42857 MiB: a 16 MiB cap rejects directional GI and produces exactly the explicit cone HDR image; a 1 MiB cap selects PBR, leaves the voxel output generation at zero, and produces exactly the explicit PBR HDR image. These are logical resource bytes, not physical device-memory or allocator-peak measurements. Scene inputs, ordinary G-buffer/direct-shadow allocations and diagnostic readbacks remain outside this GI cap.

### Live material publication

Call `RenderScene::UpdateMaterial(materialIndex, data)` between frame recordings, using a material slot from `GetMaterialsData()`. It validates finite factors, supported UV selections and already loaded texture slots. `Update()` publishes a replacement material buffer through the existing upload/deferred-destruction path before raster, shadow and voxel work. It resets lighting history and advances surface revisions separately for static, dynamic and union consumers. Texture loading, topology editing and transparent transport are not added.

Color, metallic and emission edits refresh the affected voxel surfaces and cone radiance without changing compact-list ordering or static visibility revisions. The shared gather asks its visibility provider to resolve current surface attributes from the cached hit's class/cell identity. DDA reloads current normal, albedo, emission and diffuse reflectance; the deterministic decoded provider preserves its supplied hit. Cached positions/distances and hit records remain unchanged. A native test doubles sender emission and observes doubled receiver irradiance with identical cache bytes and no extra cache-build batch.

Opacity-related fields (base alpha/texture/UV, cutoff or alpha mode) conservatively mark the affected geometry class dirty. Both producers rebuild occupancy, lists and dependent visibility, clearing removed contributions. Restoring materials restores the original HDR output exactly for both directional GI and cone.

### Fixed bounds and incomplete coverage

Voxel bounds now belong to `RenderScene` separately from the scene/shadow AABB. Motion updates geometry bounds and class coverage without changing the voxel transform. `GetVoxelCoverageMask` tests the enabled geometry of each class against the actual padded cubic volume; provider queries keep their existing `Unknown` contract for incomplete classes.

If either class leaves the grid, mode 3 uses PBR and retains the requested mode. Returning geometry resumes GI automatically, retaining compatible static intersections. To include outside geometry deliberately, call `SetVoxelBounds` with a finite renderer-world box; voxelizers make it cubic and add one cell of padding. This explicit update invalidates both classes and grid-dependent histories/caches. Resolution remains fixed at startup. Scene bounds include current geometry even when it lies outside the voxel volume, so shadow bounds do not depend on silently moving the GI grid.

The raw class calibration fixture deliberately moves boundary geometry. It now explicitly requests expanded bounds before comparing complete class/union coverage to its oracle. The new contract fixture separately verifies ordinary motion cannot expand the grid.

## Verification

Debug MSVC build on the existing RTX 5080 test machine. Native and scene GPU checks enable Vulkan synchronization validation. The new lifecycle matrix also disables native RT. All config-owning runners execute serially and restore `Data/engine.cfg` byte for byte.

| Check | Result |
| --- | --- |
| Six affected build targets | Pass |
| RenderCore, ConfigLoader, Common and VulkanRHI CPU suites | 574 passed; 7 existing disabled tests |
| Native GI, four submission modes | 108 passed, including four cached-material regressions |
| Runtime material/bounds contracts | 184 captures: 23 stages/method captures × both voxelizers × four submission modes |
| Existing static scene suite, with both budget fallbacks | 37 captures |
| Existing M7 wider-grid/fallback and method-switch checks | 15 captures in the quick grids/switches profiles |
| Class/union geometry and mixed-reflectance lifecycle oracles | 128 captures |
| 128³ material/reflectance/G-buffer fixed-grid checks | 2 producer captures |
| Current-source directional/receiver SPIR-V | 31 modules validated for Vulkan 1.1 |

The new matrix covers tint, metallic, emissive and alpha edits on static/dynamic materials; restoration; leaving the grid twice; returning; explicit expansion; and restoration of the initial grid. Each stage's HDR is byte-identical across both voxelizers and all four submission combinations. Native and scene runs report no VUID or synchronization hazards. The sample also rejects an invalid material index, nonfinite emission, invalid texture slot and reversed bounds. No image tolerance was relaxed.

## Reproduction and remaining scope

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `ConfigLoaderTest`, `CommonTest` and `VulkanRHITest`. Run native GI from `bin` with `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~` and `DISABLE_RTSS_LAYER=1`. From the repository root:

```powershell
python tools/validate_dynamic_voxel_contracts.py
python tools/validate_static_voxel_gi.py --output build/dynamic-voxel-gap-fixes/static-scenes
python tools/validate_dynamic_voxel_m7.py --quick --phase grids --output build/dynamic-voxel-gap-fixes/m7
python tools/validate_dynamic_voxel_m7.py --quick --phase switches --output build/dynamic-voxel-gap-fixes/m7
python tools/validate_voxel_classes.py --fixture geometry --policy averaged --matrix --output build/dynamic-voxel-gap-fixes/classes-geometry
python tools/validate_voxel_classes.py --fixture mixtures --policy averaged --matrix --output build/dynamic-voxel-gap-fixes/classes-mixtures
python tools/validate_voxelization.py --fixture materials --resolution 128 --reflectance-policy averaged --gbuffer --grid-percent 100 --output build/dynamic-voxel-gap-fixes/materials128
```

Artifacts are under `build/dynamic-voxel-gap-fixes/`: the untouched pre-fix snapshot, scoped patch, build/test logs, JSON results, saved configs, raw captures, shader validation and final verification manifest. Earlier M0–M8 reports retain their original measurement scope; this report adds evidence for the three repaired contracts.

M7's Stage A functional acceptance is supported with these fixes. M8 remains incomplete: cached-hit surface refresh changes gather work, so earlier timings must be remeasured before any current performance or automatic-selection claim. Cold-start/physical peak-memory and the other open profiling gates remain open. H0–H2 hardware triangle visibility remains deferred. The existing DDA approximation, point-sampled opacity, fixed resolution, unverified successful 256³ directional rendering and deferred native allocation-failure recovery limits remain.
