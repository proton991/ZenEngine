# M0 reference audit and HDR baseline

Date: 2026-09-24. **M0 passed.** [V0](VoxelizationCalibration.md) and [V1](VoxelReflectanceVerification.md) passed before these baselines. M0 adds diagnostics, configuration and preflight checks; mode 3 still executes the existing cone tracer. No DDA provider, directional irradiance algorithm or RHI ray-tracing path is implemented by this milestone.

## Reference audit and delivery choices

Paper: *Dynamic Voxel-Based Global Illumination*, Cosin Ayerbe, Poulin and Patow, CGF 44(1), e15262, DOI `10.1111/cgf.15262`. The local 23-page PDF identified in the plan has SHA-256 `cc82aec03de846c0213d60a69789be5598bae468f307a200fc89e21a754cc334`.

Source: authors' Bitbucket commit **`581c116061b1294dad2a8eca6afb45b49cc95a7e`** (2024-10-15). M0 inspected 28 shaders and three technique C++ files, retrieved using the repository REST API. The cached sources, paths/hashes and direction-table measurements are in [`reference/`](../build/dynamic-voxel-m0/reference). This is a source audit, not an executed reference comparison. It does not establish equivalence across the authors' runtime flags, scene settings or hardware.

| Topic | Observed source behavior | Engine decision for later milestones |
| --- | --- | --- |
| Origin | Static and dynamic ray generation use cell center plus signed face axis times per-axis half extent. Gather reconstructs from that same face center. The reference ray minimum scales the half-cell diagonal. | Face centers are confirmed. Keep one shared origin/reconstruction convention; calibrate provider-specific self-intersection handling instead of importing the cone bias. |
| Normal/distance packing | Static packer truncates mapped normal X/Y to seven bits each, uses bit 14 for Z sign, and stores half-distance in bits 16–31. The dynamic path also uses bit 15 for class. Decode reconstructs Z and normalizes. | DDA retains integer cell/class identity and FP32 entry distance in its proposed eight-byte record. H0 must test triangle packing against precise hit identity before adopting it. |
| Directions and gather weights | Six fixed tables contain 128 directions each; all fourth components are zero. Gather combines differential-area/distance and neighbor heuristics, reduces weighted contributions, then applies an empirical factor of four. | No verified sampling PDF or physical normalization can be inferred from that table alone. Keep the planned solid-angle estimator and `pi * L` constant-radiance test as an explicit adaptation; do not multiply it by another inverse-square form factor. |
| Face interpolation | Three signed-axis texture samples use angular weights `1-acos(abs(n_i))/(pi/2)`, followed by L2 normalization. | Use normalized squared normal components. Their sum is one, preserving a constant field even at diagonal normals; this differs from the source. |
| Lit mask | Eight interior positions use the combinations of center plus/minus `0.475 * cellExtent`. The shader accepts **more than one** visible sample. | The paper specifies **more than four**. Preserve `>4` as the documented paper baseline and use the inspected interior positions. Record the discrepancy rather than asserting agreement. |
| Temporal initialization | Zero RGB acts as uninitialized history. Initial static/dynamic values are multiplied by `0.575`/`0.7`; subsequent updates use alpha `0.03`. | Explicit validity and the first unscaled result avoid treating valid black as missing history. Retain the planned fixed-alpha reference option and elapsed-time engine option in M5. |

Source locations at the pinned commit:

