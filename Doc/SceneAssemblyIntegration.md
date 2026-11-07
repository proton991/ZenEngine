# Native scene assembly integration

ZenEngine loads `.zenscene` assemblies through `asset::LoadScene`, the editor's
Open action, and ZenApp's `.zenrender` `packagePath`. Runtime loading links the
native Reader; it does not import the original glTF files. Keep the assembly and
its relative `models/` directory together when moving them.

Models are decoded sequentially, largest geometry first, to reduce overlap between
decoded source buffers and the growing destination. Each model's resources are
composed in manifest order as soon as every earlier model is composed, preserving
resource IDs, instance order and draw-order ties. Vertex/index offsets reflect the physical loading order. Geometry, materials and textures are
appended once and shared by repeated instances. Placement roots retain authored
transforms; the combined scene is normalized once. Composition remaps absolute
indices, resource IDs, UV slots, skins, morphs, variants and animation targets.
It verifies package identities and decoded metadata before publishing a scene.
Failed or cancelled loads publish no partial scene.

Assembly light policies retain canonical light definitions while controlling
runtime submission. Explicit camera selection applies to the selected instance;
an unavailable selected camera rejects the load. The editor adopts that camera,
while ZenApp's explicit setup camera overrides the initial pose. Assemblies do
not select a camera or environment implicitly by model order. Saving assembly
policies into a legacy single-model package is rejected instead of losing them.

The asset processor submodule pins published commit `a5d4506`, which includes
the native assembly reader, image schema 3, deferred image leases and direct mesh
decoding, bounded external glTF image reads, resource-write progress and a glTF
image decode limit. See [preprocessing and memory](ScenePreprocessingAndMemory.md) for the
current loading path and measurements. Processor development
belongs in the standalone `E:\Dev\ZenAssetProcessor` repository; ZenEngine
consumes its published commits through the submodule. Mipped images
retain their base level once. Older image representations still load. Static
render scenes also avoid the unused bind-vertex copy and empty deformation pass.

Full-scene testing exposed a distance-field scratch-budget failure. The planner
now queries native buffer allocation sizes and includes each generation's
instance-range table before sizing capacities. It preserves configured minimums
and rejects an unaffordable build before allocation. The final actual-allocation
check remains in place.

## Earlier New Sponza integration checkpoint

Tested on 7 November 2026 using the Release **ZenApp.exe**, RTX 5080, threaded RHI,
async compute enabled, and Vulkan validation. The desktop assembly combines
main, curtains, ivy, trees and candles:

| Runtime observation | Value |
| --- | ---: |
| Vertices | 21,017,819 |
| Indices / triangles | 97,121,067 / 32,373,689 |
| Renderable nodes | 139 |
| Submitted lights | 24 |
| GPU-culling candidates | 451 |
| Framebuffer and render extent | 1280 × 720, windowed |

The 10,000 candle lights remain in the canonical data; their assembly instance
disables submission. This is not a test of rendering 10,024 simultaneous lights.

Six PBR runs compare direct and material-batched indirect drawing at three camera
poses, four frames per run. The original camera is inside a newly added tree and
is retained as a diagnostic close-up. Its images match exactly. The elevated
view differs at two of 921,600 pixels (maximum channel difference 19/255); the
courtyard view differs at one pixel (5/255). The cause of those isolated pixel
differences was not established. There are no Vulkan validation errors or
synchronization hazards in these runs. Each culled run reports `gpu_indirect`
without fallback and no draw-data upload on its final static frame.

The checkpoint build repeats the courtyard comparison and runs both GI backends with
the same setup. Hardware GI reports `hardware_ready` and executes probe tracing.
Compute GI allocates and begins constructing the full 32,373,689-triangle field;
it remains `field_preparing` during the four-frame run, so full-scene compute GI
convergence is not verified. Neither final GI run reports allocation errors,
Vulkan validation errors or synchronization hazards. Field scratch allocations
fit their device-memory budgets: hardware 1,640,061,440 / 1,640,063,552 bytes,
compute 2,934,324,160 / 2,934,325,248 bytes.

