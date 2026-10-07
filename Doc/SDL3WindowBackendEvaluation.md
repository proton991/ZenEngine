# SDL3 Window Backend Evaluation

Evaluated on 2026-10-03 for ZenEngine and ZenEditor. **Recommendation: adopt SDL3 behind an engine-owned window and input API in stages, targeting Windows and macOS first.** Shared window/input integration and the existing ImGui SDL3 adapter make it a useful alternative to maintaining our own native backends. The Windows probe supports this direction; macOS validation remains required before completing the migration.

**Layout update (2026-10-07):** The shared title/menu row requirement below is
superseded for macOS. The editor now uses a native title bar and macOS system
menus, with toolbar buttons in client content. Windows retains its combined row;
see [migration status](SDL3Migration.md) for the current behavior and acceptance work.

**Required platform scope: Windows and macOS. Linux is deferred.** Both required platforms must provide the shared combined title/menu layout and pass native window, input, DPI, and rendering acceptance checks. Linux, X11, and Wayland work does not block making SDL the default or removing GLFW once Windows and macOS reach parity. Keep the API portable so Linux can be added later.

The original evaluation added an isolated Windows probe under `ZenSamples/SDL3WindowProbe`. The production migration is now implemented: `ZEN_WINDOW_BACKEND=SDL3` selects the pinned, statically linked SDL source build; `GLFW` remains a selectable fallback pending native macOS acceptance. SDL owns windowing, events, and Vulkan surface integration; ZenEngine retains its RDG/RHI renderer. See [migration status and validation](SDL3Migration.md) for current results. The probe evidence below describes the original investigation.

## Evidence and scope

The probe uses [SDL 3.4.18](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18), the stable release returned by the upstream release API at evaluation time, and the engine's existing ImGui `v1.92.9b-docking` checkout and configuration. Both SDL archives were verified against the release asset SHA256 digests before extraction.

| Check on Windows x64 | Result |
| --- | --- |
| Standalone MSVC Debug and Release builds | Passed |
| Probe derives from the existing `platform::NativeWindow` | Passed without linking ZenCore or GLFW |
| Native caption hit, all eight resize edges/corners, interactive menus/buttons | Passed using `WM_NCHITTEST` through SDL |
| Disable caption dragging while a popup is active | Passed |
| Maximize to desktop work area | Passed, 2560 by 1400 on this machine |
| Native caption double click restores the window | Passed |
| Resize, minimize, restore, separate framebuffer dimensions | Passed |
| Existing ImGui SDL3 platform backend, docking, static atlas, draw-data generation | Passed |
| Synthetic key and committed-text events reach the ImGui adapter | Passed |
| Vulkan instance extensions, surface creation, presentation support, framebuffer extent | Passed across three resize and surface recreation cycles |
| Destroy a window and create another under one SDL lifetime | Passed |

Each configuration reports **43 passed checks and zero failures**. The native window is briefly shown without activation for state tests and then closed. The probe builds UI draw data on the CPU; it does not render the editor, create a swapchain, or present frames. It therefore does not replace the existing RHI integration and validation-layer tests.

Only Windows at display scale 1 was executed. Linux and macOS conclusions below come from the pinned SDL and ImGui sources. Physical mouse dragging, snapping, mixed-DPI monitor transitions, real IME composition, clipboard integration, and performance were not tested. SDL's prebuilt release DLL was used in both configurations; SDL itself was not rebuilt with debug instrumentation.

## Fit for the editor

| Requirement | Assessment |
| --- | --- |
| One shared title/menu row | Good fit. ZenEditor draws the row and provides hit regions; SDL handles native drag/resize operations. |
| Vulkan with the existing renderer | Good fit. Replace window/surface integration; retain scene rendering, RDG, RHI, draw packets, and texture ownership. |
| One ImGui repository for runtime and editor | Supported. The current checkout contains `imgui_impl_sdl3.cpp`; another ImGui copy is unnecessary. |
| Future custom UI library | Good fit if public window, input, and hit-region types stay independent of SDL and ImGui. |
| Shared layout and working native controls on Windows and macOS | Required. Needs acceptance tests on both platforms and potentially small native adaptations. |
| Linux and detached editor panels on Wayland | Deferred; the current ImGui SDL3 platform backend does not provide detached Wayland panels. Neither blocks this migration. |

SDL's [hit-test API](https://wiki.libsdl.org/SDL3/SDL_SetWindowHitTest) is designed for application-drawn title bars and resize regions. Its callback can run during native event processing. Publish a stable geometry snapshot to the backend; do not call ImGui, allocate containers, or rebuild layout from that callback. Unsupported operations must be reported explicitly.

### Windows

