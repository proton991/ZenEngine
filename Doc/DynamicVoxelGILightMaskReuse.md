# Directional GI light-mask reuse

2026-09-25. Implemented for `voxel_gi_method=dynamic_voxel`; cone tracing is unaffected.

The directional renderer previously rebuilt all enabled-light visibility bits in both voxel classes every frame. It now retains masks across unchanged frames and records no light-mask passes when no bits need updating. Gathering continues to use current light color/intensity and material data.

| Change | Mask work |
| --- | --- |
| Light color/intensity, including black and zero intensity | Reuse geometric visibility |
| Camera, receiver lists, indirect gain or filter/history controls | Reuse geometric visibility |
| Position, direction, range, type, spot cone or per-light shadow flag | Refresh the affected enabled-light slots |
| Enabled-light count/order changes | Clear/recompute slots whose presence or visibility inputs changed; identical geometric light inputs can share the existing result |
| Occluder geometry/opacity, provider generation/backend, coverage/traversal settings, grid, static list generation or dynamic-input presence | Conservatively rebuild all slots |
| Global analytic/shadow toggle, first use or rejected publication | Rebuild all slots |

The current provider generation combines scene geometry and surface revisions, so a surface-only material edit also conservatively rebuilds these masks. This change does not introduce spatial invalidation or attempt to identify which receivers a moving occluder affects.

Visibility evaluation uses the existing light attenuation/range/cone routine with unit color/intensity. The gather uses the actual light values. This prevents a cached zero mask from leaving a light dark after its intensity or color is restored. The strict five-of-eight sample threshold is unchanged.

A persistent 16-byte storage buffer records per-light unknown-query bits. Frame preparation clears only updated slots and republishes any retained unknown status to the frame fallback flags. New queries set both persistent and frame status. An unchanged failed query therefore cannot turn into a confirmed-clear result when queries are skipped. Mask data and status use ordinary RDG dependencies; no CPU readback or GPU idle wait is added to rendering. Rejected graph publication invalidates the CPU reuse key.

The resource planner and exact-budget tests include the new buffer: `456*N³ + 48 + 32*ceil(N³/64)` bytes for directional frame resources, separately from class/cone resources, receiver attachments and static hit storage. Diagnostic `.static.json` captures include `light_mask_update_bits`: zero means reuse, individual bits identify refreshed slots, and `4294967295` denotes full invalidation. A bit identifies work for a light slot, not a measured ray count.

## Verification

The new native cases compare every cell's static/dynamic mask and a raw irradiance probe against a separate renderer forced to recompute visibility for each snapshot. They also assert the requested update bits. Cases cover cold black lights, intensity/color changes through zero, camera/filter/history changes, moving lights, point/spot range/cone/type changes, slot 31, removal/re-enable/reorder, global/per-light shadow switches, analytic toggles, moving/removing/restoring occluders, grid/traversal changes, rejected publication and persistent unknown status with partial updates/provider changes.

Fresh MSVC Debug builds of `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest` and `RenderCoreTest` passed. **124 native GI tests** passed with RT disabled and synchronization validation enabled across inline/threaded submission and async off/on, including **16 new cases**. **482 RenderCore tests** passed; seven existing benchmarks remain disabled. Evidence is under [build/gi-light-mask-reuse-20260925](../build/gi-light-mask-reuse-20260925/), including the native and RenderCore GoogleTest JSON results.

**29 directional SPIR-V modules** pass validation for Vulkan 1.1. **114 HDR scene captures** pass the existing static (37), lighting (27), extended filter lifecycle (27), and material/opacity/bounds/budget contract (23) suites. No application errors, VUIDs or synchronization hazards were found in their logs. The scene runs were serialized and restored `Data/engine.cfg` byte for byte. Quick lifecycle suites use the compute producer; the static suite includes both producers and submission variants.

Compared with the pre-change audit captures, **108/114 HDR files match byte for byte**, including every static and contract capture. The two animated fifth-light captures have different recorded light positions, and four moved/deformed temporal captures have different elapsed history times; those six are not counted as identical-input comparisons. Their existing numeric checks pass. The native tests above provide the matched-input cached-versus-recomputed comparison. Capture metadata reports zero updated mask bits in stationary cases and `16` (only slot 4) in the two animated fifth-light cases. See [verification summary](../build/gi-light-mask-reuse-20260925/verification-summary.json).

No FPS or full-frame GPU-time improvement is claimed from these correctness checks. Unchanged frames omit both mask dispatches; partial updates still scan the grids but query only updated light slots. Directional M8 profiling and automatic-method promotion remain open.
