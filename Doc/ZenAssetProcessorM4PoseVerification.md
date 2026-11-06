# M4 cooked geometry pose verification

Verified 24 October 2026 in the MSVC Release build on NVIDIA GeForce RTX 5080.
The new package-rendering fixture passes **30 exact reference/reordered frame
pairs**: three animation clips, five poses and inline/threaded execution. This
adds runtime pose and transparency-order evidence to the earlier 360-source
semantic remapping audit. It does not complete all M4 or release requirements.

## Fixture and path

`ZenEditor/Tests/Fixtures/generate_geometry_scene.py` generates two original,
self-contained glTF files. Six vertices form two overlapping, coplanar triangles
in one alpha-blended primitive. Vertex storage is deliberately shuffled while
indices retain the authored corner sequence. The fixture contains two skin
joints with blended influences, a position morph, two UV sets, vertex colors and
a custom attribute. Separate clips animate skin rotation, morph weight, and both
together. Each clip has keys at 0, 1 and 2 seconds; captures include both interval
midpoints at 0.5 and 1.5 seconds.

The baseline package uses the reference profile. The comparison package uses
desktop processing with only `geometry.optimizeVertexFetch` enabled; quantization
and texture processing remain disabled. Assertions require the index stream to
change and compare resolved position, normal, both UVs, colors, joints and weights
at every corner. This prevents a no-op cook from satisfying the visual check.
The package retains one skin, three clips, one morph primitive and custom data.
The fixture's custom attribute is not consumed by these material shaders; its
presence is not a new visual proof of arbitrary custom-domain remapping.

Both packages use normal force-recook publication to v3. The test checks their
metadata, removes the temporary source copies, and loads the packages through
the editor's prepared-package path. Each clip starts from a fresh package load.
The renderer's animation API evaluates the selected time and updates deformed
geometry before drawing. This exercises the runtime adapter and normal scene
replacement/upload lifetime. The test executable includes source tools to cook
the fixtures; this is not the separate runtime-only linkage gate.

PBR rendering uses fixed directional lighting, disabled environment contribution,
no camera light or shadows, a fixed front camera, and 256x192 readbacks. The test
compares all RGBA channels exactly. The saved PPM/PNG audit additionally checks
RGB values and rejects blank or neutral frames.

## Controls and results

- All **30 reference/reordered pairs** have zero differing channels.
- A third package reverses the two triangles. All **30 order controls** visibly
  differ, by 9,411–17,211 RGB channels with a maximum channel difference of 35.
- All **18 first/last-pose controls** differ across the three packages, clips and
  execution modes. The comparison therefore exercises visible deformation.
- All **45 inline/threaded pairs** match exactly, including reversed controls.
- Two native tests pass in 2.473 seconds. Logs contain no Vulkan validation
  errors; both VMA teardowns report zero leaked bytes and the CPU tracker reports
  no leaks. Runtime is a test duration, not a performance benchmark.

The initial negative control used triangles at different depths. The engine
correctly sorted them back into the same rendering order, so all 30 controls
failed to detect the authored reversal even though the reference/reordered
comparisons passed. Inspection of `DeferredLightingRenderer` established its
per-triangle stable depth sort. The corrected fixture uses coplanar triangles,
which exercise preservation of source order at equal depth. The initial source,
binary, captures and failed log remain available; no rendering implementation or
comparison threshold was changed to make the control pass.

The array and geometry tests now share package cooking/publication, device
cleanup and fixed-light setup in `ProcessedSceneRenderingTestSupport.inl`.
All four existing array cases passed in the initial six-case run. After the
geometry-fixture correction, the two affected geometry cases passed separately.
No changes were made to the user's GI implementation.

## Evidence and limits

Evidence is under `build/asset-integration-release/`:

- `m4-geometry-pose-build.log`: successful native-test build.
- `m4-geometry-pose-tests.log/.xml`: array regression success and initial control
  failures; do not label this whole run as passed.
- `m4-geometry-pose-tests-2.log/.xml`: both corrected geometry cases pass.
- `m4-geometry-pose-captures-2/results.json`: 90 captures, six packages, comparison
  metrics and image/package hashes. PNGs are exact transcodes of saved RGB PPMs.
- `m4-geometry-pose-initial/`: selected initial fixture/source/binary snapshot;
  initial captures remain in `m4-geometry-pose-captures/`.
- `m4-geometry-pose-checkpoint/`: final selected source/binary snapshot and an
  identity manifest covering both runs and their evidence.

Reproduce with the existing configured engine build:

```powershell
python ZenEditor/Tests/Fixtures/generate_geometry_scene.py
cmake --build build/asset-integration-release --target EditorAssetRenderingTest --parallel 2
# Set this to a fresh, existing output directory to retain packages and captures.
$env:ZEN_EDITOR_GEOMETRY_CAPTURE_DIR = 'E:/Dev/ZenEngine/build/geometry-pose-rerun'
New-Item -ItemType Directory -Path $env:ZEN_EDITOR_GEOMETRY_CAPTURE_DIR -ErrorAction Stop
build/asset-integration-release/bin/EditorAssetRenderingTest.exe --gtest_filter=*CookedVertexFetch*
python ../ZenAssetProcessor/tools/validation/asset_processor_geometry_pose_report.py $env:ZEN_EDITOR_GEOMETRY_CAPTURE_DIR
```

Fixture regeneration was checked to preserve both glTF hashes. Selected source,
mid-animation and reversed-order PNGs were inspected visually. These are focused
lossless geometry checks, not lossy-quantization acceptance, a dedicated flat
provoking-vertex shader test, exhaustive animation coverage, GI/ray-query parity,
or new memory/decode/upload measurements. Those remaining plan gates stay open.
