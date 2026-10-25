# ZenAssetProcessor M0 verification

**Captured:** 22 October 2026, starting 23:34 Asia/Seoul; fresh-build follow-up
ran from 23:41 through 23:58.
**Status:** Initial CPU baseline captured; a fresh source snapshot has also been
built in Debug and Release. Additional fresh-build verification is described
below. This report does not mark the full M0 milestone complete.

## Evidence and reproducibility

The [baseline runner](../../ZenAssetProcessor/tools/validation/asset_processor_baseline.py) records the existing
engine behavior without modifying engine sources, CMake, or original assets.
Run from `E:/Dev/ZenEngine` with a new output directory:

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_baseline.py `
  --output build/asset-processor/m0-new-run `
  --corpus E:/Dev/glTF-Sample-Assets `
  --sponza E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf
```

The initial run is retained under
[`build/asset-processor/m0-20261022`](../build/asset-processor/m0-20261022).
That directory is local build evidence, not a committed/distributed golden-fixture
collection. Promote selected packages, manifests, and license notices into the
standalone project's tests before treating them as release compatibility fixtures.

| Evidence | Content |
| --- | --- |
| [metadata.json](../build/asset-processor/m0-20261022/metadata.json) | Machine, exact commands/settings, binary hashes and timestamps, original Git HEAD and dirty-tree snapshot |
| [source-manifest.json](../build/asset-processor/m0-20261022/source-manifest.json) | SHA-256 identities of engine asset sources/headers and checked-in fixtures/tests |
| [results.json](../build/asset-processor/m0-20261022/results.json) | Process exit codes, timings, memory, eight v2 source-free cases and package hashes |
| [corpus-input-manifest.json](../build/asset-processor/m0-20261022/corpus-input-manifest.json) | 1,430 glTF/GLB files and referenced external buffers/images; 1,297,970,991 bytes, no missing inputs |
| [sponza-input-manifest.json](../build/asset-processor/m0-20261022/sponza-input-manifest.json) | 74 source/buffer/image files; 2,171,830,243 bytes, no missing inputs |
| [gltf-corpus.json](../build/asset-processor/m0-20261022/gltf-corpus.json) | All 339 glTF/GLB import/structure/animation results |
| [fbx-corpus.json](../build/asset-processor/m0-20261022/fbx-corpus.json) | All 22 checked-in FBX fixture outcomes |
| [ibl-golden.json](../build/asset-processor/m0-20261022/ibl-golden.json) | External IBL source dependency identities, retained v2 package and source-free reload |
| [Debug XML](../build/asset-processor/m0-20261022/debug-tests.xml), [Release XML](../build/asset-processor/m0-20261022/release-tests.xml) | Exact test names, results, timings and skip reason |

Existing executables were copied into `binaries/debug` and `binaries/release`
before execution. Debug executables were built at approximately 18:46:11 and
Release at 18:44:42 Asia/Seoul on 22 October. Their complete SHA-256 identities
are in metadata. The baseline is explicitly tied to those binaries. It is not
evidence that the current dirty graphics tree was rebuilt or that every current
source file produced those binaries. No existing engine build was modified.

Original checkout HEAD: `309ebb2a69478a382152ebf7fc46648a334fa43e`.
The original dirty-file list is preserved; those unrelated changes were retained.
The local glTF sample corpus has no Git metadata, so content manifests identify it.

Machine: Windows 11 10.0.26300; AMD Ryzen 9 9950X3D, 16 cores/32 logical CPUs;
31.11 GiB visible RAM. Adapters: NVIDIA GeForce RTX 5080 (driver
32.0.16.1742) and AMD Radeon integrated graphics. No GPU was selected or exercised
by these CPU benchmark commands. The original Windows PowerShell capture of the
localized OS caption has an encoding artifact; the version and numeric fields
are unaffected, and the runner now uses PowerShell 7 UTF-8 output.

Compiler/cache: MSVC 14.51.36231; C++20; Ninja; Release `/O2 /Ob2 /DNDEBUG`.
Copies of the original CMake caches are beside each binary snapshot. For later
engine rebuilds, initialize the toolchain with:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/Tools/Launch-VsDevShell.ps1' `
  -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
cmake --build build/x64-windows-msvc-release `
  --target CommonTest GLTFCorpusImport SceneLoadBenchmark -j 10
```

