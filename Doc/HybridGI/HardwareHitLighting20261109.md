# HW RT secondary-hit lighting

2026-11-09. HW bounce and reflected-hit lighting now evaluate the actual triangle hit and trace
its incident light visibility. This supersedes the HW radiance-cache shading described in P5;
Compute hit shading retains its existing cache path.

`HybridRayResult` retains triangle identity and barycentrics from `HardwareQuery`. The shared
`ray_hit_lighting.glsl` implementation evaluates the hit's material textures, UV sets/transforms,
vertex color, diffuse weight, shading normal and emission. HW-only shaders cannot fall back to
voxel lighting when hit metadata is missing. Hits outside the voxel bounds remain shadeable.

Each secondary hit receives:

- Analytic lights evaluated at its position, with exact RT shadow segments for enabled shadows.
  Point/spot segments exclude their endpoint; directional visibility extends through the scene.
- One fresh environment sample, mixing full-environment importance and cosine sampling with
  their mixture PDF. Its RT visibility uses the actual hit position and geometric normal.
  Samples vary with the primary ray and successful frame, so temporal accumulation refines
  both transport and secondary sky integration.
- The hit material's emission when emissive GI is enabled.

This remains the existing **one diffuse secondary surface** contract, including for reflected-hit
radiance. It is not an unrestricted recursive path tracer. Sky misses and specular escape retain
their existing RT estimators. Alpha-tested candidates use the same scene-query acceptance rules.
Raster material extensions such as transmission retain their separate existing paths. A HW
bounce selection always traces rays, including when a legacy setup still requests cone bounce.
Explicit cone bounce remains available for Compute.

Primary HW shadows also trace their segments; the sampled light-fixture mask is no longer built
or consumed by `VoxelGIRenderer`. Selecting global HW RT defaults diffuse/specular GI to
full-resolution per-pixel gathering. Screen probes are available as an explicit gather choice;
their rays retain HW traversal and actual-hit shading. Temporal accumulation and spatial
filtering process those RT samples. Forward layers that cannot reuse their exact opaque
receiver trace their own RT transport rather than substituting voxel cones or nearby probes.

## Per-pass backend selection

The global Compute/HW RT selection remains a preset. Diffuse GI, specular GI, sky-cache
visibility and direct shadows retain independent selectors, so explicitly mixed configurations
remain supported. Direct shadows offer HW RT or shadow maps; the other passes offer HW RT
or Compute distance-field traversal. A HW bounce/reflection hit shades its incident light using
RT visibility even when the primary direct-shadow pass uses maps.

Compute never borrows HW RT during preparation or after switching away from HW RT. Pure
Compute uses the existing coarse voxel preview until the field is ready. In a mixed frame,
the HW gather marks unready Compute observations unavailable; field publication starts their
selected estimator with fresh history. An unavailable explicit HW selection prevents the
Compute preview from substituting for that pass. Auto resolves by capability.

Diffuse and specular share a gather selector directly below the pass table in GI tracing.
Per-pixel and screen-probe gathers both preserve each pass's selected traversal backend.
Changing an individual tracer preserves gather; selecting global HW RT resets it to per-pixel.
High/Ultra presets default to per-pixel; Low/Medium retain screen probes. Probe placement,
reconstruction and denoising are Compute operations, including when probe rays use HW RT.
Reference accumulation and ray-count diagnostics continue to use per-pixel gathering.

When all enabled diffuse/specular transport uses HW, voxelization, sky-cache integration,
radiance injection and voxel mips are not scheduled. The sky-cache selector is then unused.
Existing voxel allocations remain owned by the renderer; a cleared 1x1x1 texture supplies legacy
unused forward bindings, and HW trace variants declare no cache-lighting descriptors.
Environment importance-distribution preparation remains necessary for RT sampling.

Native validation and measurements for this contract are recorded in
`E:/Dev/ZenGroundTruth/fixes/20261109-strict-backends/report.md`.

HW bounce/reflection history resets when scene lighting or secondary-surface materials change.
It follows the scene revisions directly, including when delayed cache publication is enabled;
the old correlated cache-delta correction remains applicable only to Compute hit lighting.

## Verification

`RayQueryIntegrationTest.HitLightingUsesActualMaterialsAndRTVisibilityOutsideTheVoxelCache`
checks distinct hit materials, material edits, finite point-light shadows, directional shadows,
constant open sky and a sky occluder, with the voxel bounds deliberately excluding the geometry.
It runs under all four inline/threaded and async-compute configurations with Vulkan validation.

The existing hybrid-history, scene-query/alpha/deformation, cache-shading and screen-probe tests
remain relevant to the shared and mixed-provider paths. Application comparisons use standalone
ZenApp, immutable New Sponza configurations, independently generated references and recorded
executable/shader/input hashes. A high-sample dense result and a probe result are reported
separately: improving secondary-hit shading does not establish accurate probe reconstruction.

The earlier secondary-hit-only check (before the strict per-pixel contract above) compares the
same 512 geometry-matched receiver pixels per view
against 262,144 samples per pixel for C1_L1 and 1,048,576 for C3_L2. Noise-corrected relative RMS
of linear bounce irradiance (before primary albedo) is:

| View | Dense RT before / after | Probes before / after | Reference noise |
|---|---:|---:|---:|
| C1_L1 | 19.37% / 2.78% | 23.92% / 12.61% | 0.63% |
| C3_L2 | 42.31% / 4.00% | 43.91% / 27.28% | 0.58% |

Each entry passes the reference-noise <= quarter-error criterion. These are diagnostic subsets,
not full-image scores. The broader 32,768-receiver gallery probe check still reports 47.56% RMS
(8.13% reference noise); the subset misses some severe probe regions. The RT secondary-shading
fix therefore does not close the remaining probe-reconstruction quality problem.

Release builds, 128 selected CPU/GPU tests, 24 final comparison cases and a separate cache-size
capture completed successfully. Capture validity does not imply all image-quality gates pass.
Evidence, exact identities, images and Nsight timings are retained in
`E:/Dev/ZenGroundTruth/fixes/20261109-hwrt-hit-164136/report.md`.
