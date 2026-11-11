# HW RT camera-motion flicker correction (2026-11-11)

Qualification: the measurements below used the earlier preset with normal maps disabled.
The [editor/HW RT follow-up](EditorHWRTFollowup.md) records the subsequent normal-mapped
reproduction, additional history correction and clean fullscreen performance results.

The reported flicker is in the reconstructed 4K preset: 3840x2160 output and a 960x540
per-pixel GI grid. Reproduction uses standalone ZenApp, the accepted compressed New Sponza
setup, fixed timesteps and recorded camera paths. No engine.cfg changes are involved.

## Cause and correction

Direct lighting was byte-stable when repeatedly returning to the same camera pose. Disabling
reflected GI reduced the floor-region variation by 93%. Suppressing only the unsupported-receiver
reflection fallback made little difference; that diagnostic change was fully reverted.

The main issue was reflection history/filter rejection on textured rough surfaces. At reduced
resolution, adjacent receiver samples represent different points of a roughness texture. The
fixed 0.02 roughness threshold repeatedly rejected otherwise compatible floor/wall samples.
Camera motion also collapsed the extended stationary histories to native limits, including an
eight-frame reflection limit, despite having far fewer receiver samples per output pixel.

- Reduced-resolution reflection reprojection, ReSTIR temporal reuse and spatial filtering now
  use `max(0.02, 0.15 * roughness)`, matching the relative tolerance already used for spatial
  reservoir candidates. Native-resolution receivers retain the original 0.02 threshold.
- Resolution-scaled history survives camera motion when reprojection validates it. At divisor
  four, diffuse history can retain 256 frames and moving rough reflections can retain 128.
  Moving glossy reflections retain an eight-frame cap; changed-light response remains capped
  at eight frames. Geometry, identity, normals, reflection parallax and reset checks remain active.
- No new history allocations or fresh-ray samples were added. Reservoir candidate-frame caps
  remain four for diffuse and twenty for rough reflections.

The correction is in `hybrid_guides.glsl`, `hybrid_temporal.glsl`, `hybrid_filter.glsl` and
`restir_reuse.glsl`. Reconstruction's fallback remains enabled with its original sampling.

## Reproduction results

After 128 warmup frames, the camera alternates between two poses separated by 0.5 degrees for
16 frames. Each image is compared with the image two frames earlier, so the camera pose and
direct-lighting image match exactly. Region labels below are fixed image rectangles; they are
not semantic surface masks. The window is 1280x720; the render/output extent remains 3840x2160.

| Region | Mean maximum-channel change, before / after (8-bit levels) | Reduction |
| --- | ---: | ---: |
| Whole image | 0.3646 / 0.1167 | 68.0% |
| Floor region | 0.2915 / 0.0524 | 82.0% |
| Left-wall region | 0.3427 / 0.1468 | 57.2% |
| Right-wall region | 0.3740 / 0.1316 | 64.8% |

Floor changes above 16/255 fell from 0.1138% to 0.0097% of tested pixels. This is a substantial
stability improvement, not elimination of every stochastic outlier: isolated changes as high as
190/255 remain in that region, and thin/unsupported receivers still have no temporal fallback history.

The existing eight-pose continuous orbit/dolly test also improves against separately settled
views: mean error **0.6117 -> 0.3379/255**, a **44.8% reduction**. Worst-frame p99 drops from
6 to 3/255. The settled references' own residual remains similar, 0.0742 -> 0.0721/255. This
comparison uses the preceding checkpoint's identical setup and path, including validation and
1920x1080 window / 3840x2160 rendering. It checks the tested motion rather than proving absence
of lag for arbitrary scenes or camera paths.

## Verification and artifacts

The new GPU regression checks moving, reduced-resolution roughness variation, native-resolution
behavior, large roughness changes, foreign surface identities, displaced geometry, lighting
resets, glossy response and responsive-history limits in all four submission modes.

Passing checks: **781 RenderCore**, **86 RHI**, **176 GI integration** and **four ReSTIR ray-query
integration tests** (1,047 total). Nine disabled RenderCore tests remain disabled. GPU suites run
with Vulkan validation enabled. Shader builds and repository formatting checks pass.

The final 30-frame Nsight GPU Trace uses `tools/profile_hybrid_gi.py`, the same 4K window/output
setup and base GPU clocks as the performance checkpoint: median **30.675 ms (32.6 FPS)**,
p95 **31.138 ms**, maximum **31.670 ms** on the RTX 5080. The earlier checkpoint measured
27.943 ms; this run remains within the 33.33 ms target but does not establish unchanged cost.

A normal, uninstrumented 2,000-frame run sampled **7.809 GiB** peak dedicated process memory
through Windows GPU Process Memory counters. VMA device-local allocation remains **7,845,134,496
bytes**, unchanged from the preceding checkpoint; these are different measurements. This is
still a 16 GB RTX 5080 measurement, not validation on an actual 8 GB GPU.

The matched settled image differs from the preceding 2,000-frame image by **0.093/255** mean
maximum-channel error, p99 **1/255**, p99.9 **2/255**, maximum **25/255**. Mean display-space
RGB changes by less than 0.03% per channel. The images were inspected; there is no broad loss of
lighting in this view. The preceding capture enabled validation while the final normal-use preset
disables it; scene, camera, rendering settings and frame count otherwise match.

Evidence: `E:/Dev/ZenGroundTruth/restir/flicker-20261110`:

- `before-all`, `before-direct-v2`, `before-diffuse`: isolation captures.
- `probe-no-fallback-specular`, `probe-history`, `probe-roughness`, `probe-combined`: controlled
  experiments. `probe-combined` has the final shader behavior; subsequent edits only format/comment it.
- `comparison.json`, `motion-final/report.json`: repeated-pose and continuous-motion measurements.
- `timing-final/`: Nsight trace, exported timings, exact commands and input hashes.
- `static-and-memory.json`, `dedicated-memory-final.json`: settled-image comparison and raw Windows
  counters. `flicker-final.*` in the preceding optimization evidence directory contains the normal
  run, image, allocator log and provenance.
- `regression.log`, `gpu-tests.log`, `rayquery-tests.log`, `render-tests.log`, `rhi-tests.log`: tests.
- `before/`, `fix.patch`, per-run provenance files: starting sources, scoped patch, executable,
  shader, setup, camera-path, scene-package and environment hashes with exact launch arguments.

The earlier ReSTIR acceptance gaps and actual 8 GB hardware validation remain open. No commit or
push was performed for this correction.
