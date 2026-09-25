# M2 mutable geometry and class voxel outputs

2026-09-24. **M2 passed.** [M1](DynamicVoxelGIM1Verification.md) passed before this stage. Mode 3 still selects cone GI; directional irradiance gathering starts at M3.

**2026-09-25 follow-up:** the historical automatic bounds expansion described below has been replaced by fixed voxel bounds, explicit `RenderScene::SetVoxelBounds`, incomplete class coverage and PBR fallback. `RenderScene::UpdateMaterial` now publishes material changes separately from geometry and coverage. See [gap verification](DynamicVoxelGIGapVerification.md) for the current contract and regression evidence; the original M2 results below retain their historical scope.

## Implemented contract

`RenderScene` now stages instance classification, enable/removal state, affine world transforms and explicit vertex updates. Instance IDs are the renderable slots assigned at scene load. Disabling or promoting an instance preserves its slot and triangle-record IDs; disabled triangle records carry a zero class mask. New instances/topology editing, skinning and material editing are outside M2.

Call `SetInstanceClass`, `SetInstanceEnabled`, `SetInstanceTransform` and `UpdateVertices` on the main/render thread between frame recordings. `Update()` commits their combined changes before building raster, shadow or voxel passes. Finite input/range checks reject invalid setters; commit checks buffer sizes/ranges and transformed bounds before publishing replacements. A failed commit stops rendering instead of publishing a partial generation. Replacement node/triangle/vertex buffers use RenderDevice's existing upload and deferred-retirement paths, so recorded work retains its original resources. There is no production readback or added GPU-idle wait.

All consumers bind the committed node and vertex buffers. Raster and direct-shadow draw snapshots omit disabled instances. Both voxel producers use the same stable triangle records, coverage/material shaders and class mask. Scene revisions distinguish static, dynamic and all-scene changes. Bounds expand when necessary and do not contract on removal: motion within the current bounds rebuilds only the affected class; expansion changes the common grid and invalidates both classes. This is deliberately conservative and is not a moving clipmap.

`RendererServer::EnableClassVoxelization(budgetBytes)` explicitly prepares the two class outputs after a complete resource preflight. Each owns occupancy, representative surface attributes, optional V1 reflectance, a GPU occupied-cell list, a grid-to-list map and a count. IDs are `z + N*(y + N*x)`. List order follows atomic allocation and is not stable; cell IDs and inverse mappings are stable. Lists reserve `N^3` entries, so one invocation per cell proves that the count cannot overflow. Empty map entries and unused list entries are `UINT32_MAX`. Counts stay on the GPU outside opt-in diagnostics.

The prepared `VoxelDDAProvider` consumes those real outputs for static-only, dynamic-only and full-scene queries. Its positions and geometry remain cell approximations. The representative owner attributes and averaged diffuse reflectance retain the V0/V1 contracts; the provider does not invent triangle-level visibility.

The combined cone output allocates on demand for cone or mode-1 visualization. PBR does not allocate it. Entering cone after changes revoxelizes all enabled instances from the current generation, then updates opacity mips, sky and radiance before composition. It independently sums all accepted contributions, preserving weighted reflectance when classes overlap. It is retained across mode switches and reused in place through RDG dependencies; no replaced GI allocation is hidden outside the transition estimate. Mode 1 continues to visualize the union. Class outputs remain opt-in through the preparation API; `dynamic_voxel` selection still logs its cone fallback because gathering is not implemented.

## Allocation decision

M2 keeps independent reflectance scratch for the two producers, plus the combined cone scratch when needed. This replaces M0's provisional single shared scratch estimate; the full future-method estimator now includes all three concurrent allocations during a cone transition. The occupied lists reserve the complete cell capacity, independently of later receiver/cache capacities.

The M2 preflight covers both class surface/list/map/count sets and the complete cone surface/sky/radiance transition. Let `C=N^3` and `M=sum(mipSide^3)`. Its logical byte peak is `80*C + 12*M + 8` for owner reflectance, or `140*C + 12*M + 8` for averaged reflectance, plus supplied retiring GI bytes. It checks the largest individual storage-buffer range independently. The fixed-resolution implementation allocates class outputs once and reuses them, so it has no retiring GI generations to add during ordinary geometry updates.

| Grid | Owner peak, MiB | Averaged peak, MiB |
| --- | ---: | ---: |
| 64 cubed | 23.43 | 38.43 |
| 128 cubed | 187.43 | 307.43 |
| 256 cubed | 1499.43 | 2459.43 |

