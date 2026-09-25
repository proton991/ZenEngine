# M7 quality corrections and acceptance

Date: 2026-09-25. **All eight previously failing Sponza profiles now pass.** The full locked suite passes **56/56 image comparisons and 4/4 stationary sequences**, covering both voxelizers, 64³/128³, and raw/spatial filtering. The [original report](DynamicVoxelGIM7QualityVerification.md) retains the failing baseline and independent-reference methodology.

## Corrections

1. **Analytic sender visibility uses existing mesh shadow maps.** Solid occupied cells falsely occluded Sponza lights. The production mask pass now projects each cell center onto its elected owner triangle using the material-resolution barycentric helper, then evaluates alpha-aware, two-sided mesh shadows there. It uses the existing shadow bias/PCF, setting visibility above 0.5. No destination cell is ignored, no light is moved, and no intensity compensation is applied. DDA still handles bounce/environment queries; native query/reference fixtures retain their eight-corner, strict-majority oracle. This is an engine adaptation, not the paper's triangle-ray backend.
2. **Sender light direction and attenuation use that same point**, rather than a DDA cell-box entry. Static/dynamic representative points are cached with the masks. GIHit distances and occupied-box query semantics remain intact. Zero-intensity lights retain shadow maps so their visibility remains reusable when intensity changes.
3. **Six-face reconstruction preserves the first directional moment.** Signed axis weights are `(n_i² ± n_i)/2`: quadratic even components and linear odd components. This exactly reconstructs the irradiance `πa+(2π/3)b·n` of a positive field `L(w)=a+b·w`; the previous three-face squared-normal blend lost directional energy off-axis. Each contributing face must be valid, regardless of weight sign. Reconstructed RGB is clamped nonnegative. Arbitrary angular detail remains approximate.
4. **Spatial smoothing respects surface orientation.** Multiply the existing 3×3×3 Gaussian by the nonnegative cosine between occupied receiver/donor normals. Opposite sides of thin geometry no longer blur into one another. Empty dynamic-neighborhood receivers retain the spatial kernel; temporal equations and static interpolation padding remain in place.

Two vec4 sender-position grids add `32*N³` bytes: **8 MiB at 64³ / 64 MiB at 128³**. Checked frame budgeting is now `488*N³ + 48 + 32*ceil(N³/64)` bytes, excluding existing hit caches, receiver attachments and class/cone resources. Allocation, destruction and RDG dependencies include these buffers. M8 must profile the changed shader/resource costs; this validation does not establish a performance improvement.

## Results under unchanged limits

Reference radiance, scene lights, quality thresholds and reference tolerances were unchanged. The limits SHA-256 remains `0a5dee38ffa6c46a4c232ffc1c7bcb43e5c483e53e0c7521ac2818e0a6d4abb0`.

| Sponza profile | Relative RMSE | Mean RGB bias | Gate |
| --- | ---: | ---: | --- |
| 64³ raw | 52.22% | −32.44% | Pass |
| 64³ spatial | 48.86% | −34.08% | Pass |
| 128³ raw | 45.00% | −20.43% | Pass |
| 128³ spatial | 39.15% | −22.12% | Pass |

Both voxelizers produce identical diffuse captures. Baseline raw RMSE was **99.65% / 63.77%**, with mean bias **−99.91% / −63.44%**, at 64³/128³. The corrected 64³ spatial bias is close to its 35% limit: acceptance is for this declared matrix, not high fidelity in arbitrary scenes. The room and five panel fixtures also pass. All four stationary sequences have zero measured inter-frame variance and match their spatial baseline after convergence.

## Regression evidence

- **132/132 native Vulkan GI tests passed**, across all four inline/threaded and async on/off combinations. Includes new analytic first-moment and opposite-surface filtering regressions, existing DDA occupied-box oracles, mask reuse/invalidation, motion, light removal and temporal convergence.
- **482 RenderCore tests passed; 7 existing tests disabled.** Updated tests cover exact resource accounting and shadow-cache retention across zero-intensity/off-to-on changes.
- **12/12 production mesh-light controls passed:** clear visibility, lights inside occupied cells, opaque blockers, reversed winding, alpha-transparent texture cutouts, and geometry beyond the finite light endpoint, on both voxelizers. Each checks 100 analytically classified receiver cells. Opaque/backface controls have zero visible bits; the others have all 100.
- **54/54 M6 lighting/material scene profiles passed** on both voxelizers, including animated fifth-light movement/removal, sky rotation, lighting toggles, emission, texture roles/UVs and nonuniform transforms. The M6 runner now accepts `--exe` to select the tested Release binary.
- **11/11 independent CPU-reference tests passed; 68 SPIR-V modules validated.** Quality captures use MSVC Release, RT disabled, Vulkan/synchronization validation enabled, 152×96 exported images and the same 128×128 G-buffer as the failing baseline.

## Reproduction and evidence

Build the MSVC Release preset, install the pinned Python requirements, and use the validation environment from the original report:

```powershell
python tools/validate_dynamic_voxel_quality.py --output build/dynamic-voxel-m7-quality-fix-20260925/scenes --temporal-scenes
python tools/validate_dynamic_voxel_mesh_lights.py --output build/dynamic-voxel-m7-quality-fix-20260925/mesh-lights
build/x64-windows-msvc-release/bin/DynamicVoxelGIIntegrationTest.exe
build/x64-windows-msvc-release/bin/RenderCoreTest.exe
```

Artifacts: `build/dynamic-voxel-m7-quality-fix-20260925/`, including `scenes/results.json`, per-profile HDR/reference/error images, `full-quality.log`, `mesh-lights/results.json`, `native-tests.json`, `render-core-tests.json`, `python-oracle-tests.log` and `spirv-validation.json`, `m6-scenes/results.json` and `final-verification.json`. Earlier failures remain in their original directory. Every scene runner restores user configuration byte-for-byte.

**M7 quality acceptance is complete for this matrix. M8 performance acceptance and automatic promotion of dynamic_voxel remain open.**
