# Current processor integration

Verified 25 October 2026 on Windows x64 with MSVC, Ninja, Debug and Release.
ZenEngine consumes the live `E:/Dev/ZenAssetProcessor` checkout through
[`External/ZenAssetProcessor.cmake`](../External/ZenAssetProcessor.cmake).
It remains unpinned. The processor build fingerprint in both engine builds and
the runtime-only build is
`8976c628910f91106f0055657a85e9ace6afa3efaf6748639b6cd7804ca04a4f`.

This checks the completed processor against the current engine working tree,
including its existing uncommitted renderer/editor changes. The current API and
schema required no further adapter changes. The source-cache API comment was
corrected to identify `LoadSourceScene`.

## Integrated behavior

- Source loading calls the standalone importer and cook pipeline through
  `ZenEngineAssetTools`. The new glTF/FBX geometry budget is active through the
  processor's default import context.
- Cache publication uses v3 packages, Zstandard level 12 and meshoptimizer.
  The cache identity includes the processor fingerprint, source content and
  processing settings; a changed processor rebuilds the source cache.
- Subsequent validated cache hits and explicit `.zenasset` opens use the Reader
  and adapt its scene into the engine's internal model. Source-independent
  package loading, cancellation and ownership are covered by the CPU suite.
- File progress forwards processed/total counts. The editor displays the ratio
  as a percentage with file counts, while discovery remains indeterminate.
- Normal-map interpretation, compressed mip chains, atlas layers and authored
  BC6H environment faces remain supported by the existing adapter.

The [processor-owned integration suite](../../ZenAssetProcessor/tests/integration/zenengine/README.md)
remains opt-in through `ZEN_EXTERNAL_TESTS_DIR`. No asset tests, fixtures or
validation scripts were moved back into the engine.

## Verification

| Check | Result |
| --- | --- |
| Debug and Release `zen_editor` builds | Passed |
| `ZenAssetEngineTests`, each configuration | 140 passed; one Windows directory-symlink privilege skip |
| `EditorInputTest`, each configuration | 25 passed |
| `EditorModelTest`, each configuration | 47 passed |
| Native texture suite, Release RTX 5080 | 14 passed, inline and threaded execution |
| Focused editor asset rendering, Release RTX 5080 | 14 passed, inline and threaded execution |
| Current CLI package loaded by `ScenePackageSmoke` | Passed: one node, one mesh, four vertices, three textures |
| Runtime-only Release `ZenCore`, asset tools/editor/UI/testing disabled | Passed |

The editor subset covers source profiles, recooking and rollback, CLI/editor
parity, compressed array and spatial packages, minification/grazing views,
skin/morph/transparency after vertex-fetch optimization, and qualification
failure/retry. Texture checks cover array/spatial sampling and compressed normal
sampling. All native cases ran without skips, with the Khronos validation layer
enabled and no VUID diagnostics. Logs retain loader configuration messages and
engine render-graph warnings. Shared shader hashes remained unchanged during the
recorded final native run. A preceding run was repeated after another build
finished generating shared shaders.

The actual `ScenePackageSmoke` link command contains Format, Reader and the
block/Zstandard/meshoptimizer decoders, with no Processor, source importers or
encoders. The editor additionally links AssetTools and Processor. A separate
tools-disabled configure/build confirms the same reader-only dependency graph.

Initial sandboxed CPU execution could not atomically replace temporary files
(Windows error 5); the unchanged binaries passed with the required filesystem
access. Missing codec downloads during Debug/runtime configuration similarly
required network access. These failed attempts are retained separately from the
successful verification logs.

## Evidence and build outputs

- [Release configure](../build/asset-integration-release/integration-configure.log),
  [build](../build/asset-integration-release/integration-build.log),
  [CPU checks](../build/asset-integration-release/integration-cpu-verified.log).
- [Debug configure](../build/asset-integration-debug/integration-configure-verified.log),
  [build](../build/asset-integration-debug/integration-build.log),
  [CPU checks](../build/asset-integration-debug/integration-cpu-verified.log).
- [Native run script](../build/asset-integration-release/final-integration/run-native.ps1),
  [results and binary hashes](../build/asset-integration-release/final-integration/native-results.json),
  [texture XML](../build/asset-integration-release/final-integration/texture/results.xml),
  [editor XML](../build/asset-integration-release/final-integration/editor/results.xml).
- [CLI cook](../build/asset-integration-release/final-integration/batch-cook.log),
  [package load](../build/asset-integration-release/final-integration/package-smoke.log),
  [actual link commands](../build/asset-integration-release/final-integration/link-commands.txt),
  [binary identities](../build/asset-integration-release/final-integration/binary-identities.json).
- [Runtime-only configure](../build/asset-runtime-only/final-integration-configure-retry.log),
  [build](../build/asset-runtime-only/final-integration-build.log),
  [dependency audit](../build/asset-runtime-only/final-integration-dependency-audit.log).

Fresh editor executables are in `build/asset-integration-release/bin` and
`build/asset-integration-debug/bin`. Ordinary presets keep their existing output
directories and will consume the same live checkout when rebuilt.

This is development integration verification, not a release or a new performance
claim. Other hosts, the full raster/GI acceptance campaign, and atlas runtime
profitability remain separate gates. HDR source/cook/cache adaptation passed the
CPU suite; the latest BC6H encoder's rendered-frame error was not remeasured here.
