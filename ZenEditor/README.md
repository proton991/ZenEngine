# ZenEditor scene viewer

Steps 1–4 of the [implementation plan](../Doc/ZenEditorImplementationPlan.md) provide
a separate, read-only editor application. Editing, saving scene documents, undo,
gizmos and Play remain disabled until later phases.

![Running maximized ZenEditor](../Doc/imgs/zeneditor-sdl3.png)

## Build and launch

From a Visual Studio developer terminal on Windows:

```powershell
cmake --preset x64-windows-msvc-debug -DZEN_BUILD_EDITOR=ON
cmake --build build/x64-windows-msvc-debug --target zen_editor
.\build\x64-windows-msvc-debug\bin\zen_editor.exe
```

The editor starts maximized in normal desktop window mode. It uses the actual
framebuffer dimensions for presentation and resizes the offscreen scene to the
available viewport pixels. Use `--windowed` to start at 1440×900 instead.

On Windows, the menu and window title share one themed row, with minimize,
maximize/restore, and close controls at the right. Drag the space after the menus
to move the window; double-click it to maximize/restore. Window edges retain
native resizing, and maximizing fills the monitor work area without covering the
taskbar. Control requests are applied between frames before framebuffer resizing.
The same title/menu row is enabled by the SDL3 path on macOS; native Cocoa
behavior and Retina/MoltenVK presentation still require validation on a Mac.
Linux support is deferred.

Use `--scene=absolute/path/model.gltf` (or `.glb`) to open an initial asset.
Without it the shell starts empty. An import failure keeps the current scene and
reports its error in Assets and Output, including failures of the initial asset.
File → Open (Ctrl+O or the toolbar Open button) shows the platform file picker,
filtered to glTF/GLB, without blocking rendering. It starts beside the most recent
scene, otherwise in the configured `model_base_path`. The GLFW fallback backend has
no picker and asks for a path instead. File → Open Recent lists the last ten
scenes that opened successfully; missing files are disabled, and Clear Recent
empties the list.

`ZEN_BUILD_EDITOR` defaults to `OFF`; `ZEN_BUILD_RUNTIME_UI` defaults to `ON`.
All four combinations are supported. Both frontends use the same pinned
`v1.92.9b-docking` ImGui dependency. With both options off, core and samples have
no ImGui dependency. Only editor initialization enables docking.

The implementation uses SDL3 behind `platform::NativeWindow` and the engine's
RHI/RDG stack. The pinned SDL library is built statically; no SDL DLL deployment
is required. Use `-DZEN_WINDOW_BACKEND=GLFW` for the migration fallback.
See [migration status](../Doc/SDL3Migration.md) for validation and remaining macOS
acceptance work.
Shader, texture and engine configuration paths use the existing engine path
configuration and do not depend on the launch working directory.

## Workspace and controls

- Drag panel tabs to dock; use View to hide/reopen panels or Reset Layout.
- Select hierarchy nodes or click scene geometry to synchronize the Inspector.
  Hierarchy search retains ancestors and includes nodes without meshes.
- Hold right mouse over the focused Scene view to look and fly with WASD/QE;
  Shift increases speed. Alt + left mouse orbits; middle mouse pans; wheel dollies.
  Scene-image drags retain navigation ownership when the panel is floating.
- F frames the selection; Home or Frame All frames the scene. Orthographic toggles
  projection. The camera is independent of cameras authored in the glTF.
- Menus, toolbar buttons and shortcuts run the same registered actions, so each
  command has one label, shortcut and enabled state. Shortcuts work anywhere in the
  workspace except while typing, while a widget is active or while a menu or modal
  is open. Disabled commands explain which plan step enables them.
- Escape cancels navigation or a modal. Text input, active widgets, open popups,
  and loss of window/panel focus prevent viewport navigation.
- The Scene toolbar selects PBR, Voxel GI or voxel visualization. Render Settings
  includes environment controls, configuration and fallback diagnostics. Inspector
  values are read-only.
- Output retains at most 2,048 entries of 4,096 characters each, with level/text
  filtering, Clear and Copy. GPU memory and frame-time readings are snapshots.

The frontend theme follows the proposed appearance: charcoal-blue surfaces,
restrained blue selection, proportional Roboto text, toolbar icons, read-only XYZ
fields, and an Inspector dock spanning the full workspace height. Save, transform
and playback controls remain disabled in this viewer.

