# ZenEngine runtime debug UI

The runtime debug overlay and the future editor share `ZenUI`, which contains the
GLFW context/input bridge and the RHI/RDG draw backend. `ZenRuntimeUI` contains
session-only renderer controls. Neither target is a dependency of `ZenCore`.

## Build and run

`ZEN_BUILD_RUNTIME_UI` defaults to `ON`. Configure it `OFF` to build runtime targets
without downloading or linking Dear ImGui. With it enabled, interactive
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
count and properties, light markers, and orbit animation. Model paths and the
environment texture are listed as **restart required**, since changing those assets
requires a scene reload. The application implements `RuntimeSceneControls`; scene
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

Dear ImGui core and the official GLFW backend are fetched together from
[`v1.92.5-docking`](https://github.com/ocornut/imgui/tree/v1.92.5-docking).
Archive SHA-256:
`c893a95aa68e8e7380ca7868adfb2b19c45b9c6e6777a3bdd945c945972c2264`.
The upstream MIT license is retained in the fetched source as `LICENSE.txt`;
distributions must include it. CMake's `FETCHCONTENT_SOURCE_DIR_ZEN_IMGUI` can point
to an offline checkout of this exact revision. Engine configuration lives in
`Include/UI/ImGuiConfig.h`.

This first backend deliberately uses the pinned version's supported **static font
atlas** path and does not advertise `ImGuiBackendFlags_RendererHasTextures`.
Dynamic font texture requests, arbitrary image widgets, and user draw callbacks
are not implemented. The reset-render-state sentinel is supported. Unknown
textures and custom callbacks reject the draw packet with a diagnostic rather
than silently drawing the font atlas in their place. Default font coverage is
Latin; other glyph ranges require atlas configuration before initialization.

The docking source is shared so the later editor can use the same dependency.
Runtime docking and multiple OS viewports are disabled. No ImGui Vulkan backend,
Vulkan native handles, secondary swapchain, or direct queue submission is used.

## Frame and resource ownership

The application polls events, starts/builds/finalizes UI on the window thread,
resolves input capture, applies settings before scene snapshot construction, and
passes the application-owned `RenderOverlay` to `DispatchRenderWorkloads`.
The server appends the overlay after scene passes and before graph submission.

`UIRenderer` copies vertices, padded indices, projection, clipping rectangles, and
draw offsets before returning. Recorded callbacks own their command vectors and
contain no pointers to ImGui frame storage. Geometry buffers grow geometrically
per engine render-frame slot, and uploads use `RenderDevice` staging. Font sampling,
vertex/index reads, and load/store color attachment writes are declared in RDG.
Replaced buffers and the font image retire through the device's existing completion
tracking. Resize reads the current viewport target on every graph build.

The first color policy matches the existing SDR scene composition: encoded UI
colors are blended over the UNORM color backbuffer, and the font atlas supplies
coverage. Linear-light UI blending, HDR, and an sRGB render target need an explicit
future color-policy change and reference-patch tests.

Destroy the runtime UI before the shader manager, device, and GLFW window. The
platform backend restores chained callbacks. The shader manager owns the registered
UI program; RenderDevice owns the sampler cache.

## Verification

`UIDrawPacketTest` checks fractional framebuffer scaling, display offsets, clipped
draws, empty/minimized frames, independent snapshots, padded 16-bit indices, base
vertices over 64K, and rejection of unsupported textures/callbacks or invalid
ranges. `InputControllerTest` checks captured presses and releases, focus reset,
mouse delta reset, and the inclusive last GLFW input codes.

Run the native demo with validation in inline and threaded RHI modes. The existing
`--smoke-test` exercises scene-mode changes and window resize/minimize/restore;
add `--ui` to exercise the overlay throughout. Compare a captured UI frame with
`--no-ui`, and build with `ZEN_BUILD_RUNTIME_UI=OFF` to verify the dependency
boundary. Physical 150%/200% desktop scaling, clipboard/IME, and manual interaction
remain separate acceptance checks from the deterministic packet tests.

`--ui --background-test-seconds=30 --gpu-memory-stats` runs three native window
cycles, covering and unfocusing the demo for 30 seconds, returning to it, minimizing
for 30 seconds, then restoring it. Each return must render eight more frames and
regain focus. The second cycle injects F1 through the installed key callback chain
to cover leaving camera mode. It logs phase/frame progress every five seconds,
exits on completion, and rejects `--frames`, `--warmup`, or `--smoke-test`
combinations. Run it in both
Debug and Release with `--rhi-thread=0` and `--rhi-thread=1`. The built-in cover is
another GLFW window in the same process; a separate foreground application remains
an additional check. Avoid interacting with the test windows while it runs.

`RuntimeUIIntegrationTest` exercises automatic live updates, deferred resource
changes during an active edit, automatic budget clamping and manual caps, manual
apply mode, and validation/retry of scene changes against a native render device.
It also checks that switching from the default Cone grid through the UI or server
API applies the automatic profile to both the settings and the voxelizer.
`VoxelGIRuntimeSettings` planning tests cover automatic floors, device constraints,
20/8/4 GiB memory limits, transactional failure, unknown memory, and overflow.
Existing native runtime GI tests retain strict manual budgets.

## Upgrade checklist

Update the archive tag and hash together. Re-read the pinned font/texture contract
and GLFW callback chaining. Re-run packet/input tests, native validation in both RHI
modes, resize/restore, and UI-disabled build checks. Verify packed vertex layout,
index width, blend policy, and font coverage. An upgrade that requires dynamic
textures must implement and test create/update/destroy retirement before enabling
the corresponding backend flag.
