# ZenAssetProcessor M3 verification

**Updated:** 23 October 2026.
**Scope:** The raw v3 package, standalone reader/writer/CLI, engine adapter,
automatic v3 cache, and explicit v2 compatibility. Compression and texture
encoding have separate gates. This report does not declare the full plan done.

## CPU and installed-package evidence

The integrated Debug and Release `CommonTest` suites each ran 165 cases:
**164 passed, one skipped, zero failures**. The skipped case was
`SceneModelCatalog.DirectorySymlinksDoNotDiscoverModelsOutsideTheRoot`, which
requires symlink privileges. Both editor model suites passed **41/41**.

Evidence:

- [Debug engine XML](../build/asset-processor/commontest-m3-debug.xml) and
  [log](../build/asset-processor/commontest-m3-debug.log).
- [Release engine XML](../build/asset-processor/commontest-m3-release.xml) and
  [log](../build/asset-processor/commontest-m3-release.log).
- [Debug editor XML](../build/asset-processor/editormodeltest-m3-debug.xml) and
  [Release editor XML](../build/asset-processor/editormodeltest-m3-release.xml).

These include adapter ownership after source destruction; rejected images,
references and enums; cancellation and geometry limits; runtime discovery;
source-free v3 reads; v3 cache publication; metadata preservation; source,
settings and external dependency invalidation; corrupt-cache handling;
background/parallel publication; and eviction. The existing v2 IO cases remain.
The inspected tests support those contracts, rather than treating suite counts
alone as proof of them.

A separately configured engine consuming the installed Processor SDK also
passed **164 cases with the same one skip**. A fresh runtime-only engine using
the installed Reader SDK loaded **13 packages**: all nine retained v2 goldens,
three v3 engine fixtures, and one package cooked by the installed CLI after
its temporary source was removed. The link/source audit found no source
importer or encoder libraries/translation units in that runtime build.
See [installed verification](../build/asset-processor/installed-package-verification-m3.json),
[installed engine XML](../build/asset-processor/installed-engine-tests-m3.xml),
and [package-load records](../build/asset-processor/installed-runtime-goldens-m3.json).
These records describe the raw M3 snapshot before subsequent codec work.

The standalone raw scene corpus report records **360/360 accepted scenes**
(339 glTF variants and 21 accepted FBX fixtures), with independent direct
public-field comparisons after serialization and deterministic raw rewrites.
The one unsupported dual-quaternion FBX remains an expected rejection in M2.
The comparison covers supported geometry, hierarchy, materials/bindings,
images/mips, skins, morphs, custom attributes, animation and environment records.
Evidence lives in
`E:/Dev/ZenAssetProcessor/build/processor-port/scene-corpus-results.json` and its
retained test logs. This proves import-to-package round trips; it does not by
itself compare the extracted importer's floats with the old importer.

A bounded Clang 22.1.3 libFuzzer/ASan run exercised **14,307 inputs in 121
seconds** without sanitizer or invariant failures. Both instrumented reader
suites passed. Its recorded input limit was 262,144 bytes, seed 12345, using
the authored rich scene as seed. MSVC STL container annotations were disabled
to match the bundled fuzzer ABI. See
`E:/Dev/ZenAssetProcessor/build/reader-fuzz/verification.json` and `first-run.log`.
This finite run does not establish exhaustive malformed-input safety and
predates codec fuzz coverage.

## Rendered parity and the normal-rounding correction

The [visual harness](../../ZenAssetProcessor/tools/validation/asset_processor_visual_parity.py) uses the seven
visible M0 references: DamagedHelmet, SimpleSkin, AnimatedMorphCube,
SimpleInstancing, EnvironmentTest IBL, animated FBX sausage, and FBX embedded
textures. It cooks each source with the raw reference profile and compares
direct source and v3 publication in the same renderer, then compares source
pixels with frozen M0. Captures use 640x480 PBR mode 2, three fixed-step frames
plus capture, RT disabled, no UI, validation and synchronization validation,
and the exact saved M0 configuration. `--no-asset-cache` forces the source run
through the importer. All process commands, configurations, logs, package
identities, and image hashes are retained.

