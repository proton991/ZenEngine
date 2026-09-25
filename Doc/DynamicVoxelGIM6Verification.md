# M6 analytic lights, environment and emissive transport

Date: 2026-09-25. **M6 passed** for the explicit 64³ lighting profile. [Implementation plan](DynamicVoxelGIImplementationPlan.md), [M5 baseline](DynamicVoxelGIM5Verification.md).

## Implemented behavior

The explicit directional GI path now accepts zero through 32 enabled analytic lights. Each occupied static and dynamic sender gets a complete 32-bit light mask every frame. Each light retains the existing eight corner visibility samples and strict `visible > 4` threshold, using the common point/spot/directional light evaluation, finite segments, range, cones and shadow flags. Removing or disabling lights cannot leave old mask bits. Sender work includes off-camera cells.

The gather accumulates `rhoDiffuse * E_analytic`, `rhoDiffuse * E_environment` and `pi * Le` for surface hits, and `pi * L_environment(direction)` for confirmed misses, then divides by the cosine-weighted sample count. The receiver applies its diffuse BRDF once during composition. Indirect gain scales surface-hit contributions, including reflected sky and emissive senders; escaping sky remains at zero gain. The old diffuse IBL term is excluded when directional data is ready. Direct analytic lighting, directly visible emission and current specular IBL retain their existing paths.

Sender environment irradiance uses 128 cosine-weighted rays about the representative voxel normal, querying full-scene visibility through the same provider. Two RGBA16F volumes store irradiance and validity for occupied static/dynamic senders. Geometry/provider, grid/list, environment texture, intensity, rotation and enable changes invalidate this cache. It never reads previous receiver GI as sender lighting, so it adds no iterative bounce. Source-cubemap orientation uses the existing environment rotation and source Y convention. Unknown traversal is invalid and triggers the existing frame fallback; it is never treated as visible sky.

Three independent controls default to true: `dynamic_voxel_gi_analytic_lighting`, `dynamic_voxel_gi_environment_lighting`, and `dynamic_voxel_gi_emissive_lighting`. They control directional diffuse transport only. Filter histories reset when these controls or the environment change. Light add/remove, enable/type/shadow changes increment a structural revision and reset history even when the enabled count stays equal; ordinary light position/color/intensity animation retains temporal accumulation. Dynamic geometry refreshes both light masks and sender environment visibility while retaining compatible static intersection caches. Mesh direct shadows and markers continue through their existing renderer paths.

No material evaluator was duplicated. The existing shared voxel/G-buffer evaluation supplies UV0/UV1, sRGB base color, linear metallic/roughness, factors, vertex colors, alpha cutoff and transformed normals. Averaged diffuse reflectance already includes metallic weighting; gathering does not multiply it again. Representative normal and emission remain the V0/V1 owner policy. Receiver normal mapping remains separate from the voxel's representative mesh normal.

## Resource and delivery limits

The active frame-resource formula is now `456*N³ + 32 + 32*ceil(N³/64)` bytes, adding two RGBA16F sender-environment grids to M5. At 64³ these add 4 MiB. With the verified 3,072 MiB cap, averaged reflectance, a 256² G-buffer and three frames in flight, the preflight reserves 41,478 decoded static receivers and reports a 3,221,199,764-byte transition peak. Static intersections still cost 72 KiB per reserved receiver. This is the checked method allocation estimate, not measured total device memory.

Delivery remains explicit 64³ with 128 rays per face and at least seven color attachments. `auto` remains cone; unsupported requests and unready/overflow/unknown frames use the existing cone fallback. The two obsolete static-regression fallback pairs for environment and multiple lights were replaced by successful M6 directional cases. Hardware RT/triangle queries, wider-grid completion, final Stage A acceptance and profiling remain M7/M8/H0-H2 work. DDA visibility still has voxel-shaped occlusion and representative-surface error; this stage does not claim triangle-accurate paper reproduction or a frame-time target.

## Verification

RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, MSVC 14.51.36231. Native GI tests disable RT features and cover inline/threaded recording with async compute off/on. Native and scene runners use validation and synchronization validation.

| Gate | Result |
| --- | --- |
| CPU suites | 571 passed; 7 existing disabled RenderCore tests |
| Native GI | 92 passed: 24 M6 cases and all 68 earlier cases |
| M6 lighting/material scenes | 54 captures passed, both voxelizers |
| Extended geometry/filter lifecycle | 54 captures passed with five lights and environment enabled |
| Static scene regression | 35 captures passed |
| Legacy image regression | 158 GPU cases and image assertions passed |
| Directional GI SPIR-V | 30 modules validated for Vulkan 1.1, without RT constructs |

Native checks cover all 32 mask bits on both classes, 32/5/0/1-light snapshots, light animation retaining history, and emission across all four sender/receiver class pairs. Independent occupied-box hit fractions verify emission and reflected-versus-visible sky energy at gains 0/1/2. A constant source hemisphere approaches `pi * L`; a six-color cube verifies rotation/orientation for all six receiver directions. Removing a dynamic occluder refreshes sender sky irradiance, environment intensity changes refresh it again, and truncated traversal cannot expose sky.

Scene checks exercise zero lights, sky only, emission only, mixed terms and individual term disables. They run four static lights plus an animated fifth through movement/removal and actual mode/resize smoke transitions. Marker toggles leave surface lighting byte-identical. Factor/vertex-color equivalents, masked/opaque equivalents, separate/shared texture-role allocations, UV0/UV1 equivalents, and nonuniform instance versus baked geometry compare as specified by the runner. The UV1 fixture preserves UV0 for the existing receiver normal-map tangent basis; the first attempted fixture removed UV0 and therefore changed that basis. Correcting the fixture restored bitwise HDR equality without changing rendering code or relaxing a tolerance.

The extended lifecycle repeats raw, temporal-only and full filtering for both voxelizers through motion, deformation, teleportation, removal/restoration, camera change and resize/return. It retains the static cache generation and batch count. The M6 lighting scenes contain 1,115,680 ready directional pixels and 26,330 independent CPU composition probes; extended lifecycle captures add 130,506 ready pixels and 4,146 probes. All pass the previously locked local texture-interpolation/format precision bounds and occupied-donor padding checks. The static suite retains point/spot/directional, direct-shadow, normal-map, mirrored/sloped geometry, odd/upscaled extent, marker and resource-fallback tests.

HDR capture retains the existing seven-component layout. For ready directional GI, `bounced_diffuse` contains the combined surface-hit and escaping-sky term, and the old diffuse-environment component is zero. Capture metadata explicitly describes this; the independent controls and zero-gain runs isolate the terms without adding persistent irradiance banks.

## Evidence and reproduction

Artifacts are in `build/dynamic-voxel-m6/`: immutable pre-M6 `before/`, build and CPU/native logs/JSON, `scenes/results.json`, `filter-scenes/results.json`, `static-scenes/results.json`, legacy results, and hashed SPIR-V validation. Initial diagnostic failures remain separately logged. Final stage hashes and the patch relative to the pre-M6 worktree are recorded in `final-verification.json`, `changed-files.json` and `changes.patch`.

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `ConfigLoaderTest`, `CommonTest` and `VulkanRHITest`; run native tests from `bin`. Run these configuration-owning commands serially:

```text
python tools/validate_dynamic_voxel_m6.py
python tools/validate_dynamic_voxel_m5.py --extended-lighting --output build/dynamic-voxel-m6/filter-scenes
python tools/validate_static_voxel_gi.py --output build/dynamic-voxel-m6/static-scenes
```

Run `tools/validate_voxel_gi.py` with its output root redirected to `build/dynamic-voxel-m6/legacy`. Runners restore `Data/engine.cfg` byte-for-byte; the original SHA-256 is `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`. Existing changes remain uncommitted. This report does not certify M7, M8 or hardware RT.