- Origins/packing: [`voxelvisibilityraytraceraygeneration.rgen`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/voxelvisibilityraytraceraygeneration.rgen), lines 259–266, 349–379, 410–438; [`lightbouncedynamicvoxelirradiance.rgen`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/lightbouncedynamicvoxelirradiance.rgen), packing/origin routines; [`lightbouncevoxelirradiancefirstpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/lightbouncevoxelirradiancefirstpart.comp), lines 1022–1039, decode.
- Directions: [`lightbouncevoxelirradiancetechnique.cpp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/Framework2/source/rastertechnique/lightbouncevoxelirradiancetechnique.cpp), lines 451–1231. Across the six tables the minimum face cosine is 0.0878–0.1068, mean cosine 0.5409–0.5448, and maximum direction-length error below `7e-7`. These measurements describe the table, not its generation PDF.
- Gather and static initialization: [`lightbouncevoxelirradiancefirstpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/lightbouncevoxelirradiancefirstpart.comp), `voxelToVoxelIrradianceBasic` and `differentialAreaFormFactor`; [`lightbouncevoxelirradiancesecondpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/lightbouncevoxelirradiancesecondpart.comp), lines 45, 156–183. Dynamic initialization: [`lightbouncedynamicvoxelirradiancesecondpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/lightbouncedynamicvoxelirradiancesecondpart.comp), lines 206–278.
- Face weights: [`scenelightingdeferred.frag`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/scenelightingdeferred.frag), lines 301–441. Lit samples/threshold: [`litvoxelfirstpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/litvoxelfirstpart.comp), lines 229–240, and [`litvoxelsecondpart.comp`](https://bitbucket.org/Alex_Storm_/dvbgi/src/581c116061b1294dad2a8eca6afb45b49cc95a7e/data/vulkanshaders/litvoxelsecondpart.comp), lines 70–109.

Unresolved reference equivalence is explicit: there is no reproduced author executable, verified direction-generation PDF, physical derivation of the empirical gather scale, or matched reference configuration for our multiple lights/environment/emission. Later transport tests must establish the engine's declared units independently. No timing or quality superiority is claimed here.

## Implemented configuration, resource and dispatch checks

`voxel_gi_method=auto|cone|dynamic_voxel` is independent of `dynamic_voxel_gi_query_backend=auto|voxel_dda|hardware_rt` and the voxelizer choice. All nine combinations currently select **cone / backend none**. Explicit unavailable requests report the reason; a hardware feature bit never advertises an implemented RT provider. Legacy defaults are unchanged.

Settings accept grid 64/128/256, 128 rays per face, and neighbor radius 1/2. Missing grid defaults to 64 in the new-method settings; explicit values are preserved. A positive `dynamic_voxel_gi_memory_budget_mb` is required by the future cache preflight. No shipping cap is guessed, and the estimator rejects a missing cap. It is separate from V1's incremental reflectance budget. Invalid settings leave the caller's previous settings intact. The renderer presently validates these settings and logs the actual fallback; it does not allocate a dynamic GI cache.

The provider-aware estimator accounts for capacities, not current live counts: static and dynamic attributes, lists/maps/masks/validity, two static visibility streams and one dynamic stream, 36 RGBA16F irradiance/history/scratch textures, sender environment caches, optional V1 output/scratch, lazy cone transition resources, and explicitly supplied retiring bytes. DDA visibility costs `12288*S + 6144*D` bytes at 128 directions. M0 initially budgeted one V1 scratch for serially ordered class builds. M2 now counts independent static/dynamic scratch, full-capacity occupied lists/counts, and the separate cone scratch during transitions; see [M2 allocation decision](DynamicVoxelGIM2Verification.md#allocation-decision). Hardware RT estimates are rejected until queried AS sizes and validated layouts exist.

This is a logical payload estimate, not measured driver allocation/residency. Texture allocation overhead, new staging needs and future layout changes must be added when those resources are implemented. Every storage buffer is checked independently against uint32 byte-size and the device's `maxStorageBufferRange`; bounded grid/capacity validation precedes multiplication and narrowing. Mock tests cover exact range limits, overflow, capacity rejection, retiring overlap and total cap. At 256 cubed a 699,050-receiver DDA stream fits uint32 bytes; 699,051 does not, regardless of total cap.

`RHIGPUInfo` now exposes native per-axis `maxComputeWorkGroupCount` and storage range. RDG direct dispatch rejects a negative or excessive axis before submission. The raw low-level command-list API still requires callers to supply legal counts; M0 does not silently drop native commands. `BuildComputeDispatchChunk` maps linear work through bounded XYZ groups and further chunks, separately validates local size/invocations, and prevents padded-index/base-offset overflow. CPU tests enumerate each logical item exactly once for zero work, boundaries, multidimensional tails and multiple chunks. GPU-generated indirect arguments remain M4 work; no receiver-count readback was introduced.

## Floating-point capture contract

Example from `bin`:

```powershell
.\scene_renderer_demo.exe --frames=3 --mode=3 --rhi-thread=1 --async-compute=1 --capture-lighting=E:/Dev/ZenEngine/build/lighting --capture=E:/Dev/ZenEngine/build/lighting.ppm
```

The sample renders one additional frame with the diagnostic variant. Freeze animated inputs for comparisons. Capture supports PBR and cone GI and requires the enabled fragment storage-write feature. It checks dimensions, exact buffer sizes, storage range and uint32 size before use. RDG orders compute clear, fragment writes and transfer readback; the sample owns both buffers through diagnostic GPU completion. Resize/invalid request rejection prevents a stale capture from being accepted. Capture shaders and buffers are opt-in, with no production-persistent diagnostic volume or production readback/wait.

`prefix.lighting.bin` contains **112 bytes per pixel**, X fastest: seven consecutive little-endian float32 RGBA values. Components are:

| Index | Meaning |
| ---: | --- |
| 0 | Combined surface outgoing light |
| 1 | Analytic direct light, including the selected direct-shadow behavior |
| 2 | Diffuse outgoing environment/GI, with receiver material/BRDF and AO |
| 3 | Specular IBL with AO |
| 4 | Directly visible emission |
| 5 | Escaped-environment diffuse, with receiver material/BRDF and AO |
| 6 | Bounced diffuse, including the existing indirect gain and receiver factors |

RGB is scene-linear before exposure, Reinhard mapping, gamma and UNORM conversion. It is **outgoing light**, not raw face irradiance. Alpha is one for deferred surfaces and zero for cleared background, consistently across all seven components. Capture precedes light markers, so marker overlays do not replace the underlying surface measurement. PBR puts unoccluded diffuse IBL in component 5 and zero in 6. Metadata records component order/units, dimensions, camera matrix/position, enabled light data, environment controls, indirect gain, actual method/backend and reflectance policy. The original PPM path remains a presentation check.

## Locked baseline and results

Runner: [`tools/validate_dynamic_voxel_gi.py`](../tools/validate_dynamic_voxel_gi.py). Evidence: [`captures/`](../build/dynamic-voxel-m0/captures), including per-case `.cfg`, `.log`, `.lighting.json`, `.lighting.bin`, `.stats.json`, `.ppm`, and `locked-contract.json`. Generated room and emission fixtures are included there; Sponza uses the local glTF Sample Assets checkout.

The numeric contract was fixed before accepted captures: finite nonnegative floats, consistent alpha, zero background, component-sum and diffuse-split error bounded by `2e-5 + 2e-5 * max(abs(a),abs(b))`. The emission probe checks `[8,0.5,0.125]` within `1e-4`. Cross-voxelizer/threading HDR comparisons and presentation compatibility use exact bytes; unchanged individual components in the zero-indirect check use absolute `2e-6`.

A pilot room capture showed the initially proposed wall regions outside the visible room. Regions were calibrated to the fixed camera before collecting the accepted matrix; numeric tolerances were not relaxed. Coordinates below are normalized viewport rectangles `(x0,y0,x1,y1)`; only alpha-one pixels enter statistics.

| Region | Rectangle | Valid pixels | Averaged-policy diffuse mean RGB |
| --- | --- | ---: | --- |
| Room floor | `(0.43,0.67,0.57,0.72)` | 6,444 | `(0.275305,0.343101,0.526061)` |
| Room left wall | `(0.375,0.4,0.405,0.6)` | 5,472 | `(0.221256,0.001970,0.000934)` |
| Room right wall | `(0.595,0.4,0.625,0.6)` | 5,616 | `(0.000670,0.103939,0.000898)` |
| Room back wall | `(0.45,0.45,0.55,0.6)` | 13,824 | `(0.119474,0.119789,0.111370)` |
| Sponza hall | `(0.35,0.3,0.65,0.75)` | 119,846 | `(0.011868,0.008309,0.004321)` |

Both scenes use 64 cubed, 1280x720 presentation, a 2048-square G-buffer, a frozen point light at `(0,0.1,0.25)` with intensity 2/range 4 and shadows, environment intensity 1/rotation 0, and hidden skybox. Room eye is `(0,0,2)`; Sponza eye is `(0.55,-0.15,0)`. Complete original settings and overrides are saved per capture. These are reproducible cone baselines, not future DDA quality thresholds.

| Check | Result |
| --- | --- |
| 31 native runs: 27 HDR captures and four presentation-only controls | Passed, no Vulkan validation/synchronization errors |
| Room/Sponza x geometry/compute x inline/threaded x async off/on, averaged policy (16 HDR captures) | Byte-identical HDR within each scene |
| Four owner HDR captures and four capture-disabled controls | Presentation identical to each other and to the four saved V1 owner images |
| Component sums across 27 captures | Maximum combined error `4.843e-7`; diffuse split error `1.640e-7` |
| Known emission below/above one | 2,204 pixels contain `[8,0.5,0.125]` within the fixed tolerance |
| Zero indirect intensity | Bounced RGB exactly zero; direct/specular/emission/escaped components unchanged |
| Separate direct-only shadow fixture | Diffuse, specular IBL and emission exactly zero |
| PBR and marker checks | Component contract passes; marker toggle leaves pre-overlay HDR identical |
| Explicit unavailable DDA/RT requests | Actual metadata says cone/none; HDR equals baseline |
| Four unit-test executables | 566 passed: RenderCore 476, ConfigLoader 15, Common 36, VulkanRHI 39; no reported leaks |
| Focused native Vulkan workgroup/capability test | One passed; native storage/group-count limits and fragment-write feature agree with exposed capabilities |
| SPIR-V validation | Five composition/capture modules passed `spirv-val --target-env vulkan1.1` |
| Existing GI image regressions after M0 | All 158 owner-policy cases passed; evidence in `legacy-regression/` |
| Final built binary HDR probe | Exact match to the accepted room baseline; evidence in `final-capture/` |

Build/test logs and source snapshots are under [`build/dynamic-voxel-m0`](../build/dynamic-voxel-m0). The affected targets build with MSVC Debug. C++ changes use the repository `.clang-format` and the global C++ rules. Unit tests include invalid capture target/feature/range rejection with no submission/copy, followed by successful retry. Native captures verify the actual clear/write/copy path and threading behavior.

Verification device/toolchain: RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, glslang 16.4.0, MSVC 14.51.36231. Native runs enable synchronization validation and disable implicit overlay layers. Config-mutating runners run serially and restore `Data/engine.cfg` byte-for-byte; original SHA-256 is `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`.

The prior broad native integration-suite ordering failure and V0 padded-grid G-buffer emission probe limitation remain recorded in the V0/V1 reports. Only the named focused integration test is claimed here. M0 does not certify those unrelated cases, multi-device reproducibility, DDA quality, RT operation or performance. Next: **M1, shared query contract and compute DDA**, using the established preflight, dispatch mapping and diagnostics.
