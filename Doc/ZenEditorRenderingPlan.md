# ZenEditor rendering plan

Revised direction, 4 October 2026. This is the active plan for work after the
implemented ZenEditor viewer. It supersedes the delivery order of steps 5 onward
in [the original implementation plan](ZenEditorImplementationPlan.md).

Develop ZenEditor into a workspace for configuring and evaluating rendering.
Use the existing glTF loading and rendering support. Put rendering, lighting,
GI, debug output, and run settings in one **Rendering** panel. Press **Run** to
launch `zen_player`, a separate runtime program that renders the selected scene
with the complete configuration. `zen_player` is the start of ZenEngine's
runnable game and interactive scene viewer.

Scene and asset authoring are deferred: scene documents, hierarchy mutation,
transform gizmos, material editing, asset composition, reimport, undo history,
animation authoring, and gameplay preview are not prerequisites for this work.
Existing read-only inspection and navigation remain useful for examining results.
Light configuration is part of the rendering setup and does not require editing
the imported scene graph.

## Intended workflow

1. Open a `.gltf` or `.glb` through the existing File Open or Open Recent actions.
   The selected scene means the entire successfully loaded file, not a selected
   hierarchy node or mesh.
2. Use the Rendering panel to choose a rendering algorithm, set up lights (the
   file's glTF lights, manual and preset lights, and the camera light), configure
   environment, shadows and GI, and choose a final or diagnostic output.
3. Check the result in the embedded viewport. Frame and navigate the camera using
   the existing controls.
4. Press Run. Validate the complete configuration, save it with the camera as a
   setup file, and launch `zen_player` with that file. Keep the editor available.
5. Close the player window or press Stop to return to the embedded preview. Adjust
   settings and Run again, or use Restart to replace an active run.
6. Save and load rendering presets to reproduce a setup without introducing a
   project format or scene authoring system. The player opens a saved preset
   directly as well.

First-release behavior: the player renders the setup file it was launched with
and has no connection to the editor. Changes made after launch are marked
**Pending restart**; Restart saves a fresh setup and relaunches. A connection for
live settings, camera follow and remote debugging can be added later. Camera
movement inside the player remains local to that run.

## Current implementation and remaining work

The state after milestones 1–3; the last column lists what later milestones add.

| Area | Current implementation | Remaining work |
| --- | --- | --- |
| Scene loading | `ParseScene` in the editor model records the normalization center and scale; `RenderScene` normalizes the scene to unit extent; transactional scene replacement, read-only inspection and navigation | Move render-scene preparation into ZenCore so `zen_player` loads and normalizes identically (milestone 4) |
| Configuration | `rc::RenderingSettings` values and `ValidateRenderingSettings` sit below the editor UI; `EditorRenderingState` tracks the draft and the applied revision | Setup-file serialization and the running player's setup (milestone 4); presets (milestone 5) |
| Settings UI | One **Rendering** panel, still under the `RenderSettings` ID, with Apply, Revert and per-section resets; the Scene viewport keeps navigation, framing and orientation | Run settings (milestone 4); preset load and save (milestone 5) |
| Algorithms | PBR or PBR with voxel cone GI, selected separately from the debug output; `RendererServer` reports the effective algorithm and a latched fallback reason; `--mode=voxels` maps to the voxel debug output | None planned |
| Configuration source | The editor seeds its defaults from `engine.cfg` once. `RendererServer`, `VoxelGIRenderer`, both voxelizers, `VoxelizerBase` and `DeferredLightingRenderer` still read `engine.cfg` while initializing | Accept explicit initial settings so the player applies its setup file before the first allocation (milestone 4) |
| GI and shadows | Editable GI and shadow settings. Grid size, voxelizer, reflectance resources and shadow resolution wait for Apply while other edits keep previewing. A shadow memory preflight rejects edits that do not fit. Mesh shadows apply on the voxel GI path only | Shadow parity for PBR (candidate) |
| Lights | Only glTF lights load, with no automatic fallback. Manual lights and `BuildBoundsLightPreset` side and corner presets; `RenderScene::ReplaceLights` keeps IDs and revisions increasing; the camera light uses its own uniform field | Store lights with normalization records in setup files and presets (milestones 4–5) |
| Debug outputs | Final, depth, albedo, world-space normal, shadow face, 3D voxels and voxel albedo slice, each built by the renderer that owns the data, with an explicit reason when unavailable | Roughness, metallic, occlusion, emissive, GI radiance and voxel mips above zero |
| Runtime UI | `RuntimeDebugUI` combines frame, memory and render-graph statistics with GI, scene, model and config controls | Factor the statistics into a profiling overlay for `zen_player` (milestone 4) |
| Run | Toolbar Run and Stop are disabled placeholders that point to milestone 4. `scene_renderer_demo` is the only standalone renderer and is configured by `engine.cfg` | Add `zen_player` and the Run/Restart/Stop lifecycle (milestone 4) |

Relevant implementation references are [RenderSettingsPanel](../ZenEditor/ImGui/Source/Panels/RenderSettingsPanel.cpp),
[RendererServer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/RendererServer.h),
[GI settings](../ZenCore/Include/Graphics/RenderCore/V2/VoxelGISettings.h),
[SceneLights](../ZenCore/Include/Graphics/RenderCore/V2/SceneLighting.h),
[SceneShadowRenderer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h),
[DeferredLightingRenderer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h),
[EditorViewport](../ZenEditor/Rendering/Include/Editor/Rendering/EditorViewport.h), and
[RuntimeDebugUI](../ZenUI/Runtime/Include/RuntimeUI/RuntimeDebugUI.h).
The existing runtime debug UI is a behavior reference; ZenEditor must not depend
on its widgets or the sample application.

## One Rendering panel

Retain the existing `RenderSettings` panel ID for saved layouts and rename its
visible title to **Rendering**. Panel windows are named `title###id`, so the
rename keeps saved docking. Use collapsible sections within that one panel.
Keep the panel easy to reach in the default right dock. Remove the rendering mode
selector from the Scene viewport once its replacement is available. The viewport
keeps navigation, framing, selection and view orientation controls.

| Section | Controls |
| --- | --- |
| Scene and preset | Current scene, open/recent actions, preset name, load/save/reset and modified state |
| Algorithm | PBR baseline or PBR with voxel cone tracing GI; requested algorithm, actual algorithm and fallback reason |
| Lighting | Light list showing each light's origin (glTF, manual or preset); add point, directional or spot lights; add the AABB side or corner preset; remove, clear and reset to the glTF lights; enabled, color, intensity, position, direction, range, spot angles and casts shadows; camera light enabled, color, intensity and range; light markers |
| Environment | Existing HDR/cubemap selection, intensity, rotation, environment lighting and skybox visibility |
| Shadows | Enabled state, map resolution, estimated shadow memory and light selection for inspection; show applicability to the selected algorithm |
| GI | Grid resolution, cone count and angle, indirect intensity, step scale, normal bias, distance and step limits; analytic, environment and emissive contributions |
| GI resources | Voxelizer selection, reflectance policy and memory budget, async compute preference, effective backend and resource status |
| Debug output | Final image, depth, albedo, normals, shadow map and voxel views; appropriate channel, range, mip, slice and light/face selectors |
| Run | Player output size, follow-window or fixed render resolution, camera projection, present mode, RHI mode and profiling overlay visibility; Run, Restart and Stop; player state and last exit reason |
| Status | Validation errors, pending changes and pending restart, requested and applied revisions, actual output and fallback reason, GPU memory and available frame timings |

The main toolbar may mirror Run and Stop as actions, but the Rendering panel owns
their configuration. Light markers and other rendering overlays belong here too.
Do not duplicate editable light settings in the Inspector. A selected imported
light may link to this panel while its source properties remain read-only.

Use engine validation and limits rather than independent UI-only ranges. Explain
which controls affect direct light, indirect light, environment lighting or the
skybox. In particular, GI contribution toggles must not be presented as switches
for all direct lighting or specular environment lighting. Explain that the light
ball's bounce lighting uses the analytic GI contribution.

## Configuration and application rules

Introduce a toolkit-independent rendering configuration owned by
`EditorController`, with scene reference, light list, camera light, environment,
algorithm, GI/shadow settings, debug selection and run settings. Define it as
serializable values from the start, because the setup file written by Run is
that configuration. Track the editable draft, last applied preview revision, and
the setup of the running player separately. Widgets dispatch actions; they do not
directly mutate GPU resources or the active `RenderScene`.

Reuse `VoxelGIRuntimeSettings`, `SceneLight` and their validators. Define shared
serializable values and runtime application code below the editor UI so the
editor and `zen_player` use the same interpretation. Do not create a second
renderer or duplicate the demo's startup configuration logic.

| Change | Application behavior |
| --- | --- |
| Light values that keep the shadow face count, environment scalar values, cone parameters, camera light, algorithm and debug selection | Validate and stage at a frame boundary; interactive preview may coalesce changes |
| Adding, removing or clearing lights, applying a preset, and changes to enabled, type, casts shadows or a zero/non-zero intensity | Stage as one structural change. When shadows are in use, preflight the resized shadow array and reject the whole edit if it does not fit. Coalesce drags so they do not reallocate every frame |
| Environment texture | Load as a candidate; publish only on success and retain the previous environment on failure |
| Voxel resolution, voxelizer, reflectance resources and shadow resolution | Require Apply after editing. Until then, other edits keep previewing with these values held at their applied state. Preflight resource limits and perform at most one rebuild per commit |
| Player device or presentation options | Apply at the next Run or Restart; record them explicitly in the setup file |

Shadow memory is resolution² × 4 bytes × max(2, shadow faces): one face per
directional or spot light and six per point light, counting enabled shadow
casters with non-zero intensity. Thirty-two shadow-casting point lights at 2048
need about 3 GiB; the enabled light ball adds six more faces.
`ValidateVoxelGIResources` checks only reflectance memory, so
`ValidateShadowResources` performs the shadow preflight beside it.

Run and Restart validate the complete draft, commit preview-compatible changes,
and write every value into the setup file. They must not silently launch stale
preview settings when an edit is invalid or a resource load is pending. Reset
supports individual sections and the whole setup.

The editor reads `engine.cfg` once to seed defaults; explicit configuration values
take precedence after that. `zen_player` takes its rendering configuration only
from the setup file. Extend `RendererServer::Init`, and the renderers it creates,
to accept initial `VoxelGIRuntimeSettings`, light marker settings and the default
environment. Otherwise the player allocates GI and shadow resources from
`engine.cfg` first and then rebuilds them, or fails on an `engine.cfg` setting
that the setup file was meant to replace.

The existing GI apply path may wait for outstanding work, destroy resources and
allocate replacements lazily. Account for that behavior explicitly: validation
failure leaves the applied configuration intact, while a later allocation failure
must be reported as an actual fallback or failed application. Do not claim atomic
GPU rollback from the current API. Keep the last successful configuration for
recovery and never retain renderer pointers across a rebuild.

`RendererServer` reports a structured status with the effective algorithm and a
reason, such as incomplete voxel coverage, voxelizer or GI initialization failure,
or shadow allocation failure. A failure stays latched until a change that could
resolve it: GI resources or shadow resolution, the algorithm, the debug output,
the number of shadow faces, the scene, or an explicit Apply or Retry. Only an
explicit Apply or Retry rebuilds a voxelizer whose allocation failed, so dragging
a control never repeats a failed allocation every frame. The editor shows this
status in the Rendering panel and the player in its profiling overlay.

### Light setup

Load lights only from the glTF file. A file with `KHR_lights_punctual` lights
starts with exactly those lights, including hidden and zero-intensity ones; a file
without lights starts with none. Remove the automatic AABB-side fallback from
`BuildSceneLights` and `RenderScene` in the engine, not only in the editor. A
scene without lights is lit only by the environment and, when enabled, the camera
light. The Lighting section shows **No lights** so a dark result is explained.
The editor does not read the `engine.cfg` light slots, which remain a demo
configuration.

Users add further lights in two ways:

- **Manual:** add a point, directional or spot light. New point and spot lights
  start at the editor camera position, aimed along the view direction; a new
  directional light takes the view direction. Default intensity and range derive
  from the normalized scene size.
- **Preset:** add the AABB side preset (six point lights just outside the face
  centers) or the AABB corner preset (eight point lights just outside the
  corners, moved outward along each diagonal). Each light aims at the bounds
  center. Use the current fallback's margin (15% of the largest extent, at least
  0.01) and its intensity, three times the squared distance to the center. A
  preset that would exceed 32 lights is rejected as a whole.

