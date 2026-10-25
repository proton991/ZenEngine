# ZenAssetProcessor extraction verification

Verified on 23 October 2026 with MSVC 14.51, C++20 and Ninja. The independent
project is `E:/Dev/ZenAssetProcessor`; the engine consumes it through
`ZEN_ASSET_PROCESSOR_SOURCE_DIR` during local development. This records the CPU
extraction and runtime/source-tool split before the v3 cache migration.

## Results

| Check | Result | Local evidence |
| --- | --- | --- |
| Engine CommonTest, Debug and Release | 162 passed, one existing symlink privilege skip in each configuration | `build/asset-processor/commontest-{debug,release}-verified.xml` |
| EditorModelTest, Debug and Release | 41 passed in each configuration | `build/asset-processor/editormodeltest-{debug,release}-verified.xml` |
| Standalone processing tests | Synthetic processing regression and FBX fixture regression passed | `ZenAssetProcessor/build/processor-port`, CTest filter `ZenAssetProcessing` |
| glTF corpus | 339/339 accepted; all reported structural counters identical to fresh M0 | `build/asset-processor/m2-gltf-corpus.json`, `m2-gltf-comparison.json` |
| FBX fixtures | All 22 outcomes and structural records identical to M0: 21 accepted and one expected dual-quaternion rejection | `build/asset-processor/m2-fbx-comparison.json` |
| Runtime-only package loader | All nine retained v2 golden packages loaded | `build/asset-processor/runtime-golden-load.log` |
| Runtime-only dependency closure | No standalone Processor, source parser or model decoder libraries/translation units | `build/asset-processor/runtime-link-closure.json` |

The corpus checks include finite geometry, animation sampling, deformation and
variant selection exercised by `GLTFCorpusImport`. Matching structural counters
does not establish rendered visual equivalence. No GPU test ran in this phase.
The logs and generated packages above are local build evidence.

## Boundaries exercised

`ZenCore` discovers and loads prepared `.zenasset` packages. Its neutral-scene
adapter explicitly translates all scene, material, image, animation, hierarchy,
skin, morph, custom-attribute, IBL and provenance fields, then validates the engine
representation before publication. Unsupported image encodings fail explicitly.

`ZenEngineAssetTools` owns source format discovery, source loading and background
cache policy. Its importer calls the independent processor and adapts its result.
Editor and source demos link this target explicitly. Core package loading retains
the asynchronous prepare/cancel/complete lifecycle without source parser links.

Build configurations used `build/asset-integration-debug`,
`build/asset-integration-release`, and `build/asset-runtime-only`. The runtime-only
configuration sets `ZEN_BUILD_ASSET_TOOLS=OFF`, `ZEN_BUILD_EDITOR=OFF`,
`ZEN_BUILD_RUNTIME_UI=OFF`; `ScenePackageSmoke` links only `ZenCore`.

## Windows publication regression

Concurrent cache reads exposed the existing v2 writer's replacement race:
`_wfopen` readers denied delete sharing, and `MoveFileExW` could fail while readers
held the previous destination. Package readers now retain a handle that allows
delete sharing. Publication uses `FileRenameInfoEx` with replacement and POSIX
semantics, preserving an already-open reader's old bytes while new opens observe
the complete new file. The v2 file schema and field encoding are unchanged.

`SceneAssetIO.AtomicReplacementPreservesAlreadyOpenReader` covers retained-reader
semantics. The bulk cancellation test observes the exclusive temporary writer
through an attributes-only handle and still verifies cancellation before
publication, unchanged destination bytes, and temporary-file cleanup.

An early parallel test run and subsequent sequential reproduction exposed this
race; the final sequential Debug and Release suites above passed after the fix.
Using `--gtest_repeat` on the whole suite also revealed an unrelated pre-existing
global counter in `LockTests.cpp` that is not reset between iterations; final runs
used a fresh process for each configuration.
