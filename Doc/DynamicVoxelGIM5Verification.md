# M5 temporal history and spatial filtering

Date: 2026-09-25. **M5 passed** for the explicit 64³ single-light profile. [Implementation plan](DynamicVoxelGIImplementationPlan.md), [M4 baseline](DynamicVoxelGIM4Verification.md).

## Implemented behavior

The default temporal mode uses `alpha(dt) = 1 - 0.97^(60*dt)`. `dynamic_voxel_gi_temporal_filter=off|fixed|elapsed` selects no temporal accumulation, the paper-reference alpha 0.03 per update, or elapsed-time accumulation. `dynamic_voxel_gi_spatial_filter=true|false` independently controls the normalized 3³ Gaussian with separable weights `[1,2,1]`. Static interpolation padding always follows the selected filter output, including with both filters disabled.

Each class has six independent history images and a uint4 per-cell record containing occupant, last-update time, last-active time and validity. Valid black irradiance remains valid. A first valid result initializes history directly. Invalid receivers are cleared, and a changed occupant cannot inherit another occupant's lighting. Grid/list/provider changes, explicit scene history revisions, filter changes, indirect-gain/shadow changes, clock rewind, gaps above 0.3 seconds, and rejected publication reset history. Structural class/enabled changes increment the scene history revision; teleport callers can explicitly call `InvalidateGIHistory`. Ordinary transforms and deformations retain filtering.

Dynamic receiver neighborhoods remain eligible for fresh queries during a 0.3-second cooldown after the object moves away. Expired cells become invalid and zero. Cooldown does not modify geometry occupancy, and is disabled in raw mode. Static camera selection includes the 6³ union needed by Gaussian donors and interpolation padding; newly revealed cells initialize before composition. Neither camera movement nor dynamic-only movement rebuilds cached static intersections.

The temporal pass reads a cell's prior image only when metadata permits it, and writes every cell. Initialization clears metadata; it does not require history-image clear passes. Spatial reads and writes use separate images. The dynamic final target also serves as temporary static filter output until padding consumes it; declared RDG dependencies order the later dynamic overwrite. Metadata publication is ordered after all six temporal consumers. No normal-frame count readback or completion wait was added.

## Precision, resources and scope

History uses RGBA32F. Repeated RGBA16F storage caused a measured downward step-response bias at 120 Hz: 2.58984375 instead of 2.6355915 after one second in elapsed mode. Full-precision history reduces the maximum error against the analytic response to 0.0000026 in the native matrix. Raw and final composition volumes remain RGBA16F. This is a correctness choice; packing and performance tuning remain in M8.

Frame resources cost `440*N³ + 32 + 32*ceil(N³/64)` bytes: 24 RGBA16F volumes, 12 RGBA32F histories, two uint4 metadata grids, two light masks, two flags and two receiver lists, plus status/counters/arguments. The active preflight includes this cost and separately checks the metadata buffer descriptor range, including under owner reflectance. Static hit storage remains six decoded 96-byte-record buffers, or 72 KiB per reserved receiver. A 3,072 MiB cap at a 256² G-buffer with three frames in flight reserves 41,535 static receivers. Common scene/renderer/driver allocations are separate.

M5 still supports explicit 64³/128-ray directional GI with one analytic light and no diffuse environment/emissive transport. M6 adds those lighting features. `auto` remains cone; hardware RT, wider-grid delivery and profiling remain deferred.

## Verification

RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, MSVC 14.51.36231. Native GI tests explicitly disable RT features and cover inline/threaded recording with async compute off/on. Validation and synchronization validation are enabled.

| Gate | Result |
| --- | --- |
| CPU suites | 570 passed; 7 existing disabled RenderCore tests |
| Native GI | 68 passed: 24 M5 cases and all 44 earlier cases |
| Filtering lifecycle | 54 captures passed: raw/temporal/full, both voxelizers, nine scene states |
| Static scene regression | 39 captures passed |
| Legacy image regression | 158 GPU cases and image assertions passed |
| Directional GI SPIR-V | 26 modules validated for Vulkan 1.1 without RT constructs |

Native tests cover constant fields in all six filter combinations, grid edges and fractional interpolation, valid black history, fixed/elapsed steps at 30/60/120 Hz, long gaps, changed occupants, structural revisions, provider generation and rejected publication. An independent Gaussian fixture checks center/axis/diagonal weights 8/4/2 and verifies that empty cells acquire only post-filter padding. Its dark occupied center rises to approximately 1.7952 irradiance from bright neighbors; the Gaussian does not prevent cross-surface leakage. No normal/visibility-aware filter was added.

Both 3³ and 5³ dynamic neighborhoods are tested in raw, temporal-only and full modes, including fresh cooldown queries, validity before/after 0.3 seconds, and explicit object removal. Camera reveal initializes hidden static history from the current result. Matched DDA hit records and a deterministic replacement run through the production filtering, padding and deferred composition; full face and composed outputs compare bitwise with identical initial histories. The separate step tests exercise repeated accumulation.

The scene fixture captures motion, deformation, teleport, removal/restoration, camera movement and actual resize/return. All 130,506 visible pixels have ready directional data; 4,146 independent CPU composition probes pass the existing texture-interpolation/half-precision bound, with maximum absolute error 0.001853. CPU padding checks average valid occupied final values. HDR component sums are recorded for raw, temporal and full captures. Scene runs use elapsed wall time; fixed-rate response claims come from deterministic-clock native tests. Static scene regressions retain normal mapping, sloped/mirrored geometry, odd/upscaled viewports, shadow and marker checks.

## Evidence and reproduction

Artifacts are in `build/dynamic-voxel-m5/`: immutable `before/`, CPU and native JSON/logs, `step-response.json`, `scenes/results.json`, `static-scenes/results.json`, SPIR-V results, and separate initial failed diagnostic logs. Capture format 3 retains the existing byte offsets and stores raw/final pairs for both classes; metadata records filter settings, time and reset state. Full-precision history readback is confined to the focused native diagnostic shader.

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `RenderCoreTest`, `ConfigLoaderTest`, `CommonTest` and `VulkanRHITest`. Run the native executable from `bin`, `python tools/validate_dynamic_voxel_m5.py`, and `python tools/validate_static_voxel_gi.py --output build/dynamic-voxel-m5/static-scenes`. Config-mutating runners must run serially. The legacy runner is invoked with its output redirected to `build/dynamic-voxel-m5/legacy`.

The serial runners restored `Data/engine.cfg` byte-for-byte: SHA-256 `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`. `changes.patch`, `changed-files.json` and `final-verification.json` record this stage relative to the immutable pre-M5 worktree. Existing changes remain uncommitted. No M6 or later milestone is certified by this report.
