# New Sponza editor / HW RT follow-up (2026-11-11)

The reported configuration uses the High preset, per-pixel GI, half-width/half-height
GI and enabled ReSTIR options. Compute looks stable; switching to HW RT exposes noise
and fullscreen performance drops to about 15 FPS. The earlier 32.6 FPS benchmark was
not the same configuration: it used quarter-width/quarter-height GI, a BC-compressed
scene package and **disabled normal maps**. It must not establish acceptance of the
editor's default normal-mapped rendering.

Compute retains native GI resolution and does not use the ReSTIR reservoirs. Its
distance-field/voxel lighting is also an approximation of secondary lighting, so its
smooth image is not evidence that the HW estimator has converged. The editor's `GPU
MiB` figure is VMA device-local allocation accounting, not Windows dedicated residency.

## Correction and image validation

Reflection history still rejected shading-normal changes above the fixed 0.99 dot
threshold on rough, reduced-resolution receivers. Camera motion moves these receivers
between normal-map texels. Reduced-resolution reflection reprojection now uses
`mix(0.99, 0.9, roughness * roughness)` for shading normals. The 0.99 geometric-normal
gate, surface identity, plane distance, roughness, parallax and reset checks remain.
Native-resolution behavior is unchanged. No history memory or ray samples were added.

The existing alternating-camera test was repeated with normal maps **enabled**, four
fresh diffuse rays, quarter-resolution GI and the compressed package. After 128 warmup
frames, 16 frames alternate between poses separated by 0.5 degrees. Comparing each
image with the image two frames earlier measures variation at an identical pose:

| Region | Before / after mean maximum-channel difference, /255 | Reduction |
| --- | ---: | ---: |
| Whole image | 0.17181 / 0.14256 | 17.0% |
| Floor rectangle | 0.07875 / 0.06564 | 16.6% |
| Left-wall rectangle | 0.21095 / 0.17301 | 18.0% |
| Right-wall rectangle | 0.20724 / 0.16923 | 18.3% |

This is an additional improvement over the preceding roughness/history fix, not its
82% result repeated under new settings. Isolated bright changes remain (whole-image
maximum 251/255). Unsupported reconstruction receivers and independent translucent
layers still trace observations without temporal history. This is **not a claim of
noise-free HW RT**. The camera is the retained benchmark center view, not an exact
reconstruction of the camera in the user's screenshot.

All **56 ScreenProbeGatherIntegrationTest cases** passed with Vulkan validation and
zero reported VUIDs or synchronization hazards. The extended reflection regression
checks normal-map variation at half/quarter resolution, rejection at native resolution,
large shading-normal changes, changed geometric normals and glossy surfaces in all
four submission modes, alongside its existing rejection/reset cases.

## Fullscreen performance

The accepted timings below use standalone Release ZenApp through
`tools/profile_hybrid_gi.py`, with the editor closed: RTX 5080, 3840x2160 render and
framebuffer, fullscreen, normal maps on, all HW providers, ReSTIR diffuse/spatial/rough
reflection reuse, four samples, two frames in flight, threaded RHI, async compute and
GPU culling enabled, vsync/validation/UI off. Nsight uses base clocks, a 30-second start
delay and 30 measured frames. Both runs use the corrected shader and the same camera.

| Configuration | Median GPU frame | FPS | p95 |
| --- | ---: | ---: | ---: |
| Original source-cache package, half-width/height GI | 54.183 ms | 18.5 | 54.716 ms |
| BC package, quarter-width/height GI | 27.953 ms | 35.8 | 28.086 ms |

The configuration change reduces tracing and reservoir work by reducing the GI grid
from 1920x1080 to 960x540, while rasterization, direct lighting and output remain 4K.
In the final run, ForwardTranslucent takes 11.108 ms, SceneLighting 5.193 ms, HybridTrace
2.596 ms, the two ReSTIR spatial passes 1.646 ms together, and reconstruction fallback
2.162 ms. The two runs change both texture storage and GI resolution; the full speedup
must not be attributed to the normal-history correction alone.

The normal 1,200-frame fullscreen run samples about **7.81 GiB dedicated process memory**
through Windows GPU Process Memory counters. The supplied setup stays within 8 GiB in
this measurement on a 16 GB RTX 5080. Actual 8 GB hardware and other camera views remain
unvalidated; this is not an 8 GB-card performance guarantee.

Three initial timing runs are excluded: the minimized `zen_editor.exe` was still open
and retained GPU allocations. A later counter sample found 4.22 GiB dedicated and 5.27
GiB shared residency in that editor. The profiling tool now rejects another open Zen
renderer before launching and checks again after capture. The rejection was verified
while the editor was open, without creating a capture directory; both clean captures
pass the checks. Image comparisons use fixed frame steps and are retained separately.

## Reproduction and artifacts

Evidence directory: `E:/Dev/ZenGroundTruth/restir/editor-hw-20261111`.

- `half-original-clean/`, `quarter-clean/`: accepted Nsight traces, exact arguments,
  executable/setup/package/shader hashes, actual extents and backend state.
- `quarter-before/`, `quarter-normal-probe/`, `comparison.json`: normal-mapped image
  comparison, input hashes and camera path. `normal-history.patch` is the scoped fix.
- `final.*`, `dedicated-memory.json`: final 1,200-frame run, image, provenance and memory
  samples. The `.png` is a resized inspection image; `.ppm` is the full-resolution image.
- `gpu-tests.log`, `guard-check.log`: GPU regression and profiling preflight checks.
- `NewSponza-4K.zenrender`, `Run-NewSponza-4K.cmd`: ready-to-run fullscreen configuration
  with normal maps enabled. The scene package remains the published-processor artifact
  `../optimization-4k-20261110/sponza-bc.zenasset`; no asset-processor code was changed.

The editor can open that `.zenasset` directly. Select HW RT, per-pixel gather, **Quarter
width and height**, and enable ReSTIR GI, spatial reuse and rough reflection reuse.
Opening the original glTF through its existing cache does not select the compressed
package automatically. Restart an already-running editor to load the rebuilt shaders.