Preset lights become explicit positions when added. Later edits and saving keep
those positions rather than recomputing them from the bounds. Move the preset
generator into ZenCore `SceneLighting` so the demo, the editor and `zen_player`
share it. The demo applies the side preset explicitly where it relied on the
automatic fallback, so its output and `boundsPresetLights` behavior are
unchanged.

The rendering setup owns one light list. Each entry records its origin (glTF,
manual, side preset or corner preset) and a configuration ID mapped to a runtime
`LightId`. Imported glTF lights are editable copies; **Reset lights** restores the
file's lights and removes added ones. A list with zero lights is valid.

Apply the list through `RenderScene::ReplaceLights`, which replaces the lights while
keeping `LightId` allocation and light revisions increasing. Do not assign a fresh
`SceneLights` to a live scene, as the demo previously did. IDs would restart at 1 and
collide with the imported-light mapping that animation updates. Revisions could
also repeat a value voxel GI has already recorded, which skips relighting (six
fallback lights replaced by six custom lights both reach revision 7). The same
method detaches imported-light animation updates once the list has been edited.
Animation stays frozen in both the editor and the player in this release.

Positions and directions use the engine's normalized rendering world, which
scales by one over the largest extent about the bounds center. The preview and
the player apply identical normalization because both build a `RenderScene`.
Any change to the source geometry changes that transform, so positions stored in
normalized units would silently move. The `engine.cfg` light positions already
broke this way after an earlier scale change. Setup files and presets therefore
also store the normalization center and scale, so lights can be mapped into a
changed scene or the mismatch reported.

