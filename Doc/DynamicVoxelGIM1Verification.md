# M1 query contract and compute traversal

2026-09-24: **M1 passed** before M2 work. Mode 3 still uses cone GI. No directional irradiance gathering or native RT API was added.

`GIVisibilityProvider` prepares a generation-tagged query snapshot and declares its resources through RDG. The DDA provider exposes finite grid bounds, class completeness and cell-approximate precision. The diagnostic provider binds supplied decoded hits through the same consumer. Shared CPU/GLSL request and hit records are in `Graphics/Shared/GIVisibility.h`; requests are 48 bytes, decoded hits 96 bytes, harness results 112 bytes. DDA and reference variants include one shared consumer and contain no RT declarations.

Requests use a finite `[tMin,tMax)` interval, a unit world direction, static/dynamic/all mask, and an optional source class/cell hint. Results distinguish Hit, Miss and Unknown. IDs use `z + N*(y + N*x)`. A hit contains its cell-entry position/distance, representative voxel normal/base color/metallic/emission and effective diffuse reflectance. Missing attributes remain occlusion hits with the surface-valid bit unset. Normals never reject an occupied cell. Averaged reflectance is used once; owner reflectance receives its metallic/diffuse factors once.

Traversal clips to the grid first. Complete coverage declares space outside it empty; an incomplete selected class conservatively produces Unknown. Invalid rays/hints also produce Unknown, while a valid zero-length interval is empty. Cells are half-open at their upper faces; point-only/tangent contacts are not visited intervals. Exact crossing ties advance all tied axes. Co-located static/dynamic hits choose static deterministically. A self hint skips only that class/cell's initial exit interval when the original origin lies inside or on it; the other class and cells reached from outside remain occluders.

Each iteration crosses at least one cell boundary. A clipped ray crosses at most `3*N` boundaries, so the production loop is bounded by `3*N+1`. A deliberately reduced diagnostic iteration budget returns Unknown if traversal is unfinished. Closest and occlusion queries currently share this traversal; any-hit optimization is deferred.

The provider owns no native objects or submissions. Bound resources and uniform values are captured by RDG. M0's size/range and bounded XYZ/chunk helpers validate the harness dispatch. Invalid preparation prevents binding/execution. This is the query boundary required by M1; real gather/composition substitution remains M3 and filtering remains M5.

## Verification

Evidence: [`build/dynamic-voxel-m1`](../build/dynamic-voxel-m1). `DynamicVoxelGIIntegrationTest` runs with the startup RT capability switch disabled and asserts that acceleration structures, ray query and RT pipeline features are not enabled.

- Four native modes passed: inline/threaded submission, each with async compute off/on, with Vulkan synchronization validation.
- Each mode checks 2,534 rays against an independent double-precision occupied-box oracle, repeats them against empty grids, and substitutes supplied decoded responses through the deterministic provider. Fixtures cover axis/diagonal/random rays, inside/outside origins, grid faces/edges/corners, finite endpoints, source-cell hints, overlapping classes, invalid rays and exact class/cell IDs. Entry positions/distances use absolute `2e-5`; substituted responses require identical bytes.
- Additional checks cover incomplete coverage, forced traversal exhaustion, missing attributes, invalid generation/views, and preparation rejection. Small mocked dispatch limits force multidimensional mapping and multiple chunks on the real GPU.
- 566 existing CPU tests passed across RenderCore, ConfigLoader, Common and VulkanRHI. Both query SPIR-V modules pass Vulkan 1.1 validation; disassembly contains no acceleration-structure/ray-query/RT instructions or declarations.
- Build targets: `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `scene_renderer_demo`. No validation errors or reported memory leaks in the four native runs.

Device/toolchain match M0 (RTX 5080, driver 616.92, Vulkan SDK 1.4.357.0, MSVC Debug). The source snapshot preceding this stage is under `before/`. No config mutation was needed for the native query suite. These checks establish voxel-cell query behavior, not triangle-level visibility or GI energy correctness.