## Test outcomes and parity coverage

- Debug: **111 passed, 1 skipped, 0 failed**, out of 112 selected tests.
- Release: **111 passed, 1 skipped, 0 failed**, out of 112 selected tests.
- Full glTF sample corpus: **339/339 passed**, including structure checks, finite
  geometry, skin/morph deformation, first/middle/last animation sampling, posed
  bounds, and material-variant selection checks implemented by
  [GLTFCorpusImport](../../ZenAssetProcessor/tests/integration/zenengine/CommonTest/GLTFCorpusImport.cpp).
- FBX: **21 accepted; 1 expected rejection**. The rejected
  `max_transformed_skin_7500_binary.fbx` explicitly reports unsupported
  dual-quaternion skinning. Its rejection is documented in the
  [fixture README](../ZenSamples/CommonTest/Assets/FBX/README.md) and tested by the
  focused suite. It is not counted as successful import support.

The skipped test in both configurations was
`SceneModelCatalog.DirectorySymlinksDoNotDiscoverModelsOutsideTheRoot` because
Windows denied symlink creation without the required privilege.

The test filter was:
`*Scene*:*Gltf*:*GLTF*:*Fbx*:*FBX*:*Animation*:*Morph*:*PhysicalUnits*`.
Coverage includes source-independent v2 read/write, malformed data and allocation
limits, checksum/semantic corruption, cancellation, atomic publication,
dependency/content invalidation, cache eviction, concurrent writers, async scene
loading, morph/custom attributes, physical units, source compatibility, and typed
animation targets. `SceneAssetIO.RoundTripsSourceIndependentStaticAndAnimatedAssets`
compares exact runtime geometry and representative poses; the extended-data test
compares material fields, authored mips, integer custom attributes and HDR values.

The corpus runner validates its recorded invariants. It does not compare every
vertex value between arbitrary source formats or establish rendered visual fidelity.

## Retained v2 source-free packages

For each of these cases, the runner copied the source into a dedicated output
directory, recorded source structure/animation results, generated a cache package,
deleted only that temporary source, and loaded the explicit `.zenasset` using
`GLTFCorpusImport`. All nine packages loaded with matching reported counts and
successful animation/finite-data checks. The IBL case also deleted its copied
external buffer and images before loading the package.

| Fixture | v2 package bytes | Relevant feature |
| --- | ---: | --- |
| `complete_scene.glb` | 10,391 | Skin, morph, animation, hierarchy and textures |
| `typed_animation_targets.gltf` | 6,837 | Four typed animation channels |
| `all_primitive_modes.gltf` | 10,836 | Seven primitive modes |
| `blender_293_embedded_textures_7400_binary.fbx` | 203,379 | Embedded texture ownership |
| `combined_morph_skin_ascii.fbx` | 5,447 | Combined skin/morph and animation |
| `maya_instanced_skin_7700_binary.fbx` | 37,057 | Shared skinned geometry and five animation channels |
| `max_instanced_material_7700_ascii.fbx` | 11,606 | Three instances with material differences |
| `maya_camera_light_axes_y_up_7700_ascii.fbx` | 1,737 | Cameras/lights and axis conversion |
| `EnvironmentTest/glTF-IBL` | 12,686,095 | External resources and image-based HDR lighting |

Packages are under `golden/<fixture>/cache/*.zenasset`, with exact names and
SHA-256 hashes in the JSON reports. Embedded metadata retains importer diagnostics.
The instanced-skin and camera/light fixtures each produce one diagnostic both
on cold import and all three warm loads. Original source assets were untouched.
The separately retained Sponza package is performance evidence, not a source-free
test, since its original inputs remained available.

These cache packages include local source paths/build identities, so package bytes
and names can differ when regenerated at another absolute path. The saved package
hashes identify this capture; they are not a portable reproducible-build claim.

## CPU performance baseline

The benchmark uses `SceneLoader(1, 0)`, default import settings, an initially absent
asset-cache directory, and three warm loads. Cold timing includes source import
and CPU runtime construction. Warm timing includes dependency checks, v2 file
read/validation, and CPU runtime construction. Cache publication is the wait for
remaining background serialization after the cold scene was released; it is
neither the full serialization duration nor GPU scene-publication latency.

