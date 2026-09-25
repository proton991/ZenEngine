# V1 averaged diffuse reflectance

Date: 2026-09-24. **V1 passed.** This extends the [V0 calibration](VoxelizationCalibration.md); it does not change the coverage contract or claim identical weighting to the paper's raster fragments.

The selectable `voxel_reflectance_policy=averaged` accumulates one contribution for each accepted triangle-record/voxel pair. Geometry rasterization uses a single-sample rectangle with the shared-edge fill rule; each projected pixel belongs to one rectangle triangle, then each depth cell is visited once. Compute assigns each projected cell to one lane and visits its depth cells once. Both call the same material/alpha evaluator and accumulator. Repeated triangle records intentionally contribute repeatedly; tessellation changes this weighting.

Each contribution is bounded linear effective diffuse reflectance `clamp(baseColor * (1 - clamp(metallic,0,1)) * 0.96,0,1)`. Base color includes sRGB texture decoding and vertex/factor modulation. Alpha is tested once with that evaluated material. Black and fully metallic accepted surfaces increase the count. Owner election, owner base color, mesh normal, metallic value and HDR emission retain the V0 contract. Cone injection uses the mean directly, without a second metallic/diffuse factor. Mode 1 and direct G-buffer shading keep their representative attributes.

RGB is rounded with `floor(rho * 4095 + 0.5)` into three uint32 sums, with a uint32 count. Admitted triangle records must not exceed `floor(UINT32_MAX / 4095) = 1,048,832`, an upper bound on per-cell contributions. This proves no sum/count wrap regardless of arrival order. Resolve converts operands to float before dividing and writes RGBA8_UNORM; occupied alpha is one and empty output is zero. The quantization/output error bound is `0.5/4095 + 0.5/255`, plus two millionths for shader arithmetic in the analytic oracle. Texture tests retain V0's `3/255` sampling allowance in addition; exact backend/order comparisons do not use that allowance.

The opt-in policy requires an explicit positive `voxel_reflectance_budget_mb`. The incremental logical peak is `20*N^3` bytes (uint4 scratch plus RGBA8 result), or 5, 40, 320 MiB at 64, 128, 256. Preflight separately checks accumulation count, uint32 buffer size, physical `maxStorageBufferRange`, checked retirement overlap and total budget. The current fixed-resolution voxelizer reuses its allocation, so its retirement overlap is zero. M0's whole-GI estimate adds these resources to the other persistent and transition resources. Unsupported requests are logged and resolve to owner before accumulation; dimensions and sample weighting are never silently reduced. No production readback or device-wide wait was added.

The scratch is cleared on each rebuild through RDG, followed by producer read/write atomics and resolve reads. Empty input clears the resolved texture. Submission failure leaves the generation unpublished and requests a retry using the existing voxelizer contract.

Raw diagnostic metadata records the actual policy and scale. The existing `.voxels.bin` 32-byte record is unchanged. An averaged `.reflectance.bin` has 32 bytes per cell, X fastest: four little-endian uint32 sums/count, packed RGBA8 mean, then three float32 radiance components (valid only when metadata `has_radiance` is one). Capture uses diagnostic GPU completion/readback. `tools/voxelization_reflectance.py` computes independent double-precision coverage/material means; boundary and alpha uncertainty are reported separately.

The owner policy remains the compatibility default after these comparisons. Averaging is an explicit opt-in, verified for use by M0 and later GI work. The tests establish deterministic aggregation and integration, not a general visual-quality ranking. No claim of area invariance, matching paper MSAA weights, averaged normals/emission, or measured performance is made.

## Verification evidence

Results are under [`build/dynamic-voxel-v1`](../build/dynamic-voxel-v1), using the available RTX 5080, driver 616.92, MSVC Debug build and Vulkan synchronization validation. The starting working tree and exact configuration are saved under `before`; source changes already present were preserved. Configuration was restored byte-for-byte (SHA-256 `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`).

