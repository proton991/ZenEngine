# SDL3 migration

Implemented on 2026-10-03. SDL3 is now the default production window backend for
new Windows and macOS builds. GLFW remains selectable until native macOS
acceptance is complete. Linux is deferred. The migration changes window/input
integration; scene rendering and UI rendering continue through ZenEngine RDG/RHI.

## Build

```sh
cmake --preset x64-windows-msvc-debug -DZEN_WINDOW_BACKEND=SDL3 -DZEN_BUILD_EDITOR=ON
cmake --build build/x64-windows-msvc-debug
ctest --test-dir build/x64-windows-msvc-debug --output-on-failure
```

On macOS, use `arm64-apple-clang-debug` and that preset's build directory. Select
`-DZEN_WINDOW_BACKEND=GLFW` to use the fallback. Existing CMake caches keep their
explicit selection. If an offline build has `FETCHCONTENT_FULLY_DISCONNECTED=ON`,
populate `External/SDL3` first or configure once with that option disabled.

SDL 3.4.18 is fetched from its official release archive with SHA-256
`9cd42377704398796071b8597cd7e21da254a43bad98c4199739647adc13fa6f`.
The engine links the static target, retaining video/events/Vulkan while disabling
SDL audio, rendering, GPU and camera modules. Keep SDL's zlib license notice with
redistributions. The fetched source contains `LICENSE.txt`. One selected ImGui
platform adapter is compiled from the existing `v1.92.9b-docking` checkout.

## Boundaries and lifetime

- `NativeWindow` is the concrete public engine facade. Applications no longer use
  `GlfwWindowImpl`, GLFW handles, SDL handles, or backend key codes.
- An internal application registry owns one desktop lifetime across all windows
  on the owner thread. Destroying a second window cannot shut down the first.
  Headless RHI initialization does not initialize SDL video.
- `GetExtent2D()` reports window coordinates; `GetFramebufferExtent()` reports
  drawable pixels and returns zero when minimized. `GetDisplayScale()` reports
  display scaling; `GetUIScale()` converts that to window-coordinate UI sizing.
  The ImGui adapter rasterizes fonts at startup pixel density and adjusts style
  and font metrics when the monitor scale changes. An explicit editor `--scale`
  fixes the UI size. Moving displays scales the existing static atlas; rerasterizing
  fonts during that transition is deferred.
- `Key`, `MouseButton`, modifiers, logical printable symbols, UTF-8 text,
  composition, file-drop and pointer events belong to the engine. The existing
  keyboard/mouse state controller retains UI capture and quick-tap behavior.
- `NativeWindow::PollEvents()` pumps the process queue. `Update()` is the existing
  convenience entry point for application polling and optional runtime shortcuts.
  Resize callbacks are coalesced and dispatched after the pump, using pixel sizes.
  Native message processing never calls renderer resize callbacks directly.
- Window/title geometry belongs to the platform layer. The editor draws the bar,
  publishes an immutable geometry snapshot between event pumps, and queues window
  actions for the next safe frame boundary. SDL provides the native hit test.
  The old Win32 subclass is isolated in the private GLFW fallback.
- The private `WindowBackend` bridge is visible only to backend integrations and
  native tests. ZenImGui borrows the selected native handle and subscribes to raw
  events. Each context receives each raw event once; translated engine events are
  never forwarded a second time to ImGui. Destroy the UI before its window.
- VulkanRHI borrows a window only during surface creation, merges/deduplicates
  backend extension names, and preserves macOS portability setup. Surfaces must
  die before their window; RHI objects must be drained before native teardown.

The fallback preserves existing window/camera/UI behavior. SDL-specific text
composition and text-input-area control use SDL's native IME support; the GLFW
fallback does not acquire those new capabilities. Detached ImGui native windows
remain disabled. A future UI toolkit can use the engine contract and provide its
own private adapter without changing editor models or RDG rendering.

## Validation

Windows x64, MSVC, AMD Radeon RX 7900 XT:

| Check | Result |
| --- | --- |
| Full SDL3 Debug and Release builds | Passed |
| Editor/runtime UI ON/ON, ON/OFF, OFF/ON, OFF/OFF builds | Passed; UI-disabled variants also built with testing disabled |
| Remaining CTest targets, excluding the monolithic Vulkan integration executable | 19/19 passed in Debug and Release |
| Window-surface integration suite, inline/threaded and timeline/fence modes | 28/28 passed in Debug and Release in a fresh process |
| All Release Vulkan integration cases, one process per case | 338 passed, 6 existing capability skips, 0 failures (344 cases) |
| Engine input, logical/physical key separation, text/composition/drop, focus loss, relative-mode release, shared window lifetime | Passed |
| Independent ImGui contexts and event unsubscribe/recreation | Passed |
| Native Windows caption, all eight resize edges/corners, menu/button exclusions, maximize work area, double click and queued controls | Passed |
| Editor PBR, GI and voxel smoke tests in both RHI modes and both build configurations | 12/12 passed |
| Runtime UI smoke tests in both RHI modes and both build configurations | 4/4 passed |
| Windowed editor at explicit UI scale 2 | Passed, 1440 by 900 capture inspected |
| Runtime cover/focus/minimize/restore cycles in both RHI modes | Passed, three cycles each |
| GLFW fallback | Debug build and window/input/chrome/runtime UI checks passed |
| Standalone SDL3 evaluation probe after API migration | 43/43 checks passed |
| Formatting and public header dependency audit | Passed; public window/input interfaces contain no SDL or GLFW types |

The normal combined `VulkanRHIIntegrationTest` invocation remains a failing check
on this machine: presentation access violations reproduce with both SDL3 and
GLFW after earlier fixtures in the same process. A captured stack crosses
RivaTuner's `RTSSHooks64.dll` and the AMD Vulkan driver. The root cause is not
resolved. Running every Release case in a separate process completes with the
results above; this is a validation workaround, not a claim that the combined
suite is fixed. Vulkan validation remained enabled, and CTest retains the full
suite without a filter.

A native macOS run remains required for Cocoa resizing/dragging, maximize/restore,
Retina sizing, real IME, clipboard/capture/focus, and Vulkan/MoltenVK presentation.
Source portability and a Windows build do not establish macOS acceptance.
Physical Windows mixed-DPI transitions, real IME composition, and drag-to-snap
also need interactive checks. Windows 11 maximize-hover Snap Layouts remains
optional and is not implemented. GLFW removal is deferred until required native
acceptance is complete.

![SDL3 editor at the maximized 2560 by 1400 work area](imgs/zeneditor-sdl3.png)

Local evidence is retained under ignored `build/` paths:
`build/sdl3-final-ctest-debug.log`, `build/sdl3-final-ctest-release.log`,
`build/sdl3-final-test-results.json`, `build/sdl3-vulkan-isolated/results.json`,
`build/sdl3-smoke-results.json`, the `sdl3-options-*-build.log` files, and
`build/sdl3-migration-probe.log`. `build/sdl3-vulkan-isolated.py` reproduces the
per-case Vulkan runs; `build/sdl3-final-smokes.py` reproduces the application checks.
Those local scripts use this workstation's installed tools and sample-model path.
