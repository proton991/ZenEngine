# M4 receiver selection and dynamic visibility

Date: 2026-09-24. **M4 passed** for the explicit 64³ single-light profile with static and dynamic geometry. [Implementation plan](DynamicVoxelGIImplementationPlan.md), [M3 baseline](DynamicVoxelGIM3Verification.md).

## Implemented behavior

Static receiver work is selected from the same coherent G-buffer tuple and integer texel mapping used by composition. Selection marks occupied cells in the union of the eight trilinear taps and their 3³ padding donors, deduplicates them with GPU flags, then compacts them into a receiver list. This conservative union is a 4³ cell region around the interpolation base. Unselected raw values are cleared; padding still reads only valid occupied donors. Additional filter support belongs to M5 when those filters exist.

All occupied static senders retain cached static-only visibility, including off-screen senders. Selected static receivers compare each cached hit with a fresh dynamic-only query. The closest hit wins, with static winning an exact distance tie; unknown stays unknown, and an unlit first hit remains opaque. Dynamic occupied cells generate a deduplicated 5³ receiver neighborhood (3³ with radius 1), including empty interpolation cells. Dynamic receivers trace the full scene. Empty receiver cells never modify geometry occupancy or material data.

The two receiver classes have separate six-face irradiance volumes. Dynamic queries are consumed directly by the shared gather using the decoded `GIHit` contract; they do not allocate another persistent per-ray cache. Sender masks cover both complete occupied classes and use full-scene light visibility. Camera culling therefore cannot remove an off-screen sender or blocker. Final composition selects the static padded or dynamic neighborhood volume using the fetched receiver class and applies the M3 BRDF accounting once.

GPU counters produce indirect commands using M0's checked XYZ/chunk mapping. Every command stays within physical device limits; partial tiles guard the live count and unused commands do zero work. Normal frames perform no CPU receiver-count readback or additional completion wait.

Static cache initialization advances at most 4,096 receiver entries per submitted frame, across all six faces. A first-generation clear defines the full allocated cache, then bounded query batches populate it. The GPU compares the completed range with the occupied count and selects cone lighting until ready (`cache_pending=8`). CPU submission success publishes the next range; failure restarts initialization. Static compaction/grid/backend changes invalidate the cache; moving geometry or the camera alone does not. The scheduled batch counter can advance through unused reserved capacity without issuing visibility queries there; the GPU `cache_updated` counter reports actual occupied entries updated.

## Scope and storage

The explicit supported profile remains 64³, 128 rays per face, one point/spot/directional light, no diffuse environment or emissive transport, and seven G-buffer color attachments. Static and dynamic geometry now work in that profile. Owner/averaged reflectance remain selectable. `auto`, hardware RT requests and unsupported profiles retain cone selection. Filters, history, cooldown, multiple lights and broader lighting transport are still deferred.

The six decoded static hit buffers still cost 72 KiB per reserved receiver. Frame resources now cost `168*N³ + 32 + 32*ceil(N³/64)` bytes: 18 RGBA16F volumes (static raw/padded and dynamic raw), two uint light masks, two uint flag grids, two uint receiver lists, status/counters, and an argument reserve covering the smallest legal dispatch chunk. Add class/cone transition resources and the receiver-attachment reserve. The active planner checks exact total-budget arithmetic, individual buffer limits and the descriptor range before allocation. Native allocation-failure recovery remains deferred.

At a 256² G-buffer, three frames in flight and a 3,072 MiB cap, the verified profile reserves 42,502 static receivers: six 522,264,576-byte hit buffers, 37,748,736 bytes of face volumes, and a planned GI transition peak of 3,221,199,764 bytes. Common renderer, scene geometry and driver overhead are separate. This remains a diagnostic decoded layout, not a shipping memory preset; packing/profiling belongs to M8.

## Verification

Hardware/toolchain are unchanged from M3: RTX 5080, driver 616.92, Vulkan 1.4.351, SDK 1.4.357.0, MSVC 14.51.36231. Validation and synchronization validation were enabled. Dedicated GI native tests disable RT features. Both voxelizers and inline/threaded recording with async compute off/on are covered.

| Check | Result |
| --- | --- |
| CPU suites | 569 passed, with 7 existing disabled RenderCore tests |
| GI native suite | 44 passed: 24 M4 cases plus the 20 M1/M3 regressions |
| Native Vulkan recording suite | 10 passed, including retired vertex storage and real vertex-buffer/offset changes |
| Dynamic lifecycle matrix | 72 captures passed (nine states × two voxelizers × four submission modes) |
| Static scene matrix | 39 captures passed, including cold-cache fallback and the M3 surface/energy checks |
| Legacy image matrix | 158 GPU cases and image assertions passed |
| SPIR-V | 34 modules validated for Vulkan 1.1, with no RT types/capabilities |