The first run found **7/7 pixel-identical direct-source/v3 pairs** and **6/7
pixel-identical frozen-M0 comparisons**, with no validation errors. Helmet
differed in three RGB channels by one 8-bit level (RMSE 0.0018042196).
The exact threshold was not relaxed. The original failure remains in
[m3-visual-parity](../build/asset-processor/m3-visual-parity/results.json).

Loading the unchanged v2 helmet in the current renderer reproduced frozen M0
exactly, while a repeated source run reproduced the same differences. This
isolated the extraction path from unrelated rendering changes. Comparing
the canonical vertex payloads found 14,760 differing normal components,
maximum absolute error 5.960464477539063e-8; all other vertex fields matched.
The standalone vector division used multiplication by a rounded reciprocal,
where the original implementation divided each component. Restoring direct
component division made all 14,556 helmet vertex records bit-identical.

The correction is in standalone `src/Format/MathOps.h`; the frozen source
normal and expected canonical float bits form a regression in
`tests/ProcessingTests.cpp`. That regression passed. The diagnosis and before/
after payload comparisons are retained in
[m3-helmet-diagnostic](../build/asset-processor/m3-helmet-diagnostic/results.json),
[original payload comparison](../build/asset-processor/m3-helmet-diagnostic/vertex-comparison.json),
and [corrected payload comparison](../build/asset-processor/m3-helmet-diagnostic/fixed-vertex-comparison.json).

The corrected Release renderer and raw CLI were copied into an immutable
verification directory before the final run. **All seven direct-source/v3
pairs and all seven frozen-M0 comparisons are pixel-identical**, with zero
differing RGB channels, zero RMSE, no validation errors, and successful exits.
The original configuration was restored and both executable identities
remained unchanged. See
[final visual report](../build/asset-processor/m3-visual-parity-fixed/results.json).

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_visual_parity.py `
  --renderer build/asset-processor/m3-visual-fixed-binaries/scene_renderer_demo.exe `
  --processor build/asset-processor/m3-visual-fixed-binaries/zenassetc.exe `
  --reference-dir build/asset-processor/fresh-verification `
  --output build/asset-processor/m3-visual-parity-fixed
```

The raw package sizes in that run were:

| Asset | Raw v3 bytes |
| --- | ---: |
| DamagedHelmet | 86,119,041 |
| SimpleSkin | 10,394 |
| AnimatedMorphCube | 15,189 |
| SimpleInstancing | 40,493 |
| EnvironmentTest IBL | 12,742,435 |
| FBX sausage | 208,494 |
| FBX embedded textures | 204,638 |

The new renderer includes the initial transport decoder additions, but these
packages used the uncompressed reference path. The frozen raw binaries and
reports remain available as later codec work proceeds. Earlier full CPU suite
results above predate this normal correction; the corrected processor's
specific regression and the final seven-case GPU run are the post-fix checks.

The harness copies the original `Data/engine.cfg` into its output directory,
checks for external edits before each configuration write, restores only its
own last written bytes, and records restoration. Binary identities are
checked again at completion; an identity change prevents a passing result.
No unrelated GI source was modified for these checks.

## Limits of this evidence

- Rendered parity is limited to the recorded views and fixed animation time.
  It does not prove every animation frame or general material conformance.
- Byte/allocation limits bound the documented payload and record storage,
  not every parser-private allocation or total process RSS.
- Publication logs are host wall times. VMA peaks are whole-application
  committed allocation. They do not establish GPU upload execution duration
  or separate texture/geometry/staging costs.
- Raw reference results do not establish compression quality, atlas behavior,
  additional source formats, or final release packaging. Those remain later
  milestones.
