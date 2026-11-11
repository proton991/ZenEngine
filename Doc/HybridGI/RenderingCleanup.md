# Rendering cleanup (2026-11-10)

Removed confirmed unused rendering code after the ReSTIR GI / 4K performance checkpoint.
The existing uncommitted implementation and supported rendering modes are preserved.

## Removed

| Removed code | Evidence and replacement |
| --- | --- |
| `LightFixtureMasks.h/.cpp`, mask build/dilate shaders and their shared include | No instance, caller or shader consumer remains. Current HW shadows use exact triangle candidates in `ShadowMapping/light_occluders*`, followed by the original ray query when needed. |
| `ShadowMapRenderer.h/.cpp`, `ShadowMapRenderSP`, `ShadowMapping/evsm.*` | No renderer instance or caller remains. Active shadow maps are owned by `SceneShadowRenderer`; HW shadows use ray queries. The old EVSM render/copy/mipmap passes were unreachable. |
| `VoxelRenderer.h/.cpp`, `RenderResource.h/.cpp` | Empty or entirely commented out. Removed CMake entries and includes of the empty resource header. Current voxelization uses `ComputeVoxelizer` or `GeometryVoxelizer`. |
| `VoxelizationLargeTriangleCompSP`, `voxelization_large_triangles.comp`, `compute_voxelizer_common.glsl` | Only shader registration remained, with no pass or producer for its large-triangle queue. Current compute voxelization dispatches triangle records directly. |
| `capture_volume.comp`, `capture_reflectance.comp`, `capture_sky_cache.comp`, calibration `capture_owners.comp`, `capture_gbuffer.comp`, `reference.geom/.frag` | No capture entry point, test, tool or dynamically constructed program name selects them. Removed the six corresponding program registrations and reference-program class. |
| Calibration `spatial_alpha.vert`, `spatial_alpha_hardware.comp/.frag`, `spatial_alpha_query.glsl` | No test, tool or program loads these fixtures. Their include chain became orphaned when the original spatial-alpha checks were removed. |
| Unused shader-wrapper getters, per-object uniform/push-constant copies and obsolete structure definitions | Callers pass local values to RDG bindings. Retained the structure types still used by those callers. |

Total: **26 source/header/shader files and 10 shader-program registrations removed**. The shader
glob now excludes 15 obsolete entry points; their generated SPIR-V and dependency files were also
removed locally. These inactive paths did not execute in the accepted 4K frame, so this change
does not establish an FPS or frame-memory improvement.

## Retained active paths

- Pure HW GI, mixed HW/Compute and Compute distance-field GI.
- Cone lighting and the coarse voxel preview used while the Compute field prepares.
- Both voxelizers, pixel and screen-probe gathers, ReSTIR and the original/reference estimators.
- Temporal accumulation, all five spatial filter iterations, reduced-resolution reconstruction
  and its full-resolution fallback for receivers missing from the reduced grid.
- Exact shadow candidate cache, scene shadow maps, transparency, debug views and used capture/test shaders.

The audit checked the engine, editor, UI, samples, tests and tools, including shader filename
references and include dependencies. `TraceProgram` and `ProbeTraceProgram` construct mixed-mode
names dynamically; these variants are active and retained. A shader unused by one preset was not
treated as globally unused.

## Verification

Release builds pass for ZenApp, SpvShaders, RenderCoreTest, RuntimeLaunchTest, VulkanRHITest,
ConeVoxelGIIntegrationTest and RayQueryIntegrationTest.

- **781 RenderCore**, **21 runtime** and **86 RHI** unit tests pass. Nine previously disabled
  RenderCore tests remain disabled. The RHI suite exposed an outdated reflection expectation:
  pure HW tracing no longer binds voxel hit caches. The test now checks that absence and still
  checks read-only cache bindings in the mixed HW/field variant.
- **172 GI integration tests pass** with Vulkan validation enabled, covering the active voxel,
  temporal, ReSTIR, screen-probe and reflectance paths across submission modes.
- **108 ray-query/distance-field integration tests pass** with validation enabled, including the
  four ReSTIR submission-mode tests and exact shadow-cache checks. Four opt-in real-lantern
  experiments are skipped; 12 previously disabled tests remain disabled. Together the suites
  report **1,168 passing tests**. No Vulkan validation errors were reported by either GPU suite.
- The matching **200-frame ZenApp 4K captures are byte-identical**, with zero changed pixels.
  Both use the accepted reconstructed-GI preset: 3840x2160 output/framebuffer, 960x540 GI,
  HW pixel gather, windowed, async compute available, fixed timestep and no UI. This image
  comparison uses the preset's validation-disabled setting; GPU integration tests enable validation.
- All retained shader binaries match their pre-cleanup SHA-256 hashes. Only the 18 obsolete
  binaries (15 entry points plus three micromap variants) and their dependency files disappeared.
- Changed C++ files pass the repository formatter; diff whitespace checks pass. The shader
  filename/include audit has no remaining orphan candidates or missing GLSL includes. Runtime
  naming rules were checked separately for the dynamically selected variants.

Evidence: `E:/Dev/ZenGroundTruth/restir/cleanup-20261110`. Capture files and their command/input
hash manifests use the `cleanup-before-complete` and `cleanup-after` prefixes under
`E:/Dev/ZenGroundTruth/restir/optimization-4k-20261110`.

No commit or push was performed. Broader quality and actual 8 GB GPU acceptance remain as
recorded in the preceding performance checkpoint; this cleanup does not close those gates.