Opening another scene resets the light list to that scene's glTF lights and
resets the camera. Algorithm, GI, environment, camera light and output settings
carry over; the environment already carries across scenes in the current editor.
An explicitly loaded preset restores its own scene and light list together.
Persistent source node identities and per-material overrides are outside this
scope.

### Camera light ball

Revised after milestones 1–3: the camera light is now a small, visible colored ball
for testing local illumination and GI while moving through the scene. It follows
at an adjustable distance ahead of the rendering camera. Turning off **Follow
camera** holds its current world position, so the user can walk around it. The
held position is also editable. Configure enabled, color, intensity, finite range,
sphere radius and follow distance in Rendering > Lighting. Defaults in normalized
scene units are range 0.12, radius 0.005, follow distance 0.08 and intensity 0.0025.

The source is an omnidirectional point light with the existing inverse-square
falloff and smooth finite-range cutoff. The rendered sphere indicates its position
and color; it is not a second emissive source. Both deferred and forward materials
receive its direct light. The analytic GI contribution injects its light into voxel
radiance, and the mesh-shadow path gives it six point-light faces. PBR retains its
existing mesh-shadow limitation.

Keep the dedicated scene-uniform field and reserve one extra shadow slot outside
the 32 scene-light slots. Position, range, color and intensity changes advance a
separate lighting revision so GI radiance and its mips refresh; unchanged geometry
and sky irradiance stay cached. A held ball does not relight when only the camera
moves. Disabling it removes its contribution on the next frame.

