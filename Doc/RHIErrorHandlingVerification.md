# RHI error handling verification

Status: implemented, 2026-10-03. This completes the no-exception error-handling delivery and R17 option B on top of the existing R1–R6 containment and R23 lifetime work. Unrelated pre-existing changes were preserved.

## Implemented contracts

- Engine-owned C++ under `ZenCore`, `ZenSamples` and `ZenUI` has no `try`, `catch`, `throw`, legacy throwing macros or exception-based test assertions. Fatal invariants use release-active `VERIFY_EXPR*` diagnostics and abort; supported local failures use checked status/null results. Third-party code and its exception compiler settings are unchanged.
- Fixed-size `RHIError` causes survive finalization, submission, batch cancellation and terminal publication. First cause and later device-loss escalation remain separate. Primary terminal diagnostics include frame/slot, operation, native code, source, resource ID and cached queue submission serials.
- Worker admission distinguishes accepted, full, closed and failed states. Explicit failure wakes producers; checked invocation runs or cancels exactly once. Ordered cleanup stays admitted through the established backend finalizer boundary.
- Checked progress/waits and acquire/present results distinguish incomplete conditions, surface reconstruction and failures. Present queue acceptance remains separate from accepted rendering and resource retirement. Failed construction releases partial ownership; failed output handles are not published. Uncertain idle/retirement errors abort before unsafe force-retirement.
- Required-frame rejection is terminal in all execution modes ([R17](RHIProductionR17Verification.md)); malformed shader input and reflection cleanup are checked ([R18](RHIProductionR18Verification.md)); interactive Windows failure diagnostics use CPU/platform UI ([R21](RHIProductionR21Verification.md)).
- glTF import treats invalid or unsupported content as a status, not an abort. `FastGLTFLoader::LoadFromFile` returns `false` and `GetError()` gives the first cause; the destination scene and the loader's geometry stay unchanged. Texture-decoding workers report through the same error sink, and every import stage skips its work after the first error. The demo keeps the active scene and shows the error; `GLTFCorpusImport` reports a failed import with exit code 1. A missing required engine asset (the voxelizer's cube model) still aborts with the loader's message.
- Color blend state checks its attachment count in every build; a ninth attachment stops with a diagnostic instead of writing past the arrays.

Pointer/bool resource factory adapters remain supported explicit failure contracts; callers must check null/false. The richer results are used at recording, executor, wait/progress, admission and WSI boundaries. This change does not redesign CPU allocators or promise recovery from third-party exceptions.

## Environment and commands

Windows, AMD Radeon RX 7900 XT, MSVC 2022 Professional, Ninja Debug and Release presets. The full default target set builds in both configurations. GPU tests use the repository's existing `7zFM.exe` alias to exclude RTSS hooks, with `VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~` and `DISABLE_RTSS_LAYER=1`. Running the original executable names on this host allowed RTSS interference; those exploratory runs are not acceptance results.

From a Visual Studio developer shell:

```powershell
cmake --build build/x64-windows-msvc-debug --parallel 8
cmake --build build/x64-windows-msvc-release --parallel 8
python tools/check_no_exceptions.py
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-debug --output build/error-handling-completion/debug-verified --smoke --executable-alias 7zFM.exe
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-release --output build/error-handling-completion/release-verified --smoke --executable-alias 7zFM.exe
ctest --test-dir build/x64-windows-msvc-debug -R '^(NoEngineExceptions|LRUCacheTest|SmartPtrTest|FlatHashMapTest)$' --output-on-failure
ctest --test-dir build/x64-windows-msvc-release -R '^(NoEngineExceptions|LRUCacheTest|SmartPtrTest|FlatHashMapTest)$' --output-on-failure
```

The other test executables (`InputControllerTest`, `ConfigLoaderTest`, `ThreadPoolTest`, `UIDrawPacketTest`, `RuntimeUIIntegrationTest`, `SceneModelSwitchTest` and `ConeVoxelGIIntegrationTest`) ran the same way as `7zFM.exe` copies with the same environment, in both configurations.