These are GI resource bytes, not physical allocator measurements or total renderer memory. Scene input buffers, G-buffer, direct-shadow maps, mode-1 visualization buffers, diagnostic readbacks and allocator padding are outside this GI cap. V1's incremental reflectance cap also remains enforced. No resolution is silently reduced. Native allocation-failure recovery and larger-grid runtime acceptance remain deferred as specified by the plan.

## Verification

Evidence lives in [`build/dynamic-voxel-m2`](../build/dynamic-voxel-m2); the preceding source snapshot is in `before/`. The native device/toolchain are the same RTX 5080, driver 616.92, Vulkan SDK 1.4.357.0 and MSVC Debug used for M0/M1. Native runs enable Vulkan synchronization validation and disable implicit overlays.

`tools/validate_voxel_classes.py` invokes `--capture-voxels=prefix --voxel-classes` with an explicit memory budget. It reuses the existing independent double-precision V0 clipping/material oracle and V1 contribution/count oracle. Captured GPU inputs define the geometry, while a separate lifecycle check compares those inputs to the intended CPU edits. Existing captures without class metadata retain their original interpretation.

- **28 native runs, 224 lifecycle states, 672 class/combined raw volumes passed.** Geometry and material fixtures exercise both geometry/compute producers across inline/threaded execution and async compute off/on. Additional cases cover owner policy and overlapping reflectance with reversed records, duplicates and tessellation.
- States cover initial mixed classes, rigid motion, vertex deformation, dynamic removal, static-to-dynamic promotion, all instances disabled, restoration, and PBR-to-cone entry after queued transform/vertex replacements. Budget rejection is checked before class allocation. Invalid IDs, class masks, nonfinite transforms and vertex ranges are rejected.
- All 448 class list/map captures are bijections over occupied cells with correct counts and cleared empty entries. Combined occupancy equals the class union, and its owner/surface record is the deterministic winning class record. Combined integer RGB sums and counts equal the sum of both classes; no unweighted averaging of class means occurs.
- **129,024 real-output DDA queries passed** an independent occupied-box oracle: exact status/class/cell IDs and decoded attributes, entry position/distance within `2e-5`, with all three class masks. The set contains 10,458 hits. M1's separate boundary/random/incomplete-coverage suite remains the broader traversal test.
- Stable triangle/instance/material IDs and identical per-class scene-input snapshots are checked in every state. Bounded dynamic motion leaves static bytes/revision unchanged. Empty composition is black with sky, environment and markers disabled; restoration reproduces the initial image exactly when the grid is unchanged. Expanded bounds deliberately remain expanded, so returning geometry can use a different discretization.
- 568 CPU tests passed: 478 RenderCore, 15 ConfigLoader, 36 Common and 39 VulkanRHI. Added cases cover lazy allocation/class immutability and independent scratch, transition, retirement, descriptor-range and budget limits at 64/128/256. Four native M1 modes passed again with RT features disabled.
- The original fixed-grid geometry and material/reflectance lifecycle suites passed again: 12 V0 and 12 V1 captures, including the material G-buffer check. Both complete legacy suites passed: 158 owner-policy cases and 158 averaged-policy cases, covering modes, lights, direct shadows, materials, markers and resizing. Lazy allocation exposed a frame-capture sampler dependency; capture now obtains a cached RenderDevice sampler and PBR-first runs pass.
- A final two-producer material lifecycle run passed after review, including a valid `1e-5` affine scale. Transform validation rejects singular/nonfinite transforms or nonfinite inverse matrices without imposing an arbitrary scale cutoff.
- Thirteen affected/query SPIR-V modules passed Vulkan 1.1 validation. C++ changes use the repository `.clang-format` and the user's global rules.

All config-mutating runners ran serially and restored the exact original `Data/engine.cfg` bytes (SHA-256 `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`). No validation errors or memory leaks were reported in the passing runs. The previously documented broad Vulkan integration-suite ordering issue is outside this milestone; only the named focused native suites are claimed.

Class native coverage here is 64 cubed. The larger grids above are preflight boundary tests, not a claim of larger-grid runtime or performance acceptance. No directional gathering, temporal filter, hardware RT integration, performance improvement or paper-executable equivalence is claimed by M2.

Next: **M3**, static six-face visibility caching and single-light diffuse gathering, followed by deferred composition and interpolation checks.