Store the follow/hold state, held position and all ball settings in future setup
files and presets. Use Sponza with other lighting disabled to compare direct-only
and indirect lighting while carrying and holding the ball.

## Algorithms and debug outputs

Start with the implemented PBR path and PBR with voxel cone tracing GI. Voxel
visualization becomes a debug output rather than a competing lighting algorithm.
Keep compatibility with existing command-line modes at the adapter boundary.
Future algorithms can register capabilities when implemented; hardware ray query
lighting and path tracing are not promised by this editor plan.

Add an engine-owned debug output description containing a stable output ID,
availability, extent, format, interpretation and valid subresources. Build a
visualization pass into the same RDG frame and return its display image to the
editor viewport, and later to the player window. The renderer that owns a
producer builds its views: G-buffer attachments are per-frame graph resources
looked up by name, so `DeferredLightingRenderer` builds their visualization. Keep
transient graph resources inside that frame and retain displayed images through
GPU completion. Panels must not cache raw G-buffer or shadow pointers or depend
on internal attachment names.

| Output | Initial visualization and controls |
| --- | --- |
| Final | Normal composed image for the chosen algorithm |
| Depth | Available on both paths, since deferred lighting and forward materials both write the view depth target. Raw device depth or linearized depth with adjustable range and correct perspective/orthographic handling |
| Albedo | Base color with an explicit display conversion; no lighting baked into the view |
| Normal | Decoded normals mapped to RGB; label the coordinate space |
| Shadow | Select a shadow-casting light by `LightId`; the engine resolves its layers each frame, because layers follow the order of enabled lights. Point lights expose six faces; provide depth range controls |
| Voxel | Existing 3D occupancy/color view plus axis/slice and mip selection for available voxel resources |
| Additional channels | Roughness, metallic, occlusion, emissive and GI radiance after the primary views; expose only data the renderer actually produces |