The OS filesystem cache was **not flushed**. Cold means asset-cache miss, not cold
disk access. Runs were sequential but the machine was not isolated from other
applications. Small fixtures are correctness inputs and their millisecond-scale
timings are unsuitable for optimization claims.

| Asset | Cold seconds | Publication wait seconds | Warm seconds (three runs) | Package bytes |
| --- | ---: | ---: | --- | ---: |
| Main Sponza | 6.149 | 3.626 | 3.328 / 3.193 / 3.382 | 5,156,323,819 |
| EnvironmentTest IBL | 0.038 | 0.007 | 0.019 / 0.011 / 0.012 | 12,686,095 |
| Complete scene fixture | 0.002 | 0.002 | 0.005 / 0.001 / 0.001 | 10,391 |
| Combined morph/skin FBX | 0.002 | 0.001 | 0.007 / 0.001 / 0.001 | 5,447 |

Sponza loads 2,049,137 vertices, 11,241,054 indices and 77 runtime textures,
with zero diagnostics. All warm loads report `cacheHit=1` and all publication
waits report zero failures. The Sponza package is 4.802 GiB.

Windows `GetProcessMemoryInfo` recorded **7.029 GiB peak working set** and
**7.859 GiB peak private commit** for the Sponza benchmark process, covering its
cold load, cache writer and three warm loads together. Sampling occurs every
20 ms and once after exit, using the OS-maintained peak counters. These figures
do not split memory by phase, include allocator/runtime overhead, and do not
include GPU allocations. The engine's own allocator log separately reports
5.57 GB peak tracked allocation and no outstanding tracked allocations at exit;
that narrower counter must not replace process memory.

Default animation bake settings in the captured source are 60 samples/second,
translation tolerance `1e-5`, rotation tolerance `1e-4`, scale tolerance `1e-5`,
value tolerance `1e-4`, maximum 10 subdivisions and 64 Mi values. These are the
existing importer settings, not newly approved quantization or image-quality
thresholds.

## Fresh source-matched builds and quality configuration

The follow-up captures an independent source snapshot at
[`baseline-source`](../build/asset-processor/baseline-source). Its
[manifest](../build/asset-processor/baseline-source-manifest.json) records 970
project files and 18,425 external dependency files with SHA-256 hashes, the
original Git HEAD, and the dirty checkout state. The
[patch](../build/asset-processor/baseline-source.patch) preserves tracked changes.
The snapshot contains the active rendering changes as captured, without altering
their implementation. Subsequent engine checkout changes cannot affect these builds.

Fresh `baseline-debug` and `baseline-release` configurations both succeeded with
MSVC/Ninja, C++20, and `FETCHCONTENT_FULLY_DISCONNECTED=ON` using the copied,
already-populated external sources. Both configurations built `CommonTest`,
`GLTFCorpusImport`, `SceneLoadBenchmark`, `RenderCoreTest`, `EditorModelTest`,
`EditorRenderingTest`, `EditorPreviewTest`, `VulkanRHIIntegrationTest`, and
`scene_renderer_demo` successfully. These were fresh build directories, without
reusing the prior engine's compiled objects. Shader generation writes into the
isolated snapshot rather than the working engine's shader directory.

Exact commands, exit codes and completion timestamps are in the
[Debug build record](../build/asset-processor/baseline-Debug-build-results.json)
and [Release build record](../build/asset-processor/baseline-Release-build-results.json).
The reproducible local scripts are
[snapshot_m0.py](../build/asset-processor/snapshot_m0.py),
[build_m0.ps1](../build/asset-processor/build_m0.ps1), and
[verify_fresh_m0.py](../build/asset-processor/verify_fresh_m0.py).
The snapshot script refuses an existing snapshot directory; retain the manifest
and rename the output location when making a new capture.

The versioned [quality contract](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json) now
defines exact reference semantic/pixel/pose comparisons, preserves the existing
FBX bake tolerances, names required timing/memory/identity measurements, and
requires at least three cold and five warm samples for performance comparisons.
Desktop geometry/texture/atlas loss thresholds are explicitly **provisional**:
they must be evaluated against M4/M5 evidence before lossy passes are enabled by
default. The configuration defines requirements; it is not evidence those future
passes have met them. Reference pose checking requires all authored keys and
midpoints, which exceeds the existing corpus runner's three samples per clip.

