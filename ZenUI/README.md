# ZenEngine UI

ZenEditor and ZenApp share the toolkit-independent `ZenUI` RHI/RDG renderer.
`ZenImGui` owns the private platform/input bridge, static font atlas, and ImGui draw
conversion. `ZenSessionUI` supplies diagnostics shared by editor-hosted Play and
standalone ZenApp. None of these targets is a dependency of `ZenCore`.

| Folder | Target | Include root | Contents |
| --- | --- | --- | --- |
| `Renderer/` | `ZenUI` | `Renderer/Include` (`UI/...`) | Neutral packets, texture registry, RDG renderer |
| `ImGui/` | `ZenImGui` | `ImGui/Include` (`ImGui/...`) | ImGui context, platform bridge, fonts, packet conversion |
| `Runtime/` | `ZenSessionUI` | `Runtime/Include` (`RuntimeUI/...`) | Session status, logs, pause/resume and stop controls |

Each target exposes its own include root. The platform adapter reaches the native
window through ZenCore's private `ZenWindowBackendInternal` interface. The editor's
model, render and service libraries have no ImGui dependency.

`ZEN_BUILD_RUNTIME_UI=OFF` omits the standalone ZenApp overlay. ImGui is omitted
entirely when the editor is also disabled. With the runtime overlay compiled in,
ZenApp shows it by default; `--no-ui` disables it for captures and profiling.

```powershell
./build/x64-windows-msvc-release/bin/ZenApp.exe --setup=scene.zenrender
./build/x64-windows-msvc-release/bin/ZenApp.exe --setup=scene.zenrender --no-ui
```

F1 toggles the session overlay, Space pauses/resumes, and Escape exits. Rendering
controls and source-model import live in [ZenEditor](../ZenEditor/README.md).
The former demo's model/settings UI and its `ZenRuntimeUI` library were removed.

## Dependency contract

