# Camera-relative rendering plan

Status: proposed on 2026-10-10; not started. Split out of hybrid GI P4 by decision 8 of [P4 gaps](HybridGI/P4Gaps.md#decisions-2026-10-10): the P4 exit criteria never required large world coordinates, and the fix touches every system that consumes world positions.

## Problem

The renderer computes and stores absolute world positions in 32-bit floats. A float's step is about 1.2e-7 of its magnitude, so precision is lost as a scene moves away from the world origin, independently of its distance from the camera:

| Scene offset | Float step at the scene | 32-step ray-start offset |
| --- | --- | --- |
| 0 (Sponza, unit-normalized) | ~1e-7 | ~4e-6 |
| 100 units | 7.6e-6 | 2.4e-4 |
| 10,000 units | 9.8e-4 (about one pixel's footprint) | 0.031 |

The P4 origin-stress sequence translates every instance and the camera together, so the image should not change. Measured against its zero-offset capture ([P4 execution](HybridGI/P4Execution.md#native-acceptance-checks); captures in `build/p4-gaps/fp32-native/origin-top-origin-*`):

- **10,000 units:** full-image bias −49.65%, RMS 75.53%, 177,691 unexpected zeros. Receiver positions move by up to one float step (median 8.2e-4 relative to the camera). The geometric normal, `cross(dFdx(p), dFdy(p))` of those positions, is mostly rounding noise: median error 16.8°, 56% of pixels beyond 5°. The sky trace rejects directions below that normal and offsets ray starts along it, so raw sky falls to 0.503×; of the 178,794 pixels that are zero but lit at zero offset (above 5% of the mean), 74% have a normal more than 5° wrong. The ray-start offset of 32 float steps is 0.031 units, enough to cross thin geometry, and the hardware transforms each ray into instance space at the same magnitude.
- **100 units:** 420 unexpected zeros, though positions and normals are nearly exact (median normal error 0°, 0.1% beyond 5°). Of the 445 pixels that are zero but lit at zero offset, 71 have a wrong normal; the rest are not yet isolated (likely ray-start offsets at grazing angles).

The translated ray-query integration tests pass at both offsets because they trace a few rays from known origins with tolerances; they do not go through raster receivers or derivative normals.

## Approach

Keep world transforms in double precision on the CPU and give the GPU only positions relative to a per-view origin at the camera. This is the established pattern: Unreal's translated world space (since UE4; UE5's Large World Coordinates add double CPU transforms), Unity HDRP's camera-relative rendering, and Cesium's relative-to-eye rendering. Floating-origin rebasing (Kerbal Space Program, UE4 world origin rebasing) shifts the whole world occasionally instead; it is cheaper per frame but every system must handle the shift, so it is used here only for structures too costly to rebuild each frame (the TLAS and the voxel volume).

Precision is lost in the vertex shader's first `modelMatrix * position`, so no shader-only change can recover it; the subtraction must happen in double precision before upload.

## Work

1. **Transforms.** Store node world transforms (`Transform::GetWorldMatrix`, `Mat4` today) and the camera position in double precision. Each frame, upload model matrices with the view origin subtracted from their translation, and a view matrix without the camera translation.
2. **Receivers and G-buffer.** `inWorldPos`, the FP32 receiver-position target, depth reconstruction (`hybrid.worldOrigin` already shifts the inverse matrix) and the derivative geometric normal all become camera-relative, which restores their precision near the camera. Shaders that need world positions add the view origin explicitly; uniforms carry it split into high and low floats where an absolute position must be exact.
3. **Motion vectors and history.** The previous frame's matrices are expressed relative to the current view origin, so reprojection survives camera motion and rebasing.
4. **Acceleration structures.** Build TLAS instance transforms (`SceneRayQuery.cpp`) relative to a ray origin that follows the camera and is rebased when the camera moves more than a threshold away; rebasing rewrites instance transforms and rebuilds the TLAS (BLASes are in object space and unchanged). Ray origins and `HybridTraceOrigin` operate in the same space.
5. **Voxel GI.** The voxel grid minimum and `WorldToVoxelUV` use the same rebased origin; a rebase moves the grid and invalidates or shifts voxel and probe histories.
6. **Lights and shadows.** Light positions, shadow matrices and cascades are computed relative to the view origin.
7. **Ray-start offsets.** With small coordinates the existing float-step offset of `HybridTraceOrigin` is again proportional to local precision; recheck it at grazing angles, where the 100-unit zeros suggest it is marginal.

## Validation

- The origin-stress sequence at 0, 100, 10,000 and 1,000,000 units must match its zero-offset capture within the shipping-preset limits, with no unexpected zeros, on both providers.
- Raster-only comparison (no GI) at the same offsets, to separate raster precision from GI.
- Camera motion across a rebase boundary: no history reset beyond the documented one and no visible jump.
- The P4 ground-truth sweep and native fixtures must be unchanged at zero offset.

## Not in scope

Physics, audio, scripting and networking coordinates; streaming of very large worlds; cascaded voxel volumes (a later candidate of the [hybrid GI plan](HardwareRayQueryEnvironmentLightingPlan.md#later-candidates)).
