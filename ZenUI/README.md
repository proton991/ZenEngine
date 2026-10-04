# ZenEngine runtime debug UI

The runtime debug overlay and ZenEditor share the toolkit-independent `ZenUI`
RHI/RDG renderer. `ZenImGui` owns the private SDL3 context/input bridge, static font atlas,
and ImGui draw conversion. `ZenRuntimeUI` contains session-only renderer controls.
None of these targets is a dependency of `ZenCore`.

| Folder | Target | Include root | Contents |
| --- | --- | --- | --- |
| `Renderer/` | `ZenUI` | `Renderer/Include` (`UI/...`) | Neutral draw packets, texture registry, RDG renderer |
| `ImGui/` | `ZenImGui` | `ImGui/Include` (`ImGui/...`) | ImGui context, platform bridge, fonts, packet conversion |
| `Runtime/` | `ZenRuntimeUI` | `Runtime/Include` (`RuntimeUI/...`) | Runtime debug panels and `RuntimeSceneControls` |

Each target exposes only its own include root, so code that links `ZenUI` alone cannot
include ImGui or runtime-panel headers. `scene_renderer_demo` links `ZenRuntimeUI` from
`ZenSamples/CMakeLists.txt`; ZenUI does not modify sample targets. The ImGui adapter
reaches the native window through ZenCore's `ZenWindowBackendInternal` interface
target, which exposes only the window adapter boundary header.

The editor's model, render and service libraries also have no ImGui dependency. See
[ZenEditor](../ZenEditor/README.md) for the editor's build options and controls.

## Build and run

`ZEN_BUILD_RUNTIME_UI` defaults to `ON`. Configure it `OFF` to build runtime targets
without linking the runtime overlay. ImGui is omitted entirely when both this
option and `ZEN_BUILD_EDITOR` are `OFF`. With the runtime option enabled, interactive
`scene_renderer_demo` runs show the panel by default. Automated frame-limited,
profile, and capture runs leave it disabled unless explicitly requested.

```text
scene_renderer_demo --ui
scene_renderer_demo --no-ui
scene_renderer_demo --ui --frames=48 --smoke-test --mode=2
```

Press **F1** to switch between debug interaction and camera control. In debug mode,
the cursor is visible and scene input is blocked; Tab navigates widgets. In camera
mode, the existing movement controls and Escape-to-close behavior apply. Focus
loss returns to debug mode and clears held inputs. Window close works in either
mode.

Below FPS, the panel shows live engine GPU memory, its lifetime peak, and a usage
bar against GPU capacity. Hover the readout for total commitments across all memory
heaps. Counters include allocator pools and resources awaiting retirement; they
exclude driver, swapchain and other applications' memory. They update without
waiting for GPU work and do not require `--gpu-memory-stats` (which enables the
shutdown log). The optional averaged-reflectance budget is a separate allocation limit.

The panel has **GI**, **Scene**, and **Config reference** tabs. The reference lists
all supported `engine.cfg` keys, including optional properties for all 32 light
slots, with startup values and control locations. Live controls cover every GI
setting, camera position, environment illumination and skybox visibility, light
count and properties, light markers, and orbit animation. Above the tabs, the
**Model** selector recursively lists `.gltf` and `.glb` files beneath the configured
`model_base_path`. Search filters the relative paths, including model variants;
**Refresh models** rescans the directory. Selecting a model loads it for the current
session without restarting or rewriting `engine.cfg`. Each file is imported when
selected; a failed import reports its error and keeps the current scene. Successful
switches use the new model's camera or fit its bounds, reset scene lighting and
animation controls, and release the previous model's GPU resources. The environment
texture still requires a restart. The application implements `RuntimeSceneControls`; scene
and animation ownership remain outside the shared UI renderer.

Cone tracing is the only voxel GI method. The GI tab contains cone parameters,
voxel resources, shadows, and independent analytic/environment/emissive diffuse GI
contributions. Direct lighting, visible emission, and specular IBL remain independent.
The obsolete method selector, directional filters, query backend, cache and automatic
GI budget settings have been removed. Old unknown config keys are ignored by the loader.

