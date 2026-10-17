# RHI production readiness: R20 verification

Status: implemented and verified 2026-10-17.

## Behavior

`RHIOptions` defaults validation on without `NDEBUG` and off with `NDEBUG`. Both Windows Release and Performance presets define `NDEBUG`; Performance inherits Release. Release-active invariant checks are unchanged.

The scene demo and editor preserve this default unless the user supplies `--enable-validation` or `--disable-validation`. Repeating the same flag is allowed; conflicting flags are rejected before Vulkan initialization. The demo's `--validation-printf` explicitly enables validation, works with `--enable-validation` in either order, and conflicts with `--disable-validation` in either order.

Native RHI, ray-query, Cone, model-switch, window-platform and UI test executables use a shared `ZenVulkanTestMain` that explicitly enables validation before tests run. Editor native fixtures already enable it explicitly. The Release deliberate-leak death test retains its explicit opt-out. CPU-only `VulkanRHITest` keeps the build default and adds a regression covering the default and both setter overrides.

Validation smoke/capture runners now pass `--enable-validation`. `benchmark_voxel_gi.py --validation` explicitly enables it; ordinary benchmark runs still explicitly disable it. The Performance launch wrapper preserves the build default and accepts `--enable-validation` for diagnostics.

## Verification

Windows x64, MSVC 14.51.36231, Vulkan SDK 1.4.357.0, NVIDIA GeForce RTX 5080, driver 617.42. Debug, Release and Performance builds succeeded. Build configurations were processed sequentially because their shader outputs share `Data/SpvShaders`.

Native runs used synchronization validation (`VK_LAYER_VALIDATE_SYNC=1`) and disabled implicit layers (`VK_LOADER_LAYERS_DISABLE=~implicit~`, `DISABLE_RTSS_LAYER=1`). Executables were copied to isolated `7zFM.exe` paths to use this machine's existing RTSS exclusion. Editor runs used separate settings directories under the verification output directory.

| Check | Debug | Release | Performance |
| --- | ---: | ---: | ---: |
| Demo/editor default, overrides, repeats and conflicts | 19 passed | 19 passed | 19 passed |
| `VulkanRHITest` | 62 passed | 62 passed | 62 passed |
| `CommonTest` | 114 passed, 1 skipped | 114 passed, 1 skipped | Not run |
| `RenderCoreTest` | 588 passed | 588 passed | Not run |
| `VulkanRHIIntegrationTest` | 385 passed | 386 passed | Not run |
| `RayQueryIntegrationTest` | 16 passed | 16 passed | 16 passed |
| `ConeVoxelGIIntegrationTest` | 24 passed | 24 passed | Not run |
| `SceneModelSwitchTest` | 8 passed | 8 passed | Not run |
| `WindowPlatformTest` | 4 passed | 4 passed | Not run |
| `UIRenderingTest` | 5 passed | 5 passed | Not run |
| `RuntimeUIIntegrationTest` | 10 passed | 10 passed | Not run |
| Renderer smoke matrix | 12 passed | 12 passed | Not run |

The one skipped CommonTest case needs Windows directory-symlink privileges; it is not counted as a pass. RenderCore's eight existing disabled benchmarks were not run. The additional Release RHI case deliberately checks non-strict teardown with validation disabled.

The pre-commit review also rebuilt `UIDrawPacketTest` and `EditorRenderingTest` in Debug and Release, then ran the affected `UIShaderReflectionTest.*` and `EditorMeshPreviewShader.*` tests. All four test runs passed.

The 57 startup cases checked exit codes, successful instance creation, and the actual enabled-layer startup log. `SetupInstanceLayers` logs the same layer list that `CreateInstance` passes to `vkCreateInstance`; merely enumerating a supported layer was not treated as enabling it. Both applications enabled validation by default only in Debug. Explicit enable/disable and demo printf combinations behaved as specified; all conflicting combinations exited with code 1 without creating a Vulkan instance.

Every native acceptance and smoke log confirmed validation enabled. There were no VUID or synchronization-validation errors. Smoke coverage includes modes 1/2/3, inline/threaded RHI, and async compute off/on. Changed C++ ranges pass the project clang-format check; modified Python scripts pass syntax checks; the no-exceptions policy check and `git diff --check` pass. The other image/capture workflows received explicit validation flags and syntax checks; their full image matrices were not rerun for this option change.

## Reproduction and local evidence

From an MSVC developer shell, build the relevant preset and run its production acceptance suite:

```powershell
cmake --build --preset x64-windows-msvc-release --target scene_renderer_demo zen_editor VulkanRHITest VulkanRHIIntegrationTest CommonTest RenderCoreTest -j 10
python tools/verify_rhi_production.py --build-dir build/x64-windows-msvc-release --output build/rhi-validation-defaults/release/acceptance --smoke --executable-alias 7zFM.exe
```

Use the Debug preset/path for Debug acceptance. The executable alias is specific to this machine's overlay exclusion and can be omitted on a machine without overlay injection. For default/override checks, launch each build of the demo with `--frames=3 --mode=2 --no-ui --fixed-step --vsync=0`, or the editor with `--frames=3 --hidden --windowed --settings-dir=<isolated-directory>`, then vary the validation flags described above.

Local evidence is under `build/rhi-validation-defaults/`: `build-{debug,release,performance}.log`, per-configuration `options-matrix.json` and process logs, Debug/Release `acceptance/results.json` and `extra-suites.json`, and the Performance CPU/ray-query logs. The local `verify_configuration.py` records and asserts all startup combinations.