### Fresh verification results

The [fresh report](../build/asset-processor/fresh-verification/results.json)
contains commands, binary identities, timings and image comparisons. XML and raw
logs are in the same directory. A final hash recheck found **zero changes** to the
970 project files and 18,425 external inputs used for compilation.
The [final recheck](../build/asset-processor/fresh-verification/final-snapshot-recheck.json)
repeats that audit after the supplemental tests and captures.

| Suite | Debug | Release |
| --- | --- | --- |
| Selected scene/import tests | 111 passed, 1 skipped | 111 passed, 1 skipped |
| RenderCore | 697 passed, 9 disabled | 697 passed, 9 disabled |
| EditorModel | 41 passed | 41 passed |
| EditorPreview | 16 passed | 16 passed |
| EditorRendering | 53 passed, 2 disabled after corpus-path setup | 53 passed, 2 disabled |
| Full Vulkan integration | Rerun: 409 passed, 4 skipped; initial abort retained | 410 passed, 4 skipped |
| Focused Vulkan asset/upload subset | 56 passed, 4 skipped | Covered by full suite |

The same symlink-privilege skip remains in the scene tests. Four Vulkan skips are
intentional parameter cases that disable timeline submissions, while packed
uploads require timeline submissions (and in two cases distinct graphics/compute
queues). Disabled test counts are separate from passed counts.

The first Debug EditorRendering attempt aborted because the relocated snapshot's
default model root resolved to a missing sibling corpus directory. The complete
failure log is retained. Creating a junction under the build output,
`build/asset-processor/glTF-Sample-Assets -> E:/Dev/glTF-Sample-Assets`, restored
the existing checkout layout without editing sources. The complete rerun passed;
its XML is [here](../build/asset-processor/fresh-verification/debug-EditorRenderingTest-corpus-configured.xml).
Recreating this junction is a prerequisite when reproducing tests from the local
snapshot. No original model files were changed.

The initial full Debug Vulkan run exited **3**, after entering
`TimelineAndFence/VulkanPresentationSubmissionTest.RejectedCopyAfterSuccessfulFramesRetainsAcquisition/0`.
It produced no completed XML; its earlier successful log entries are not a full
suite pass. That exact case passed when isolated, and the focused asset/upload
subset passed. Both outcomes are retained in
[supplement.json](../build/asset-processor/fresh-verification/supplement.json).
One further full Debug run then completed with **409 passed and 4 skipped**;
its [XML](../build/asset-processor/fresh-verification/debug-Vulkan-full-rerun.xml)
and [run record](../build/asset-processor/fresh-verification/debug-Vulkan-full-rerun.json)
are retained separately. The initial abort was not reproduced or explained, so
the rerun does not establish that the intermittent failure is fixed. No RHI fix
was attempted as part of asset extraction.

Fresh Release corpus verification passed **339/339** assets. All nine retained
earlier v2 golden packages also loaded through the fresh reader with source copies
still absent; see [legacy-golden-reloads.json](../build/asset-processor/fresh-verification/legacy-golden-reloads.json).

The fresh Release Sponza series used three separate cold asset-cache imports,
each followed by five warm loads. Package size remained **5,156,323,819 bytes**.

| Measurement | Samples | Minimum | Median | Maximum |
| --- | ---: | ---: | ---: | ---: |
| Cold source + CPU runtime load | 3 | 4.195 s | 4.409 s | 7.347 s |
| Warm v2 + CPU runtime load | 15 | 1.480 s | 3.165 s | 4.083 s |
| Publication wait after cold release | 3 | 2.871 s | 4.432 s | 6.003 s |

Peak working sets were 7.056 / 7.126 / 6.332 GiB; peak private commits were
7.898 / 7.914 / 7.946 GiB. All loads succeeded; warm loads hit cache and
publication failures stayed zero. This shared machine was not isolated and OS
caches were not flushed. The spread matters: differences against the initial
single-run capture are not evidence of a processor optimization.

### GPU publication, memory, and paired visual references

The fresh Release renderer selected **NVIDIA GeForce RTX 5080**. Captures used
PBR mode 2, 640×480, three fixed-step frames followed by the capture frame,
validation and synchronization validation enabled, RT disabled, no UI, no
dynamic lights, and the same camera-fitting/environment settings for each pair.
The exact per-run configuration bytes and commands are retained.