CPU process peak working set in these PBR runs was approximately 14.68 GiB. At this
checkpoint only `RenderScene`'s copy of the static bind vertices had been removed;
2.19 GiB is that copy's calculated size, not a measured before/after peak. An
identical copy stayed in `SceneAssetData` until the [review fixes](#review-fixes).

Release regression results at this checkpoint: six assembly tests, 17 runtime-launch tests,
141 asset integration tests (one existing Windows symlink skip), 775 RenderCore
tests (nine existing disabled), and 41 native GPU tests pass. Native coverage
includes 24 culling/geometry cases, five buffer-allocation query configurations,
and 12 distance-field publication, capacity and scratch-reuse cases. The static
memory optimization preserves the scene-bounds refresh; the native field tests
verify geometry outside the radiance-cache bounds is still included.
ZenApp and the editor build successfully. Formatting, whitespace and engine
no-exception checks pass.

Generated evidence is under `build/sponza-multimodel-integration/`: exact commands,
executable/setup/assembly/model/shader hashes, copied setups, launch logs, captures,
test XML, and the synchronized processor source manifest. `Data/engine.cfg` stays
byte-identical. These are correctness smoke runs, not steady-state performance
measurements or a complete large-scene GI quality qualification.
`verification.json` summarizes the final checks. The earlier `hardware` and
`hardware-fixed` folders retain the allocation failure that led to the budget
fix; `hardware-final` and `compute-final` contain the clean final-build GI runs.

To repeat a capture with the local prepared assembly:

```powershell
build/x64-windows-msvc-release/bin/ZenApp.exe --setup=build/sponza-multimodel-integration/courtyard.zenrender --frames=4 --gpu-culling=1 --disable-rt --width=1280 --height=720 --fullscreen=0 --rhi=threaded --async-compute=1 --validation=1 --vsync=0 --fixed-step --no-ui --capture=build/sponza-multimodel-integration/repeat.ppm
```

Use `--gpu-culling=0` for the direct comparison. The setup references the local
desktop `.zenscene`; update its absolute package path when relocating the data.

## Full-scene performance investigation

The follow-up on 7 November 2026 reproduces single-digit FPS in standalone
Release **ZenApp.exe** with the full assembly. These measurements use the fixed
courtyard camera, RTX 5080, 1280 × 720 framebuffer and render extent, windowed,
threaded RHI, async compute off, VSync off and validation off. PBR disables ray
tracing; the Compute GI run explicitly selects the compute backend. All runs
keep GPU culling enabled and report `gpu_indirect` with 451 candidates and no
fallback. The user's ZenEditor viewport has not been measured, so these results
do not establish its frame rate or the effect of its camera and render settings.

The bottleneck is the forward transparency preparation and submission. The
assembly produces 1,362,710 transparent triangle records. The previous path
rebuilds and sorts them every frame, then issues 302,270 transparent draw commands
because equivalent node records still split adjacent triangles into separate
draws. A cache now reuses the prepared order while the scene identity, committed
geometry and surface revisions, renderable count and camera view remain equal.
Adjacent sorted triangles with the same material and identical complete node
data can share a draw. Topology, depth-write and index-buffer changes still split
batches; a failed sorted-index upload uses the original triangle draws and retries
the upload on later frames.

The PBR comparison uses `probe-before` and `probe-after-clean`:

| Static courtyard measurement | Before | After |
| --- | ---: | ---: |
| Transparent draw commands | 302,270 | 9,422 |
| Median forward CPU preparation | 126.079 ms | 0.051 ms |
| Mean frame interval | 191.625 ms | 3.077 ms |
| FPS from mean frame interval | 5.2 | 325.0 |
| Compared capture pixels | 921,600 | 921,600 |

The captures match exactly: zero different pixels and zero maximum channel
delta. Frame intervals come from once-per-frame log timestamps with millisecond
precision, not GPU timing queries. The before run has 12 frames and eight sampled
intervals after startup; the after run has 30 frames and 26 sampled intervals.
These short static runs isolate the observed problem. At this revision, camera
movement or committed geometry/material changes invalidated the cache and paid the
full CPU sort; the [review fixes](#review-fixes) reduce that cost. The temporary CPU
preparation logging used for these measurements is removed from the production path.

The longer compute run also exposed a separate construction failure. The old
320-texel atlas holds only 40³ = 64,000 bricks, while this scene needs 65,065.
Production field settings now use `atlasTexelsPerAxis = cellsPerAxis * 8`, allowing
one 8³-texel brick for every field cell. The supported 32³ and 64³ field grids use
256³ and 512³ atlases respectively; these field resolutions are separate from the
radiance voxel-grid resolution. The production persistent-allocation cap is now
512 MiB so two full 64³ fields can coexist during refresh alongside the maximum
32 local slots and one temporary local build, approximately 398 MiB in total.
The cap does not preallocate that memory. The generic distance-field default
remains 256 MiB, and native allocation-budget checks still apply.

In the 600-frame `compute-after` run, the global field finishes after 71 submitted
graphs in 13.208 seconds, processes all 32,373,689 triangles, allocates 65,065
bricks and reports zero unavailable cells. Compute GI then reports
`field_selected`; subsequent local fields also complete. The last 200 frames,
after global and local construction, average 6.337 ms per frame (157.8 FPS over
199 intervals), with 0.081 ms median forward CPU preparation and 9,422 transparent
draw commands. The run exits successfully without logged engine errors. Its
validation-disabled timing does not replace the separate native validation tests.

Startup and refresh stutter remain possible: the existing 250 ms throughput
frame target for field construction is unchanged. The atlas fix addresses the
capacity failure; it does not change construction scheduling or make the 13-second
warmup representative of steady-state rendering.

Evidence is under `build/sponza-multimodel-performance-20261107/`.
`summary.json` records the measurements and exact pixel comparison. Each accepted
run retains its command line, executable/setup/assembly/model/shader hashes,
copied setup, actual extents, backend and async state, launch log and image.
`Data/engine.cfg` remains byte-identical. `probe-after` is excluded because a CPU
test suite ran concurrently; `probe-after-clean` ran alone. The failed Nsight
`baseline` capture is also excluded from the timing comparison because field
construction failed and the event buffer overflowed. The earlier checkpoint
tests above describe that integration revision.

The final Release build includes ZenApp and `zen_editor` with the temporary
profiling probe removed. Final regression results are 776 RenderCore tests
(nine existing disabled), 32 native raster/culling/geometry tests and 12 native
distance-field tests passing, with no detected memory leaks. The new native
raster cases verify identical pixels when equivalent BLEND nodes merge, retain
different transforms, and invalidate the cache for camera, geometry, visibility,
material, variant and scene changes. Formatting, whitespace and no-exception
checks pass; the processor submodule has no local file changes.

The final executable also completes `compute-validated`: 600 frames at the same
1280 × 720 configuration with Vulkan validation enabled. Its global field becomes
ready in 13.374 seconds with all 32,373,689 triangles, 65,065 bricks and zero
unavailable cells, followed by the local fields. The run reports no engine errors,
Vulkan validation errors, synchronization hazards or detected memory leaks.
This is a correctness run, excluded from performance comparisons. Exact final
results and test counts are in `summary.json` under `final_validation`; the
`compute-validated/` directory retains the final executable hash and capture.

## Review fixes

A code review on 7 November 2026 changed the paths above.

The forward cache now has two levels. Classification, node merging and triangle
centers are prepared once per committed scene state. A view change only recomputes
depths and reorders them with a stable radix sort. Depths use the same arithmetic as
before, so blend order is unchanged: a 30-frame static PBR capture using the
`probe-after-clean` command is byte-identical to that run's capture.

A temporary probe measured the moving-view case in the same configuration. For 50
frames it rotated the view used for sorting by 0.5° per frame and forced a full
rebuild on every eighth frame:

| Forward preparation | `probe-before` | Moving view |
| --- | ---: | ---: |
| Median CPU time, view change | 126.079 ms | 28.6 ms |
| Median CPU time, full rebuild | 126.079 ms | 35.1 ms |
| Mean frame interval | 191.625 ms | 33.830 ms |
| FPS from mean frame interval | 5.2 | 29.6 |

`probe-before` rebuilt every frame, so it is the comparison for both rows; it did not
upload a new index order, and the moving-view run does. The moving-view row has 42
view-change and 6 full-rebuild samples. Only the sort order rotated: the rendered camera
stayed still, so this is not an orbit in the editor, whose frame rate remains unmeasured.
The probe is removed from the production path.

Static scenes no longer keep bind vertices, bind UVs or an empty per-vertex UV list.
Assemblies with at most two UV sets and no morph primitives no longer store per-vertex
UV lists, because `uv0` and `uv1` already hold those sets. Peak working set is 10.79 GiB
in the static and moving-view runs. Compared with the earlier 14.68 GiB, those runs
differ in revision and length, so this is not a controlled before/after measurement.

Assembly models now load under the same read limits as standalone packages, and each
model's build diagnostics are published with its key prefixed to the diagnostic object.
Field planning memoizes native allocation sizes by size and usage, so deferred and
repeated generation starts no longer wait on the RHI thread for sizes they already have.
Composed assemblies keep their own validation pass: `CompleteSceneLoad` builds through
`BuildValidatedSceneRuntime`, which does not validate again.

Debug CTest passes all 24 suites. Release CTest passes 25 of 27. The two others cannot
build in the Release tree, which alone sets `ZEN_EXTERNAL_TESTS_DIR`: the processor's
integration tests `EditorAssetRenderingTest` and `ZenAssetTextureIntegrationTest` expect
headers under a `ZenSamples` folder that no longer exists, so `check_pr.py` stops at that
Release build. The result listed for `EditorAssetRenderingTest` comes from a stale binary
dated 25 October. New tests cover the sorted-index upload fallback and retry, field size
memoization, and static-scene storage and model diagnostics in assemblies.

Evidence is under `build/review-fixes-20261107/`: `final-static` is the parity and
memory run, `moving-probe` the probe run, and `moving_camera_probe.py` the probe itself.