### Environment and skybox

Open **Render Settings → Environment** to select a skybox and image-based lighting
texture together. The repository includes the papermill cubemap. An optional
[starter set](../Data/Textures/Environments/README.md) of two outdoor skies and five
indoor HDRs (studio, rooms, corridor and workshop) from Poly Haven (CC0) is not
stored in the repository; `python tools/fetch_environments.py` downloads it.
**Browse** accepts a 2:1 Radiance `.hdr` panorama up to 8K or a floating-point
RGBA16F/32F `.ktx`/`.dds` cubemap. A typed path is available when the window
backend has no native picker. **Refresh list** discovers additional
files under `Data/Textures`; browsing can load files elsewhere.

Intensity, rotation, environment lighting and skybox visibility apply immediately.
The override and controls persist across scene switches for this editor session;
they do not edit or save the glTF. **Scene / engine default** restores the selected
scene's authored image-based light when present, otherwise `environment_texture`
from engine configuration. A missing/unsupported file preserves the active texture
and reports an error. Open a scene with renderable geometry to enable these controls.

Texture changes run between frames. HDR decoding/conversion runs on the CPU; upload,
irradiance generation, reflection prefiltering and drawing use the existing RDG/RHI
pipeline. Switching can briefly pause the editor while decoding and preparing GPU
resources. Retired environments cancel their queued preprocessing, and replacement
invalidates GI lighting history without rebuilding geometry or clearing selection.

For startup or capture runs, combine `--scene=...` with
`--environment=Environments/studio_small_09_1k.hdr` (after fetching the starter
set). Relative environment paths resolve under `Data/Textures`; absolute UTF-8
paths are also accepted.

Opening a scene shows a modal progress bar for reading data, preparing graphics
resources and publishing the scene. CPU import runs on an engine worker so the
window keeps repainting; GPU preparation and publication run on the engine thread,
after their stage has been displayed. The bar animates during import and then shows
completed stages, without claiming a byte count or time estimate. GPU work can
temporarily pause the animation. Failed loads keep the previous scene and show a
dismissible error. Startup scenes, File/Open and recent files use this same flow.

Assets lists the open scene's meshes, materials, textures and animations, with
category filters, search and grid/list views. Selecting one shows it in the
Inspector, including links to the materials, meshes and nodes that reference it.
The Inspector's permanent Selection tab follows selection in Hierarchy, Scene and
Assets. A reference link from Selection opens a closable tab without changing the
scene selection. Further links navigate within that tab; Ctrl/Cmd-click opens or
focuses another tab. Each reference tab has Back/Forward buttons (Alt+Left/Right
while the Inspector is focused), including a route back to its originating page.
Switching pages retains their scroll and expanded sections. Selecting another
object returns to Selection while keeping reference tabs; loading another scene
clears those tabs. Tabs and history last for the current editor session.

Mesh assets, and the Mesh section of a selected node, show a material-free preview
like RenderDoc's mesh view. The mesh is drawn in its own vertex space, so node
transforms are not applied, from the open scene's GPU vertex and index buffers
into a separate image. Shading is Flat (face normals), Smooth (vertex normals) or
Normals (normals as colors); Wireframe overlays edges where the GPU supports line
fill. Drag to orbit, middle-drag to pan and use the wheel to zoom; **Frame** refits
the mesh bounds. The preview camera is independent of the Scene camera and refits
when another mesh is shown; shading options persist across selections. Only
triangle primitives are drawn, and the preview renders only in frames in which the
Inspector shows it.

Inspector Back/Forward buttons and shortcuts invoke the same registered actions.
Their shortcuts use an Inspector scope, so the workspace's global dispatcher cannot
trigger them from another panel. The frontend synchronizes navigation before drawing
its controls; `GetInspector()` only returns state, and successful scene replacement
resets navigation within the controller's load transaction.

Uncompressed RGBA8 textures show previews downsampled on the CPU to at most 128
pixels in their stored encoding,
so sRGB data is not decoded twice. Materials show that preview of their base color
texture tinted by the base color factor, or a swatch. Compressed and floating-point
textures show an icon. The loader's placeholder textures, its linear copies of
color images (folded into the original) and an unused default material are not
listed. Projects and a file browser for building scenes from several assets
belong to later plan steps.