**Apply changes automatically** starts enabled. Live sliders update while dragging;
resource changes (grid, active reflectance budget, shadow resolution, light count) wait until
the edit ends. Disable it to stage changes, then use **Apply pending**. **Reload
current** discards the draft. Values are validated before applying. Resource changes
use `RendererServer::ApplyVoxelGISettings` and may wait for GPU work. Changes affect
the current session; they do not rewrite `engine.cfg` or scene files.

Enabling **Averaged reflectance**, or raising the resolution while it is enabled,
raises its budget to at least the required extra storage: **5 MiB at 64³, 40 MiB
at 128³, and 320 MiB at 256³**. The panel displays this minimum and the active
reflectance policy. A manually entered budget below the minimum, or an unsupported
GPU buffer size, is rejected before waiting for GPU work or replacing the current
resources. Changing the budget while owner reflectance is active does not rebuild
the voxel volumes. Valid policy/resolution changes still rebuild those volumes and
can briefly pause rendering.

Cone uses mesh shadows for direct light and analytic radiance injection. Injection
samples the owner triangle surface rather than its voxel center. PBR fallback has
neither voxel GI nor these mesh shadows; the panel reports the last rendered view.

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

Hosts using scene rendering or `RuntimeDebugUI` call
`RenderDevice::InitializeRendererServer()` once during setup, after device
initialization and shader registration. A native viewport alone does not enable
scene rendering. `GetRendererServer()` only retrieves the borrowed instance;
`RenderDevice::Destroy()` owns its teardown. The neutral UI renderer can operate
without a renderer server.

The application polls events, starts/builds/finalizes UI on the window thread,
resolves input capture, applies settings before scene snapshot construction, and
passes an explicit scene `RenderView` and the application-owned `RenderOverlay` to
`DispatchRenderWorkloads`. Runtime callers construct the view from the native
viewport's current buffers each frame; editor callers supply offscreen scene
targets. The native viewport remains the separate UI/presentation destination.
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

Run the native demo with validation in inline and threaded RHI modes. The existing
`--smoke-test` exercises scene-mode changes and window resize/minimize/restore;
add `--ui` to exercise the overlay throughout. Compare a captured UI frame with
`--no-ui`, and build with `ZEN_BUILD_RUNTIME_UI=OFF` to verify the dependency
boundary. Physical 150%/200% desktop scaling, clipboard/IME, and manual interaction
remain separate acceptance checks from the deterministic packet tests.

`--ui --background-test-seconds=30 --gpu-memory-stats` runs three native window
cycles, covering and unfocusing the demo for 30 seconds, returning to it, minimizing
for 30 seconds, then restoring it. Each return must render eight more frames and
regain focus. Backend-specific key-callback injection has been removed from this
application-level probe; `WindowPlatformTest` covers focus-loss input reset and
relative-mode release, and `UIPlatformTest` covers the private ImGui event path.
The probe logs phase/frame progress every five seconds,
exits on completion, and rejects `--frames`, `--warmup`, or `--smoke-test`
combinations. Run it in both
Debug and Release with `--rhi-thread=0` and `--rhi-thread=1`. The built-in cover is
another engine window in the same process; a separate foreground application remains
an additional check. Avoid interacting with the test windows while it runs.

`RuntimeUIIntegrationTest` exercises automatic live updates, deferred resource
changes during an active edit, automatic budget clamping and manual caps, manual
apply mode, and validation/retry of scene changes against a native render device.
It also checks that switching from the default Cone grid through the UI or server
API applies the automatic profile to both the settings and the voxelizer.
Model selector checks cover filtering, loading feedback, rejected requests,
and resetting drafts after a switch. `GLTFModelCatalog` tests cover recursive variant
discovery, extension matching, Unicode paths, and missing directories.
`SceneModelSwitchTest` (defined with the demo in `ZenSamples`) loads and renders
several models through the demo in inline and threaded RHI modes, checks
orthographic/infinite camera transitions, and verifies that a failed import retains
the current scene. Resource lifetime tests cover scene retirement and partial loads.
The native bindless reset regression also verifies replacement pixels and rejects
heap resets while earlier command recordings remain live.
`VoxelGIRuntimeSettings` planning tests cover automatic floors, device constraints,
20/8/4 GiB memory limits, transactional failure, unknown memory, and overflow.
Existing native runtime GI tests retain strict manual budgets.

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