Dear ImGui core and the selected official SDL3/GLFW platform backend are fetched together from
[`v1.92.9b-docking`](https://github.com/ocornut/imgui/tree/v1.92.9b-docking)
in `External/imgui`, using the checked FetchContent archive declared in
`External/CMakeLists.txt`.
Archive SHA-256:
`0434445157a575f452ff0f2d1681fdd90ea8939e0c6983e6f8e47c51fba1bccd`.
The upstream MIT license is retained in the fetched source as `LICENSE.txt`;
distributions must include it. CMake's `FETCHCONTENT_SOURCE_DIR_ZEN_IMGUI` can point
to an offline checkout of this exact revision. Engine configuration lives in
`ImGui/Include/ImGui/ImGuiConfig.h`. `UIContextOptions::fontSize` sets the default
font's unscaled pixel size: 13 for the runtime overlay and 15 for the editor.

This first backend deliberately uses the pinned version's supported **static font
atlas** path and does not advertise `ImGuiBackendFlags_RendererHasTextures`.
Dynamic font texture requests and user draw callbacks are not implemented.
Application images are supported through `UIRenderer::RegisterTexture`: the
adapter maps its generation-checked, renderer-local `UITextureHandle` to
`ImTextureID`. Zero, stale or unknown handles reject the packet. The
reset-render-state sentinel is supported; custom callbacks are rejected. Default font coverage is
Latin; other glyph ranges require atlas configuration before initialization.

The docking source is shared by both frontends. Docking is enabled only for the
editor; multiple OS viewports remain disabled in both. No ImGui Vulkan backend,
Vulkan native handles, secondary swapchain, or direct queue submission is used.

## Frame and resource ownership

Hosts using scene rendering call
`RenderDevice::InitializeRendererServer()` once during setup, after device
initialization and shader registration. A native viewport alone does not enable
scene rendering. `GetRendererServer()` only retrieves the borrowed instance;
`RenderDevice::Destroy()` owns its teardown. The neutral UI renderer can operate
without a renderer server.

The application polls events, starts/builds/finalizes UI on the window thread,
resolves input capture, applies settings before scene snapshot construction, and
passes an explicit scene `RenderView` and the application-owned `RenderOverlay` to
`DispatchRenderWorkloads`. ZenApp and the editor supply offscreen scene targets;
ZenApp composites its scene image into the native viewport. The native viewport
remains the separate UI/presentation destination.
The server appends the overlay after scene passes and before graph submission.

`zen::ui::BuildUIDrawPacket` copies ImGui frame storage into an owned neutral
packet: typed `UIVertex` and 32-bit index lists, projection, physical pixel
scissors, draw offsets, and engine texture handles. `UIRenderer` validates
geometry and handles before recording; it consumes no ImGui objects. A custom UI
can submit this packet directly. Recorded callbacks own their commands.

Geometry buffers grow geometrically per engine render-frame slot; uploads use
`RenderDevice` staging. `UI/ui.vert` and `UI/ui.frag` sample a 16-entry texture and
sampler array; each draw selects its slot through a push constant. Draws keep their
order and share one pass until a new image would exceed the 16 slots, so a frame with
a scene image, the font and a page of thumbnails is normally a single pass. Every
sampled image, vertex/index read and target load/store is declared in RDG.
Registry slots retain texture/sampler references. Once graph construction succeeds,
the graph retains resolved bindings independently of later unregistration.
Unregistration invalidates the generation immediately and retires ownership
through device completion tracking. Replaced geometry buffers and font images use
the same mechanism. The neutral target is an explicit color image and extent.

The first color policy matches the existing SDR scene composition: encoded UI
colors are blended over the UNORM color backbuffer, and the font atlas supplies
coverage. Linear-light UI blending, HDR, and an sRGB render target need an explicit
future color-policy change and reference-patch tests.

Destroy the runtime UI before the shader manager, device, and native window. The
platform backend restores chained callbacks. The shader manager owns the registered
UI program; RenderDevice owns the sampler cache.

## Verification

`UIRenderingTest` links only `ZenUI`, validates synthetic geometry and texture
generations, and renders a triangle without ImGui or a window in both RHI modes. It
also draws 21 commands over 20 images and checks that they compile to two UI passes.
`UIDrawPacketTest` also checks the shaders' vertex layout, texture/sampler arrays and
push-constant size through SPIR-V reflection.
`UIDrawPacketTest` checks fractional framebuffer scaling, display offsets, clipped
draws, empty/minimized frames, independent snapshots, 16-to-32-bit index conversion, base
vertices over 64K, and rejection of invalid texture IDs/callbacks or invalid
ranges. `InputControllerTest` checks captured presses and releases, focus reset,
mouse delta reset, and the final engine key and mouse-button enum values.

Run ZenApp with a prepared setup, `--validation=1`, and both `--rhi=inline` and
`--rhi=threaded`. Use `--no-ui` to check the overlay boundary. `EditorRenderingTest`
covers editor-hosted Play; `RuntimeLaunchTest`, `WindowPlatformTest` and
`UIPlatformTest` cover launch settings, native lifecycle and ImGui input routing.
Physical desktop scaling, clipboard/IME and manual interaction remain separate
acceptance checks. The demo-only UI and model-switch suites were retired with
that application.

## Upgrade checklist

Update the archive tag and hash together. Re-read the pinned font/texture contract
and SDL event routing (or GLFW callback chaining in the fallback). Re-run packet/input tests, native validation in both RHI
modes, resize/restore, and UI-disabled build checks. Verify packed vertex layout,
index width, blend policy, and font coverage. An upgrade that requires dynamic
textures must implement and test create/update/destroy retirement before enabling
the corresponding backend flag.

## Window backend

`ZEN_WINDOW_BACKEND=SDL3` is the default. SDL 3.4.18 is compiled statically from
one pinned, hash-verified archive; runtime UI and editor share one ImGui checkout.
`UIContext::Init` takes `platform::NativeWindow&`. Only the private adapter includes
SDL headers or borrows an SDL window. The application polls once, forwarding raw
SDL events once per subscribed ImGui context and translating engine input
separately. UI destruction removes the subscription before destroying its context.
Native multi-viewport mode remains disabled.

The `GLFW` option retains the old official ImGui adapter as a migration fallback.
No ImGui Vulkan renderer, SDL_Renderer, or SDL_GPU rendering path is used.

Window and framebuffer units are distinct on Retina displays. Font atlases are
rasterized at the starting pixel density, and style/font metrics follow display
scale changes unless the editor was given an explicit `--scale`. The static atlas
is scaled when moving displays; dynamic font-atlas regeneration remains deferred.