The pinned backend maps drag and resize results to native non-client hit codes. The probe confirms that the existing custom Win32 subclass is unnecessary for those basic operations when using SDL. Keep the current implementation until the SDL editor path passes the same acceptance checks. [SDL Windows event implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/windows/SDL_windowsevents.c)

The portable hit-test result set has no maximize-button role. Windows 11 maximize-hover Snap Layouts therefore remains an optional Win32 integration; moving to SDL does not automatically add it. Native drag-to-snap must also be tested interactively. [SDL hit-test result API](https://wiki.libsdl.org/SDL3/SDL_HitTestResult)

### macOS

SDL creates borderless, resizable Cocoa windows and recognizes draggable hit regions. However, the pinned Cocoa `processHitTest` handles the draggable result without explicit handling for the resize-edge results. Native edge resizing may cover the desired behavior; arbitrary custom resize regions are not established by this evaluation. Require a Mac test before promising identical resize grips or double-click behavior. A small Cocoa adapter may remain useful. [Pinned Cocoa implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/cocoa/SDL_cocoawindow.m)

Use the same in-window button order to meet the uniform-layout requirement. The macOS global application menu, desktop shadows, and operating-system window management remain outside the shared editor layout.

macOS is a required acceptance gate. Extend the standalone probe with portable checks and Cocoa-specific assertions, then run it on a Mac before committing to GLFW removal. Validate borderless edge/corner resizing, dragging, double-click behavior, maximize/restore within the usable desktop area, Retina framebuffer dimensions, focus/cursor capture, text input, and the existing Vulkan/MoltenVK presentation path. A successful Windows build or Cocoa source review does not satisfy this gate.

### Deferred Linux support

The following findings are retained for a future Linux milestone and are not current acceptance gates. SDL's Wayland input backend already sends `xdg_toplevel_move` and `xdg_toplevel_resize` with the associated seat and input serial, and supports the libdecor equivalents. ZenEditor would not need to obtain these objects from GLFW internals. X11 similarly maps the hit regions into window-manager move/resize requests. [Wayland implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/wayland/SDL_waylandevents.c), [X11 implementation](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/video/x11/SDL_x11events.c)

Wayland still controls window placement and may not report externally initiated minimization. A visible window requires presenting a buffer. Do not design the engine API around guaranteed global coordinates or synchronous window-state changes. When Linux support begins, test both GNOME and KDE, including borderless operation with differing libdecor configurations. [SDL Wayland guidance](https://github.com/libsdl-org/SDL/blob/release-3.4.18/docs/README-wayland.md)

In ImGui 1.92.9b, `ImGuiBackendFlags_PlatformHasViewports` is enabled through a global-mouse-state whitelist that excludes Wayland. Docking tabs inside the main window is distinct from dragging panels into additional operating-system windows. Keep native multi-viewport mode disabled for the initial migration. [Pinned ImGui SDL3 backend](https://github.com/ocornut/imgui/blob/v1.92.9b-docking/backends/imgui_impl_sdl3.cpp)

There is also an existing engine gap: `ZenCore/Include/Graphics/VulkanRHI/VulkanRHI.h` selects Windows and macOS Vulkan platform implementations, with no Linux selection. SDL provides the relevant window/surface operations, but the rest of the engine still needs a Linux build and runtime audit.

## Proposed engine boundary

Build on `ZenCore/Include/Platform`; a new top-level module is not required for the first migration. Keep the backend implementation private and avoid exposing `SDL_Window*`, `SDL_Event`, SDL key values, or SDL property identifiers through public application interfaces.

| Owner | Proposed responsibility |
| --- | --- |
| Platform application | Own SDL initialization and shutdown, event polling, window lookup, and main-thread checks. One lifetime shared by all windows. |
| `NativeWindow` | Expose window-coordinate extent, framebuffer pixel extent, display scale, observed state, close requests, cursor mode, and window actions. Extend the existing type rather than adding another overlapping window abstraction. |
| Platform input types | Define physical keys, logical keys/modifiers, pointer events, committed UTF-8 text, and composition events independently of SDL numeric values. |
| Custom frame contract | Accept rectangles and roles for client content, dragging, and resize edges, with an explicit policy for menus and active popups. |
| ZenEditor | Draw the title bar and publish its geometry; request minimize/maximize/restore/close through the platform API. |
| Private ZenImGui SDL bridge | Subscribe privately to the platform event pump and feed each SDL event to the official ImGui platform backend. The platform layer separately translates it for engine consumers. Keep SDL types inside implementation files. |
| VulkanRHI platform integration | Obtain required surface extensions and create/destroy surfaces through a private SDL-aware integration. Public window APIs remain independent of Vulkan. |

The official ImGui SDL backend is a practical first adapter. Reimplementing it in terms of neutral events is optional later; it is not necessary to hide SDL from editor and runtime code. Never forward both raw events and their translated equivalents into ImGui, which would duplicate input. Continue deciding viewport/gameplay capture after the UI has resolved its frame, while always delivering focus changes and releases.

Keep Vulkan surface creation in VulkanRHI. A private bridge can resolve a borrowed SDL window from an engine window ID and call `SDL_Vulkan_CreateSurface`; do not put `CreateVulkanSurface` on the generic `NativeWindow` interface. Merge the active backend's required extensions with existing RHI requirements, deduplicate them, and retain the macOS portability setup. Destroy the surface before the SDL window and respect the current native-thread ownership constraints. [SDL Vulkan integration](https://wiki.libsdl.org/SDL3/CategoryVulkan), [instance extension API](https://wiki.libsdl.org/SDL3/SDL_Vulkan_GetInstanceExtensions)

Window units, framebuffer pixels, and display scale must remain distinct. SDL documents different coordinate conventions across Windows/X11 and macOS/Wayland. Convert editor hit geometry explicitly and use the actual pixel extent for swapchain and render-target sizing. Preserve the editor's initial maximize behavior by observing realized size, not by treating a successful request or state flag as proof of completion. The probe initially demonstrated that a hidden window can report a pending maximized flag while retaining its old extent. [SDL DPI guidance](https://wiki.libsdl.org/SDL3/README-highdpi), [window synchronization API](https://wiki.libsdl.org/SDL3/SDL_SyncWindow)

## Migration sequence and completion criteria

1. **Define the engine contract with the current GLFW backend.** Replace public GLFW key definitions, raw window handles, direct editor window calls, and the Vulkan viewport's concrete `GlfwWindowImpl` cast. Preserve current behavior and run input/window regression tests. In parallel with the contract work, extend and run the macOS probe to resolve the required platform's borderless-window behavior early.
2. **Add SDL as a selectable build backend for Windows and macOS.** Use one pinned SDL dependency for runtime and editor and one selected window backend per executable. Initialize video and required input facilities only. Keep GLFW available as a migration fallback. Headless rendering must continue without initializing a desktop video subsystem. Defer Linux build and backend integration.
3. **Connect ImGui and Vulkan privately.** Select `imgui_impl_sdl3.cpp` from the same ImGui source. Exercise pixel-size events, minimized/suspended rendering, surface recreation, and shutdown under both inline and threaded RHI execution. Retain the engine renderer and static atlas policy.
4. **Move custom title-bar behavior into the shared window contract.** Port the native chrome acceptance cases to the SDL path. Test editor and runtime focus/capture, title dragging, native resizing, maximization, and startup at the realized framebuffer size. Apply the same frame contract to any future additional editor windows.
5. **Complete Windows and macOS acceptance.** Run Windows mixed-DPI checks and macOS Retina and borderless resize checks. On both platforms, exercise the shared title/menu layout, real IME input, file drops, clipboard, relative mouse capture, focus loss, minimized windows, and full editor/runtime rendering. A Mac run is required; record supported capabilities rather than assuming matching native behavior.
6. **Make SDL the default and remove GLFW after Windows and macOS parity.** Remove GLFW, its ImGui adapter, concrete casts, key constants, native adapters superseded by SDL, and build dependencies. Validate all editor/runtime-UI build-option combinations and headless tests on the required platforms. Linux support and detached Wayland panels are deferred and do not block completion.

Window events and rendering suspension deserve the most attention during migration: hidden, minimized, maximized, and framebuffer-sized are separate facts. A backend switch must not change the existing rule that native window work happens on its owner thread and GPU resize/recreation happens at a safe frame boundary.

## Dependency and maintenance assessment

SDL is a broader library than GLFW. This evaluation does not establish a performance or binary-size improvement, and adopting its window backend does not require adopting SDL audio, SDL_Renderer, or SDL_GPU. For production, pin an archive and checksum using the existing `External/CMakeLists.txt` convention, choose shared or static deployment deliberately, and verify the chosen source-build feature settings with the ImGui adapter. The official prebuilt package was sufficient for this probe.

SDL uses the zlib license; retain its license notice with source distributions and the dependency's existing attribution records. [Pinned license](https://github.com/libsdl-org/SDL/blob/release-3.4.18/LICENSE.txt)

The recommendation is a **go for staged SDL integration on Windows and macOS**. Full GLFW removal should follow engine API cleanup and actual validation on both required platforms. A private GLFW fork or complete native rewrite is not justified by the combined-title-bar requirement based on the evidence collected here. Linux remains a separate future milestone.

## Reproduction

See [the probe README](../ZenSamples/SDL3WindowProbe/README.md) for the dependency digests and build commands. Local evaluation logs are in `build/sdl3-evaluation/probe-debug.log` and `build/sdl3-evaluation/probe-release.log`; these are ignored build artifacts. The repository probe remains independent of the production CMake configuration.
