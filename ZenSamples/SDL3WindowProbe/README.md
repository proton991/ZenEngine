# SDL3 Window Probe

An isolated Windows x64 evaluation of SDL3 for ZenEngine windowing. This project
is deliberately absent from the production build. It shares the engine window
value types, links the current ImGui source with
its official SDL3 platform backend, and exercises real Windows/Vulkan integration.
No ZenCore, GLFW, SDL renderer, or ImGui Vulkan renderer is linked.

Read [the evaluation](../../Doc/SDL3WindowBackendEvaluation.md) for the decision,
proposed engine API boundary, platform limitations, and migration sequence.

Windows and macOS are required migration targets; Linux is deferred. This probe
currently validates Windows only. Adding and running macOS acceptance checks is
required before the migration can be considered complete.

## Dependencies

- Windows x64, Visual Studio C++ toolchain, CMake, Ninja, Vulkan SDK and driver.
- Engine dependencies populated so `External/imgui` contains `v1.92.9b-docking`.
  An older `_deps/zen_imgui-src` cache is not a substitute; CMake checks the version.
- Extract the official [SDL3 development package for Visual C++](https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-devel-3.4.18-VC.zip).
  The probe requires SDL 3.4.18 exactly. It copies the package's SDL3 DLL beside
  the executable and does not install anything system-wide.

Release asset checksums used in this evaluation:

| Archive | SHA256 |
| --- | --- |
| `SDL3-devel-3.4.18-VC.zip` | `78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd` |
| `SDL3-3.4.18.zip` source archive, for audit only | `9cd42377704398796071b8597cd7e21da254a43bad98c4199739647adc13fa6f` |

## Build and run

From a Visual Studio x64 Developer Command Prompt at the repository root, using
the package extraction path from this evaluation:

```bat
cmake -S ZenSamples/SDL3WindowProbe -B build/sdl3-evaluation/probe -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSDL3_DIR="%CD%/build/sdl3-evaluation/deps/vc/SDL3-3.4.18/cmake"
cmake --build build/sdl3-evaluation/probe
ctest --test-dir build/sdl3-evaluation/probe -V
```

Adjust `SDL3_DIR` if the package is elsewhere. To validate Release, use a separate
build directory and `-DCMAKE_BUILD_TYPE=Release`. `ZEN_IMGUI_SOURCE_DIR` can override
the default `External/imgui` location without downloading a second ImGui copy.

The probe briefly shows a test window without activation to check realized native
window state, then hides and destroys it. Success is `RESULT passed=43 failed=0`.
Both Debug and Release passed on the evaluation host at display scale 1.0, with a
maximized client of 2560 by 1400 matching the desktop work area.

## Limits

The native checks use Windows messages to test SDL's hit-test integration, including
caption double click. They do not automate physical mouse dragging or snapping.
Input checks inject key/committed-text events into ImGui's adapter, which does not
prove hardware input, IME composition, or engine capture behavior.

UI checks generate draw data without displaying it. Vulkan checks create an
instance and three surfaces, query presentation support and pixel dimensions,
and destroy the surfaces. They do not create a swapchain or render/present frames.
This probe is not a complete SDL engine backend or a Linux/macOS acceptance test.

The original probe inherited the minimal `NativeWindow` interface. Production
migration made that type the concrete engine facade, so this standalone probe
now uses only its value types. Production acceptance is covered by
`WindowPlatformTest`, `UIPlatformTest`, `EditorWindowChromeTest`, and the existing
RHI/UI integration tests; see [migration status](../../Doc/SDL3Migration.md).
