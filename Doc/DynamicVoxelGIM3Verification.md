# M3 static diffuse GI verification

Date: 2026-09-24. **M3 passed** for the explicit static, 64³, single analytic light profile. [Implementation plan](DynamicVoxelGIImplementationPlan.md). Hardware RT remains deferred; visibility is the M1 occupied-cell DDA approximation.

## Implemented contract

`DynamicVoxelGIRenderer` builds six static visibility caches, evaluates eight light-visibility samples per occupied sender (strictly more than four visible), gathers 128 cosine-weighted directions per face, and pads empty interpolation cells from valid occupied donors in a separate output. Padding never reads other padding and remains enabled without filters. The cached first hit remains opaque when unlit. Point/spot attenuation is applied once; directional lights use the same light evaluator.

Raw faces contain irradiance: sender diffuse reflectance times direct incident illumination and sender cosine, averaged over the cosine samples. The Lambertian sender factor and sampling weight cancel. Composition applies the visible receiver's diffuse BRDF and AO once, using normalized squared normal components to blend three signed faces. Emission, diffuse environment transport, multiple lights, dynamic transport and filters are outside this milestone.

Deferred recording now produces the G-buffer before GI. Receiver identity, position, shading normal, geometric normal and material use one integer pixel-center texel mapping. The geometric normal is produced from original position derivatives before alpha discard, oriented against the unperturbed normal; degenerate derivatives use that unperturbed normal. It remains separate from the mapped shading normal used by the BRDF. Direct mesh shadows and light markers remain in their existing paths.

Cache invalidation includes provider/backend generation, compact-list generation and grid placement. Cache readiness is published only after successful graph execution. Lighting is recomputed without rebuilding static visibility. GPU overflow, unknown queries or unsupported dynamic occupancy select cone lighting for the frame; unsupported scene profiles and pre-allocation rejection select cone before recording. `auto` remains cone.

## Resources and configuration

Opt in with `voxel_gi_method=dynamic_voxel`, `voxel_resolution=64`, backend `auto` or `voxel_dda`, 128 rays per face, one enabled analytic light, diffuse environment disabled, no emissive materials, and an explicit positive `dynamic_voxel_gi_memory_budget_mb`. Owner and averaged reflectance both work. Averaged reflectance retains its separate V1 budget. Seven color attachments are required.

The initial cache deliberately retains the decoded **96-byte `GIHit`**, split into six buffers. It costs **72 KiB per reserved static receiver**, not the proposed packed eight-byte record. The active M3 planner checks each descriptor/RHI range and the total class/cone-transition, raw/padded volume, mask/status and receiver-attachment reserve. The M0 packed-layout estimator describes future storage, not these allocations. Packing is deferred to M8. Native allocation failure recovery and GPU timing remain outside scope.

The verified room has 28,337 occupied cells. At a 256² G-buffer and three frames in flight, 3,072 MiB permits 42,745 cached receivers: six 525,250,560-byte hit buffers, 25,165,824 bytes of raw/padded faces, and 3,145,728 bytes of additional receiver attachments; the planned transition peak is 3,221,158,788 bytes. This logical GI estimate excludes common renderer/driver overhead. A 2,048 MiB budget permits only 28,225 receivers and correctly falls back. These are fixture values, not a recommended shipping preset.

## Verification

Environment: Windows, MSVC 14.51.36231, Vulkan SDK 1.4.357.0, RTX 5080, driver 616.92 / Vulkan 1.4.351. Native tests disable RT features, enable validation and synchronization validation, and exercise inline/threaded RHI recording with async compute off/on.

| Check | Result |
| --- | --- |
| CPU suites | 569 passed: RenderCore 479 (7 disabled), ConfigLoader 15, Common 36, VulkanRHI 39 |
| Native Vulkan suite | 20 passed: 16 M3 cases and 4 M1 query/provider regression cases |
| Static scene matrix | 38 cases passed; both voxelizers, all four submission modes, owner/averaged reflectance |
| Existing image matrix | 158 cases and assertions passed |
| Legacy HDR capture | Additional quick room case passed |
| SPIR-V | 26 dynamic-query/static-GI and scene-renderer modules validated for Vulkan 1.1; no RT types/capabilities |

Native checks cover constant incident radiance and final BRDF scaling, strict four/five visibility threshold, point/spot attenuation, overflow, unlit blockers in front of lit senders, and identical DDA versus replayed decoded-hit gather and actual deferred composition. Constant-field probes cover fractional positions, grid corners, axis/diagonal normals, and 11,560 probes on flat/sloped planes translated across cell layers. Padding donors remain occupied-only. Incomplete query coverage produces unknown/fallback rather than sky illumination.

The scene matrix verifies color bleeding against black senders, zero/double indirect gain, unchanged direct/specular/emission components, marker isolation, unsupported-profile equivalence to explicit cone, and actual overflow fallback. Normal-mapped flat, sloped and mirrored receivers run at equal resolution, 2×, 1.5× and odd extents (257² G-buffer to 515×321 viewport). Mirrored fixtures explicitly supply front-facing winding and authored normals; this is a normal-transform test, not a general mirrored-culling change. All valid supported-profile pixels report directional readiness. The eight voxelizer/submission room results are bitwise identical.

Independent CPU checks validate padding and reproduce directional sampling plus receiver BRDF for 6,752 diffuse probes across 273,765 valid surface pixels. Maximum geometric-normal error is 0.000307. Maximum diffuse absolute error is 0.013311 in the double-gain case; the bound accounts for local texture gradients, the device's eight `subTexelPrecisionBits`, and half-float interpolation. It is not a universal fixed error allowance. Vulkan defines this interpolation coordinate precision in [VkPhysicalDeviceLimits](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html). Pixels selecting the same native G-buffer texel retain identical surface tuples and direct lighting at scaled extents.

## Evidence and reproduction

Local artifacts are under `build/dynamic-voxel-m3/`: CPU JSON/logs, `native-final.json`, `scenes/results.json`, `legacy.log`, `legacy-hdr/`, `spirv-validation.json`, `final-verification.json` and `changes.patch`. `before/` preserves the pre-M3 dirty-source snapshot. The final diagnostic capture matches the original room's lighting, surface and static buffers byte for byte.

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `ConfigLoaderTest`, `CommonTest`, and `VulkanRHITest` with the existing debug preset. Run native binaries from `bin`. Run `python tools/validate_static_voxel_gi.py` for the new scene matrix, and `tools/validate_voxel_gi.py` for the legacy image matrix. Config-mutating runners must run serially. The original `Data/engine.cfg` is restored byte for byte, SHA256 `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`.

Opt-in diagnostics add `.surface.bin` (six float4s per viewport pixel; metadata describes uint-bitcast identity/class/texel coordinates), `.static.bin` (six raw/padded float4 pairs per Z-fast cell, then uint light masks and uint4 status), and `.static.json`. Existing `.lighting.bin` remains seven float4 components. Diagnostic readbacks wait for GPU completion outside the ordinary frame path; they are not performance measurements. The final raw-volume capture writes one complete face at a time into an 8 MiB GPU temporary, eliminating a partial-write content-coverage warning. Existing presentation capture with a disabled skybox can still report the pre-existing backbuffer coverage warning; no Vulkan validation errors or tracked leaks occurred.

The complete paper executable was not run. Visibility, thin geometry, self-cell rejection, representative normals and material mixing retain the documented V0/V1/M1 approximation limits. M4 is next; M5 filtering, M6 lighting extensions, M7 acceptance, M8 performance and hardware RT remain unfinished.