The font is `Roboto-Medium.ttf` from the pinned ImGui dependency's `misc/fonts`
directory (Apache 2.0, attribution in upstream `docs/FONTS.md`). CMake supplies its
absolute location, including when using an offline ImGui checkout. The adapter's
default font remains the fallback. Editor styling does not change the runtime UI.

The ImGui layout lives in `imgui-layout-v2.ini`. Toolkit-independent preferences
(panel visibility keyed by stable panel ID, and recent files) live in
`preferences-v3.txt`, which the model layer reads and replaces atomically. The
directory is `%LOCALAPPDATA%/ZenEngine/ZenEditor` on Windows, or
`$XDG_CONFIG_HOME/ZenEngine/ZenEditor` (`~/.config/ZenEngine/ZenEditor` fallback).
Missing or invalid layouts are rebuilt. Without `preferences-v3.txt`, visibility and
recent files migrate from `preferences-v2.txt`, or visibility alone from
`preferences-v1.txt`; earlier files are left in place. Panels unknown to a saved file
use their default visibility, so adding or reordering panels is safe. Reset Layout
restores default visibility and keeps the recent files.
`--settings-dir=path` provides an isolated directory for testing.

Startup monitor scale controls the static font atlas and spacing; `--scale=1`,
`--scale=1.5`, or `--scale=2` provides a diagnostic override. Dock widths remain
adjustable; narrower panels wrap Inspector values. CJK/IME and rebuilding the
atlas while moving between monitors are follow-up work, as specified in the plan.

## Boundaries for a future UI replacement

Each library has its own folder and include root, so a layer cannot include the
headers of a layer it does not link:

| Folder / target | Responsibility | Direct dependencies |
| --- | --- | --- |
| `Model/` `ZenEditorModel` | Scene parsing and the read-only `EditorScene` (node lookup, child lists, hierarchy filter, inspection, bounds, CPU picking), `SceneAssetIndex` and CPU previews, `EditorSelection` and pick stamps, `InspectorNavigation` tabs and history, `EditorCamera`, the `EditorActions` registry, `EditorPreferences` and recent files, environment discovery, mesh preview settings, logs | `ZenCore` |
| `Rendering/` `ZenEditorRender` | `EditorViewport`: GPU scene publication as a prepare/commit/discard transaction, offscreen images, preview textures, selection bounds and GPU pick results. `MeshPreviewRenderer`: the Inspector's material-free mesh image. Reads the model, never changes it | `ZenEditorModel` |
| `Services/` `ZenEditorServices` | `EditorController`: owns the editor state, runs transactions across model and GPU (loading, applying picks, framing) and registers the core actions | `ZenEditorRender` |
| `Platform/` `ZenEditorPlatform` | Native title-bar hit testing and window actions; no ImGui types | `ZenCore` |
| `ImGui/` `ZenEditorUI` | Workspace, descriptor-based panels (one file each under `Source/Panels`), theme palette, shortcut translation, toolkit layout file | Services, platform, `ZenImGui` |
| `App/` `zen_editor` | Application lifecycle, platform file picker, preference loading/saving, frame submission and diagnostics | `ZenEditorUI` |

A replacement frontend keeps the model, render and service libraries and their
tests; it draws `EditorController` state, dispatches its actions and replaces panels,
toolkit layout and input translation. It may supply neutral packets to `ZenUI` or use
its own renderer. Only `ZenEditorUI` and `zen_editor` link ImGui.

Adding a panel means one file under `ImGui/Source/Panels`, one factory call in
`CreateEditorPanels` and a descriptor with a stable ID and default dock area; the
default layout, View menu and saved visibility follow from the descriptor. Adding a
command means registering an action with the owner of the state it changes; menus,
toolbar and shortcuts then share it.

Applications explicitly call `RenderDevice::InitializeRendererServer()` once,
after device initialization and shader registration, before initializing
`EditorController`. This setup is the same with or without a native window.
`RenderDevice::Init()` initializes only device/graph infrastructure;
`GetRendererServer()` is a read-only accessor that returns null before the explicit
initialization call. `RenderDevice::Destroy()` still owns server teardown.