Seven visible source/v2 pairs were **pixel-identical**, with zero differing RGB
channels and zero RMSE, and no validation errors or nonzero process exits:

| Asset | Purpose | Source publication | v2 publication |
| --- | --- | ---: | ---: |
| DamagedHelmet | Static textured PBR | 85.192 ms | 42.930 ms |
| SimpleSkin | Skin deformation | 29.971 ms | 29.583 ms |
| AnimatedMorphCube | Morph animation | 32.315 ms | 30.135 ms |
| SimpleInstancing | Shared mesh instances | 29.577 ms | 30.158 ms |
| EnvironmentTest IBL | Authored HDR environment | 21.864 ms | 20.808 ms |
| FBX blender sausage | Animated FBX geometry | 31.076 ms | 30.591 ms |
| FBX embedded textures | Decoded FBX image ownership | 30.606 ms | 31.692 ms |

The first five pairs are in the main fresh report; the FBX supplements are in
[fbx-visuals.json](../build/asset-processor/fresh-verification/fbx-visuals.json).
Representative inspected images:
[helmet](../build/asset-processor/fresh-verification/visual-0-source.png),
[skin](../build/asset-processor/fresh-verification/visual-1-source.png),
[morph](../build/asset-processor/fresh-verification/visual-2-source.png),
[instances](../build/asset-processor/fresh-verification/visual-3-source.png),
[IBL](../build/asset-processor/fresh-verification/visual-4-source.png),
[animated FBX](../build/asset-processor/fresh-verification/fbx-visual-0-source.png),
[textured FBX](../build/asset-processor/fresh-verification/fbx-visual-1-source.png).
These establish parity at the captured view and animation time, not complete
material conformance or all-frame animation equivalence. The animated FBX uses
the existing automatic framing, which clips the top of the model in this view.

An additional `combined_morph_skin_ascii.fbx` pair produced matching **all-black**
images at the automatic view. Those files and measurements are preserved, but
are explicitly excluded from the seven visible visual references. CPU geometry,
animation and source-free tests still cover that fixture.

Publication times are the existing `SceneRendererDemo::PublishModel` wall timer,
covering placement, camera setup, `RenderScene::Init`, resource reconfiguration,
scene replacement and lighting setup. They exclude source import and CPU runtime
construction, and do not separately identify GPU execution duration. Each is one
observed publication, not a performance distribution.

`--gpu-memory-stats` reports VMA committed-allocation peaks across the entire
application lifecycle. For the helmet pair, both runs used **412,614,656 peak
committed bytes**, including **278,396,928 device-local bytes**. IBL used
208,666,624 / 141,557,760 bytes. Skin, morph, instancing and animated-FBX pairs
each used 211,288,064 / 144,179,200 bytes. Embedded-FBX source/v2 runs peaked at
261,102,080 / 241,875,968 committed bytes, respectively. Every run reported zero
remaining VMA bytes at teardown.

These are allocator block peaks, including render targets, environment resources,
staging, and other renderer work. They do **not** isolate model textures from
geometry, do not represent physical VRAM residency, and should not be compared
as encoded texture sizes. Split texture/geometry/staging allocation measurements
remain outstanding.

## Remaining M0 evidence

- The fresh snapshot/builds above resolve the original binary-provenance gap;
  the first capture's measurements remain tied to its earlier binaries.
- Separate GPU texture/geometry/staging allocation and GPU upload execution
  duration are still unmeasured. The new VMA peaks and host publication timers
  provide only their stated broader scopes.
- Paired visible references now cover the listed representative scenes;
  exhaustive material and animation-time image coverage remains outstanding.
- The quality contract is versioned, but exhaustive adapted-resource parity and
  qualification of the provisional desktop thresholds remain outstanding.
- Retained packages/reports still need appropriate distribution placement and
  upstream license handling. This report does not authorize redistributing the
  large Sponza asset or the entire external corpus.

The baseline script was syntax-checked with `python -m py_compile`, and its core
run plus FBX/IBL supplements and dependency-manifest stages were executed. The
initial run was supplemented as the driver acquired those stages; the final
command above executes them together on a fresh output directory.