Make output availability explicit. `UsesForwardMaterials()` selects the forward
path for the whole scene when any material is blended or transmissive or any
primitive is not a triangle list. Such scenes have no G-buffer, so albedo, normal
and the other G-buffer views are unavailable for the entire scene. Before fixing
the debug milestone's acceptance, count how many test fixtures take the forward
path; if most do, schedule the diagnostic material pass sooner instead of
shipping views that rarely work. Initially show an unavailable reason where the
requested producer does not exist. Never display a previous frame's attachment
or silently substitute final color under a debug label. The diagnostic material
pass for forward-only scenes is not a reason to rework glTF support now.

Request only the additional passes needed by the selected debug output. For
example, voxel inspection can request voxelization while retaining PBR as the
selected lighting algorithm. Do not run every diagnostic producer every frame.
Apply debug ranges and channel mapping on the GPU; continuous inspection must
not require blocking CPU readbacks. Hide selection/light overlays in debug output
by default so they do not obscure the framebuffer being inspected.

The current PBR branch does not use the GI branch's mesh-shadow path, so casts
shadows and the Shadows section have no effect under PBR. Label that limitation
rather than showing a misleading enabled checkbox. `BuildCompositionGraph`
already accepts the shadow renderer, so shadow parity for PBR is mostly a
deferred and forward shader variant; it is a candidate for the lighting
milestone.

## zen_player

### A separate program

`zen_player` is a separate executable, not a mode of `zen_editor`. Its final goal
is a runnable game and interactive scene viewer, which must ship without the
editor: the ImGui workspace, editor services, editor shaders, preferences and
window chrome. Godot runs games from its editor binary during development, but it
ships games on runtime-only export templates built without editor code. Building
one program twice would add that same split as a second build configuration.
With two programs the linker enforces the boundary, and the player that Run
launches is the player that ships. The cost is small: the build places
`zen_player` beside `zen_editor` and builds it whenever the editor is built, and
the setup file carries a format version.

### What the player is

Create `zen_player` in a top-level `ZenPlayer/` directory. It links ZenCore and,
when `ZEN_BUILD_RUNTIME_UI` is enabled, the profiling overlay; it never links a
ZenEditor library. Without the overlay it builds without ImGui.

Start from `scene_renderer_demo`'s application shell: native window, device and
`RendererServer`, frame loop, resize, present mode, vsync and RHI-thread options,
profiling and frame capture. Move what both programs need into shared code
rather than copying it. The demo stays as a sample.

Share these pieces through ZenCore so the editor preview and the player cannot
diverge:

- the setup file types, reader and writer;
- `ParseScene` and render-scene preparation, meaning the `SceneData` setup in
  `EditorViewport::PrepareScene`, the default environment and light setup;
- the camera controller, moving `EditorCamera` into ZenCore so the player
  navigates exactly like the editor viewport.

Run it as `zen_player <setup file>`, optionally with `--frames=N`,
`--capture=path`, `--profile=prefix` and `--rhi=inline|threaded`. The same
command runs a saved preset without the editor, for profiling, automated
captures and parity tests.

### Launch and lifecycle

There is no connection between the editor and the player in this release. The
editor knows only whether the process is running and how it exited.

1. Run validates the draft and writes `setup.zenrender` into a new run folder
   under the editor settings directory, with every path resolved.
2. Launch `zen_player` through a Windows/macOS process wrapper. Pass arguments as
   an argument vector and locate the executable beside `zen_editor`,
   independently of the current working directory.
3. The player validates the setup file, loads the scene and resources, applies
   settings in dependency order and renders. It writes its log into the run
   folder. On failure it exits with a documented code for an invalid setup file,
   a scene or asset load failure, or a device or renderer failure. When launched
   by the editor it shows no blocking dialogs.