Native checks independently integrate box intersections for all four sender/receiver class combinations. They exercise a moving unlit blocker in front of a lit static sender, camera entry/exit and background selection, off-screen senders, interpolation donors, dynamic teleport/removal/empty sets, both neighborhood radii, cache initialization across a 4,096-entry boundary, failed publication and restart. The static hit bytes remain identical through dynamic-only updates. GPU-generated dispatches execute 13 zero/boundary/partial/chunk cases per submission mode under mocked `(2,2,1)` limits, checking that every accepted receiver executes exactly once.

The lifecycle fixture contains adjacent static/dynamic receiver planes and an off-screen sender. It captures rigid motion, deformation through the vertex-update API, teleportation, removal, restoration, camera movement, actual window resize to 515×321, and return to 320×180 with a 256² G-buffer. Every visible surface has the correct instance/class tuple and ready directional data. The 72 captures contain 174,008 valid pixels and 5,528 independent CPU diffuse probes, with maximum absolute error 0.001853 under the M3 gradient/half-precision bound. Static generation and cache counters stay unchanged through dynamic/camera changes. All eight configurations produce bitwise-identical HDR per stage; restoration and camera/viewport return reproduce the initial HDR exactly. Cleared dynamic cells remain zero after teleportation/removal.

In the static room, selection updates 23,560 of 28,337 occupied static receivers and produces the same HDR bytes as the M3 all-receiver baseline. This is a work-count observation, not a GPU performance claim. The static matrix also retains normal-mapped flat/sloped/mirrored surfaces at equal, 2×, 1.5× and odd extents, strict energy checks, fallback metadata, shadows and marker isolation.

## Issues found and fixed

- Receiver selection initially bound the integer identity texture with a linear sampler. It now uses the existing nearest-sampler configuration. The indirect argument buffer also declares transfer-source usage for explicit diagnostic readbacks.
- Actual resize exposed an existing `Camera::UpdateAspect` defect: it changed the projection matrix but left published camera uniforms and frustum stale while input was idle. The method now publishes those values immediately. The failing resize/return capture became byte-identical to the initial capture.
- The geometry/async deformation run exposed an existing native-state defect: `VulkanGfxState::PreDraw` rebound cached vertex-buffer handles even for shaders with no vertex inputs. A later procedural draw could therefore use already-retired mesh storage without a declared resource reference. Binding now follows the active shader's vertex-input declaration. A native regression destroys stale vertex storage before a procedural draw, and the affected lifecycle plus actual vertex-buffer/offset tests pass. No additional wait was added to hide the lifetime error.

## Evidence and reproduction

Artifacts: `build/dynamic-voxel-m4/`, including `before/` (verified pre-M4 worktree), CPU JSON/logs, `native-final.json`, `native-recording.json`, `scenes/results.json`, `static-scenes/results.json`, `retirement-regression/`, `legacy.log`, `spirv-validation.json`, `changes.patch` and `final-verification.json`. Initial failed-run logs are preserved separately from final passing evidence.

The original `Data/engine.cfg` was restored byte-for-byte after the serial scene runners: SHA-256 `2bd8e0658e24304bd053ab9f55a9bfa8a047f616878571de9771deaf9c171d57`. Existing worktree changes remain uncommitted; the M4 patch is relative to the pre-M4 snapshot, with HEAD as the baseline for previously unchanged tracked files.

Build `scene_renderer_demo`, `DynamicVoxelGIIntegrationTest`, `VulkanRHIIntegrationTest`, and the existing CPU targets with the debug preset. Run native binaries from `bin`; use `--gtest_filter=VulkanRecordingIntegrationTest.*` for the focused native recording suite. The complete Vulkan integration executable was not run as one suite. Run `python tools/validate_dynamic_voxel_m4.py` for the lifecycle matrix, `python tools/validate_static_voxel_gi.py --output build/dynamic-voxel-m4/static-scenes` for static coverage, and the existing legacy runner for image regressions. Config-mutating runners must run serially and restore exact bytes.

`--dynamic-gi-lifecycle --capture-lighting=prefix` is a diagnostic for the generated four-mesh fixture, not a general animation API. Capture format version 2 preserves the M3 `.static.bin` prefix and appends work counters, the static occupancy-to-list map, both receiver lists, the dynamic light mask and dynamic raw faces. The JSON `offsets` object gives byte offsets; dynamic faces use the same raw/padded pair layout, with the raw value duplicated because M4 has no dynamic filter. Metadata includes generations, actual selected counts and cache progress. Readback waits remain confined to diagnostics.

DDA retains occupied-cell visibility, representative-normal/material and self-cell-rejection approximations. This does not certify triangle visibility or reproduce the paper's complete executable. M5 is the next functional milestone; hardware RT remains deferred.