| Check | Result |
| --- | --- |
| Independent raw-volume suite | 112 captures passed: 64-cubed geometry/material lifecycle and submission matrices; 128-cubed materials; 64/128/256 mixtures; reordered, duplicated and subdivided records |
| Coverage and owner attributes | No missing/extra cells, owner mismatches or bad attributes outside the V0 uncertainty contract; fixed-grid G-buffer comparisons passed; 20 corresponding raw owner volumes match saved V0 bytes exactly |
| Raw sums/counts/means | All oracle checks passed; backend/thread/async outputs are byte-identical, including uncertainty cells; removal clears sums/count/output and restore matches the initial bytes |
| Analytic mean error | Maximum 0.001905, below 0.002085; texture/material maximum 0.003843, below the inherited sampling-plus-output tolerance 0.013850 |
| Record order and tessellation | Reversal preserves exact sums and resolved output. Known red/blue/black/metal mixture counts are 4, 5, 7 for original, duplicated-red, subdivided-red cases, with the expected exact sums |
| Controlled radiance | Both producers match `meanReflectance * normalCosine` with directional intensity pi and no environment, within 0.00025 after FP16 radiance storage; metallic/diffuse factors are applied once |
| Room, mixed sender room, Sponza and mixture | 16 lighting/raw captures passed; owner attributes unchanged by policy; both voxelizers produce identical presentation and raw outputs |
| Compatibility | Four owner-policy room/Sponza presentation images match the pre-V1 images byte-for-byte |
| Native configuration rejection | Zero and insufficient budgets on both producers select owner explicitly and preserve the owner presentation bytes |
| Existing GI suite | 158 cases passed for owner and 158 for averaged, including modes, lights, environment, emission, shadows, markers and submission modes |
| CPU/graph/RHI | 475 RenderCore, 15 ConfigLoader, 36 Common and 39 VulkanRHI tests passed (565 total); includes count/range/budget/retirement rejection and failed-generation retry. One focused native workgroup/capability test passed |
| Shader validity | 14 affected SPIR-V modules passed `spirv-val --target-env vulkan1.1`; hashes and build logs retained |

The mixed sender room changes 60,086 RGB channels, with mean absolute presentation difference 0.03012/255 and maximum 12/255. Sponza changes 131,787 channels, mean 0.05625/255 and maximum 15/255. These are policy differences in fixed images, not HDR energy measurements or proof that one image is more accurate. M0 adds the pre-tone-map component baseline.

Verification found two implementation issues and fixed them before the final runs. Resolve now explicitly rounds to an 8-bit code before image storage, keeping half-step conversion within the stated bound. Raster and compute compilers also evaluated ill-conditioned barycentric expressions differently in Sponza; averaged contributions now use explicit dot arithmetic and `precise` intermediates to prevent contraction/reassociation. This change is scoped to contribution evaluation; owner resolve keeps the previous arithmetic. Final Sponza counts, sums, mean, radiance and presentation match exactly. `precise` maps to SPIR-V `NoContraction` in the [Khronos Vulkan GLSL contract](https://github.com/KhronosGroup/GLSL/blob/main/extensions/khr/GL_KHR_vulkan_glsl.txt); the observed cross-stage agreement is tested on this device, not a claim of bitwise equivalence across vendors.

An extra G-buffer probe using the normally padded grid instead of V0's fixed 100% calibration grid exceeds the atlas's 0.016 emission threshold at four pixels (0.01758–0.01953). The same discrepancies and identical voxel bytes reproduce with owner and averaged policies (`padded-owner-control` / `padded-averaged-control`); this probe is not counted as passing. The documented fixed-grid comparison passes. The pre-existing broader Vulkan integration suite-order failure remains outside this focused native run.

Reproduce after building the targets:

```powershell
python tools/validate_voxel_reflectance.py --phase raw
python tools/validate_voxel_reflectance.py --phase images
python tools/validate_voxel_reflectance.py --phase gi
```

Run these phases serially. Unit binaries run from `bin`; do not run config-reading tests concurrently with a config-mutating capture runner. V1 is complete; M0 may now capture either verified policy, recording the actual selection.