4. The editor checks the process each frame: Idle, Running, Stopping, then Idle or
   Failed. Failed shows the exit reason and the log path in the Rendering panel
   and the Output log, preserves the editor setup and permits another attempt.
5. Stop sends a graceful close request: `WM_CLOSE` to the player's window on
   Windows and `SIGTERM` on macOS, which the player treats as closing its window.
   The editor force-terminates the player after a timeout. Closing the player
   window also returns the editor to Idle.
6. Run is disabled while a player is running; Restart stops it and launches a new
   setup. Editor shutdown stops its player. A closed or crashed player never
   terminates the editor. Remove a run folder after a clean exit; keep the last
   failed run's folder for inspection until the next Run.

The player duplicates CPU/GPU scene allocations. Suspend embedded scene rendering
while a run is active by default to avoid competing rendering workloads;
`DispatchRenderWorkloads(view, overlay, drawScene)` already supports this and the
editor UI stays responsive. Retained allocations still consume memory, so
resource preflight and out-of-memory recovery must cover the combined workload.
Do not add automatic editor scene unloading in the first implementation.

Start the player camera at the editor camera's pose and projection. Render into
the same offscreen RGBA8 target format as the embedded preview and letterbox it
into the native window in both resolution modes, so the output matches the
preview regardless of the swapchain format. Fixed resolution keeps its extent;
follow-window mode resizes the offscreen target to the physical framebuffer.
Window resize must not silently change a fixed output resolution. Focus loss
releases captured input. Run is a rendering session; Pause and simulation
controls remain deferred.

### Profiling overlay

Trim the runtime UI to profiling information for the player. Factor the
statistics section of `RuntimeDebugUI` into a reusable profiling overlay;
`RuntimeDebugUI` keeps using it, and `zen_player` shows only the overlay. It
contains no settings controls, since the editor owns configuration. It shows:

- FPS, CPU frame time and a frame-time history graph;
- engine GPU memory against device capacity;
- render resolution, present mode, vsync and RHI mode;
- the algorithm actually rendered and any fallback reason, which is needed to
  read the timings;
- render-graph pass and resource counts, plus preparation, execution and
  submission CPU time;
- per-pass GPU times when GPU timing is enabled, which `RDGMetrics` already
  records as an opt-in.

F1 toggles the overlay, and scene captures omit it. `scene_renderer_demo` keeps
its full runtime UI until the editor's Rendering panel covers those controls;
retiring it is a separate decision.

### Later: editor connection

Add a connection only when debugging or advanced features need it. A loopback TCP
channel, like Godot's remote debugger, would carry logs, status, live settings
updates, camera follow and remote inspection. The setup file stays the startup
contract, so the connection is additive.

## Presets and reproducibility

Use one small versioned file format, provisionally `.zenrender` in JSON, for both
rendering presets and the setup files Run writes; the player opens either. JSON
is chosen because the file will grow into the runnable scene description. Store
scene reference, configuration, the light list with each light's origin, camera
light settings, environment, camera, debug/output selection, run settings and the
scene normalization center and scale. Save portable asset paths relative to the
preset where possible; resolve paths before writing a setup file. Save
atomically, as editor preferences already do with a temporary file and rename,
and reject malformed or unsupported versions without replacing the current
setup. The bundled simdjson library only parses, so add a small writer.

Presets are separate from source glTF files, `engine.cfg`, docking state and editor
preferences. Import a preset transactionally, including scene/environment loading.
Initialize from engine defaults once; explicit preset and setup values take
precedence thereafter. Record source fingerprints for the scene and referenced
asset files so missing assets and changes since loading or saving can be reported
instead of silently resolving to a different model or light setup. A setup file
freezes configuration; it does not copy or freeze the asset files.

## Implementation milestones