The offscreen `RenderView` contains borrowed color/depth images and physical extent.
`RendererServer::DispatchRenderWorkloads` requires this view explicitly and passes
it to scene graph builders. The runtime constructs a fresh
`RenderView::FromViewport(*nativeViewport)` each frame; the editor supplies
`EditorViewport::GetRenderView()`. Scene renderers retain neither a native viewport
nor an optional target override. The server's native viewport is used separately
for UI composition and presentation. `GetColorTarget()` and `GetDepthTarget()`
refer to either kind of target; there is no runtime/editor selection in the renderers.

The Scene panel quantizes target dimensions to eight pixels and letterboxes to the
actual aspect; navigation/picks use that image rectangle. Hidden/zero-size panels
skip scene rendering. The graph retains replaced images until queued work finishes;
ordinary splitter resize and GPU picking never introduce an idle wait. Model
replacement and native swapchain recreation use the existing synchronization path.

Color is single-sample SDR, stored encoded in an RGBA8 UNORM scene image and sampled
without another gamma conversion. UI uses the existing straight-alpha SDR policy.
HDR, linear-light UI blending and MSAA resolution are not provided by this viewer.

## Picking contract

CPU bounds picking gives immediate feedback. An R32_UINT object-ID pass followed by
a one-pixel compute extraction and asynchronous buffer readback refines selection.
Results carry scene, camera, target and selection revisions; stale results cannot
overwrite a newer action. No ImGui object or synchronous GPU wait is needed to apply
a result. IDs map to scene-generation + node-index identities, never display names.

The ID pass reuses scene geometry/transforms and material alpha-mask/backface rules.
It includes visible opaque and alpha-masked triangles. Blended or transmissive
surfaces, points and lines are excluded; a solid surface behind them may be selected.
Unselectable solid geometry occludes with ID zero. Background clicks clear selection.
Skinned/morphed geometry uses the loader's frozen initial pose. Instances use the
loader's expanded scene nodes; primitive-level, bone and per-vertex selection are
not implemented. Bounds are an x-ray line overlay. These are viewer policies, not a
claim of animated or transparency-aware precise selection.

## Verification and diagnostics

```powershell
cmake --build build/x64-windows-msvc-debug --target EditorModelTest EditorRenderingTest UIDrawPacketTest UIRenderingTest RuntimeUIIntegrationTest SceneModelSwitchTest
ctest --test-dir build/x64-windows-msvc-debug -R "EditorModel|EditorRendering|UIDrawPacket|UIRendering|RuntimeUIIntegration|SceneModelSwitch" --output-on-failure
```

Model tests initialize no window, GPU or ImGui. Rendering tests initialize no window
or ImGui and exercise both RHI modes, including a CPU bounding-box hit that GPU
surface picking correctly clears, stale results, resize and scene replacement.
Neutral UI tests render a synthetic triangle and retire registered resources after
graph declaration. Existing runtime UI and model-switch regression tests remain.

`--smoke-test` runs 40 frames with resize, minimize/restore, hide/show, picks,
camera/projection changes, layout reset and asset selection. Combine it with `--rhi=inline` or
`--rhi=threaded`, `--mode=pbr|gi|voxels`, and `--hidden` for native validation.
`--frames=N --capture=path.ppm` captures the last frame. Diagnostic capture waits
for completion; normal rendering does not use this capture path.

`EditorWindowChromeTest` verifies Windows client/drag/resize regions, decoration
restoration, maximize work-area bounds, double-click restore, and queued window
controls. The adapter follows the native
[custom-frame message protocol](https://learn.microsoft.com/en-us/windows/win32/dwm/customframe).
Windows 11's maximize-button hover Snap Layout menu is not implemented; the custom
button remains an ordinary UI control. Cross-monitor DPI transitions and manual
drag-to-snap behavior remain platform acceptance checks.

Validated on Windows with an AMD Radeon RX 7900 XT: Debug and Release builds,
the four build-option combinations, seven selected CTest suites, 29 swapchain
regression tests, native fixture/Sponza rendering, and synthetic 100%/150%/200% UI
scales. Restart/corrupt-layout recovery preserves neutral preferences; launching
from another directory and failed initial import recovery also pass. Actual per-monitor DPI changes, manual docking/navigation,
clipboard/IME, and other operating systems remain acceptance checks.