## Results

| Check | Debug | Release |
| --- | --- | --- |
| Full default build | Pass | Pass |
| CommonTest | 112 passed, 1 skipped | 112 passed, 1 skipped |
| RenderCoreTest | 557 passed | 557 passed |
| VulkanRHITest | 48 passed | 48 passed |
| VulkanRHIIntegrationTest | 336 passed, 6 skipped | 337 passed, 6 skipped |
| SmartPtrTest, FlatHashMapTest, LRUCacheTest and source-policy CTest | 4/4 passed | 4/4 passed |
| InputControllerTest, ConfigLoaderTest, UIDrawPacketTest, RuntimeUIIntegrationTest, SceneModelSwitchTest, ConeVoxelGIIntegrationTest | 7, 21, 8, 10, 8 and 4 passed | Same |
| ThreadPoolTest | Exit 0 | Exit 0 |
| Renderer smoke matrix | 12/12 passed | 12/12 passed |
| VUID/synchronization-validation errors | 0 | 0 |
| Repeated reflection test | 10,000 passed | 10,000 passed |

Release captures (`--mode=1/2/3 --frames=8 --fixed-step --vsync=0 --capture=...`) are byte-identical to the R22 and R23 baselines: SHA-256 `2f9890cd…`, `08b5aa62…` and `8d81caff…` for modes 1, 2 and 3. `GLTFCorpusImport` exits 0 on `complete_scene.gltf` and 1 with `ZEN_GLTF_IMPORT_ERROR` on a file that is not glTF.

The Release-only native test verifies non-strict shutdown diagnostics. The CommonTest skip requires directory-symlink privileges. Native skips comprise five presentation-maintenance/EXT capability cases and unsupported D24S8; skipped cases are not counted as passes. Successful native suites report no engine allocator leaks.

The no-exception audit checks all 320 owned C++ files. All 74 changed C++ files pass the repository's `clang-format --dry-run --Werror` check; `git diff --check` also passes. Full logs and machine-readable results live under `build/error-handling-completion/`, including `debug-verified/results.json`, `release-verified/results.json`, `debug-utility.log`, `release-utility.log` and the final build logs.

## Coverage and limits

Fault injection exercises command reset/begin/end, descriptor/uniform preparation, allocation/mapping, pipeline creation, acquire/present, native queue rejection, accepted prefixes, progress errors and retirement. Existing bindless transaction rollback, timeline/fence paths, native readback and resource lifetime tests remain enabled. New worker cases cover a full queue, cancellation without blocking, move-only/void checked results, first-cause preservation and later device-loss escalation. Utility tests check fatal misuse with death tests. Import tests check failed imports through the loader's status and that the destination scene and loaded geometry stay unchanged, including a texture that fails to decode on a worker thread and a later successful import with the same loader. Both ignored `FinalizeCommandLists` results identified by H1 are checked, as is the reflection result `UIDrawPacketTest` used to ignore.

The first full run of every registered test found a regression the four acceptance suites miss: converting the loader's 58 throw sites into aborts made a broken model in the demo's model picker end the process, and `SceneModelSwitchTest` aborted. Before this work the demo caught the exception and kept the active scene. Invalid content is bad input rather than an engine bug, so the loader now returns a status (see above), and `SceneModelSwitchTest` passes.

Smoke runs cover modes 1/2/3, RHI thread 0/1 and async compute 0/1, 44 fixed-step frames each. Capability-dependent skips are recorded separately. Reflection memory measurements are scoped in the R18 record.

No physical device removal, deliberate GPU hang, NVIDIA/Intel hardware acceptance, SwiftShader CI execution or manual message-box interaction was performed. R7, R8, R19, R20 and R24 remain separate production items. Validation success and bounded reflection memory on this machine do not certify those items.