These milestones replace the old documents, gizmos and asset-authoring sequence.
Milestones **1–3 are implemented** in the editor and shared renderer. Milestones
4–5 remain planned; there is no new executable or preset-file workflow yet.
The [editor README](../ZenEditor/README.md#rendering-configuration-lighting-and-diagnostics)
documents the controls and capture options.

Windows validation for milestones 1–3: MSVC Debug and Release builds; 112 focused
Debug tests and 61 Release tests, including inline/threaded RHI, GPU pixel checks,
resource rejection, light replacement, scene switching and stale-output checks.
Native fixture and Sponza captures were inspected. The fixture audit found that
forward-path candidates were a minority; unsupported albedo/normal outputs remain
explicitly unavailable. Surface voxel slices expose mip zero. Additional material
channels and radiance views remain follow-up work. macOS verification and player
parity remain in the later acceptance milestone.

| Milestone | Deliverable | Acceptance |
| --- | --- | --- |
| 1. Rendering configuration | Serializable value types and validation; controller draft/applied state; migrate environment and mode controls into Rendering; effective algorithm and fallback status | One source of settings; invalid edits preserve applied state; existing loading and preview still work |
| 2. Lights, shadows and GI controls | glTF-only light loading with the engine fallback removed; manual lights and AABB side/corner presets; camera light; shadow memory preflight; GI editing with explicit Apply for resource changes | Changes affect direct lighting, shadows and GI as applicable; zero-light scenes, light removal and presets work; rejected allocations and fallbacks are visible |
| 3. Render debug view | Output descriptions and renderer-built visualization passes; final, depth, albedo, normal, shadow and voxel views in the editor viewport | Outputs are correct on supported paths, unavailable states are explicit, and mode/scene/extent changes never show stale resources |
| 4. zen_player and Run | Setup file format and shared ZenCore loading; explicit initial renderer settings; `zen_player` with the profiling overlay; Run/Restart/Stop with exit codes and logs | Player scene captures match the embedded preview within tolerance; every configuration value reaches the setup file; repeated launch, stop, close and failure recovery keep the editor usable; the player builds without editor libraries |
| 5. Presets and acceptance | Save/load/reset preset UI, portable path handling, normalization records, the player opening presets directly, diagnostics and platform verification | Reopening a preset reproduces the setup; Windows/macOS and inline/threaded RHI checks pass within supported renderer capabilities |

Complete one milestone at a time, with a working end-to-end rendering workflow at
each checkpoint. Milestones 1 to 3 are editor work and need no new executable.
Define the configuration as serializable values in milestone 1 and add every
setting from milestones 2 and 3 to it, so milestone 4 writes the same values
instead of inventing a schema. Add the preset file workflow in milestone 5. Do
not pull deferred scene authoring back into these dependencies.

## Verification

Use model tests without a GPU for validation, configuration revisions, the light
list (origins, reset, scene-switch reset and carry-over), preset positions (six
sides and eight corners outside the bounds and aimed at the center, rejection
past 32 lights), setup and preset round trips including normalization records,
precedence and setup file completeness. Cover every exposed setting so the player
cannot silently omit a control.

Engine tests must cover the light changes. `BuildSceneLights` returns no lights
for a file without glTF lights; update the `SceneImportTests` cases that expect
six fallback lights and the fallback-suppression case. The demo still renders the
side preset. Light replacement keeps IDs and revisions increasing and detaches
imported animation. The light ball follows or holds position without changing the
scene-light list; its movement refreshes GI radiance and point-light shadows
without revoxelization. The shadow preflight rejects arrays that do not fit.

Use rendering tests and focused native checks for light/shadow/GI invalidation,
resource rebuilds, debug encoding and subresource selection, unavailable forward
outputs, and stale image retirement after resize, algorithm changes and scene
replacement. Reuse small glTF fixtures and Sponza. Compare scene-target captures
from the editor preview and from `zen_player --frames=N --capture=path` at
matching camera, resolution and settings with a suitable image tolerance rather
than requiring bit-identical frames. The current editor `--capture` option reads
the back buffer including the UI, so add a scene-target capture to the editor.

Exercise Run, Stop, Restart, player window close, invalid setup, load failure,
crash, minimize, restore, DPI changes and editor exit on Windows and macOS. Test
inline and threaded RHI modes, and build the player both with and without the
profiling overlay. Extend only the affected existing editor, renderer and
platform suites. A successful unit test run does not replace visual inspection of
framebuffer views and light response.

The release is complete when a user can open a supported glTF scene, set up its
glTF, manual, preset and camera lights, configure all supported rendering options
in one panel, inspect the primary debug outputs, save the setup, and Run it in
`zen_player` with matching settings, profiling information and clear failure
reporting.
