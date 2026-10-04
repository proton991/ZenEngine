# ZenEditor rendering plan

Revised direction, 4 October 2026. This is the active plan for work after the
implemented ZenEditor viewer. It supersedes the delivery order of steps 5 onward
in [the original implementation plan](ZenEditorImplementationPlan.md).

Develop ZenEditor into a workspace for configuring and evaluating rendering.
Use the existing glTF loading and rendering support. Put rendering, lighting,
GI, debug output, and render launch settings in one **Rendering** panel. Press
**Run** to open a separate render window using the selected scene and the complete
configuration.

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
4. Press Run. Validate the complete configuration, capture the camera and settings,
   and launch a separate native render window. Keep the editor available.
5. Close the render window or press Stop to return to the embedded preview. Adjust
   settings and Run again, or use Restart to replace an active run.
6. Save and load rendering presets to reproduce a setup without introducing a
   project format or scene authoring system.

Proposed first-release behavior: the render window uses an immutable snapshot.
Changes made after launch are marked **Pending restart**; Restart takes a fresh
snapshot. Live synchronization with the external window can follow once this
workflow is reliable. Camera movement inside the render window remains local to
that run.

## Current implementation and required changes

| Area | Current implementation | Work required |
| --- | --- | --- |
| Scene loading | `ParseScene` in the editor model, transactional scene replacement, read-only inspection and navigation; `RenderScene` normalizes the scene to unit extent | Move render-scene preparation into ZenCore so the render application loads and normalizes identically |
| Settings UI | `RenderSettingsPanel` edits environment settings and displays GI settings read-only; `ScenePanel` selects PBR, Voxel GI or Voxels | Consolidate controls and introduce one validated configuration model |
| Algorithms | `rc::RenderOption` provides PBR, Voxel GI and voxel visualization; a fallback to PBR is only logged and is re-evaluated every frame | Separate lighting algorithm selection from debug output selection; report the effective algorithm and a latched fallback reason |
| Configuration source | `RendererServer`, `VoxelGIRenderer`, both voxelizers, `VoxelizerBase` and `DeferredLightingRenderer` read `engine.cfg` while initializing; `EditorViewport::PrepareScene` reads the default environment from it | Accept explicit initial settings so a launch configuration is applied before the first allocation |
| GI controls | `VoxelGIRuntimeSettings`, validation, resource preflight and apply APIs already exist | Connect editor actions; distinguish inexpensive updates from resource rebuilds |
| Lights | `SceneLights` supports point, directional and spot lights, add/update/remove and revisions, up to 32 lights. `RenderScene` adds the glTF lights, or six automatic AABB-side lights when the file has none | Load only glTF lights; add manual lights, AABB side and corner presets, and a camera light; replace lights without resetting IDs or revisions |
| Shadows | `SceneShadowRenderer` keeps one array layer per shadow face (six per point light) and reallocates the array when the face count changes; only the Voxel GI path uses it | Preflight shadow memory; treat face-count changes as resource changes |
| Debug resources | G-buffer attachments exist only as per-frame graph resources, and only when no material needs the forward path; shadow maps, voxel data and GI radiance exist | Add renderer-built visualization passes and report unavailable outputs; most resources are not currently editor images |
| Run | Toolbar Play/Pause/Stop actions are disabled; their reasons still cite the old plan step 8 | Add rendering Run/Restart/Stop lifecycle and a render application; update the disabled reasons |

Relevant implementation references are [RenderSettingsPanel](../ZenEditor/ImGui/Source/Panels/RenderSettingsPanel.cpp),
[RendererServer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/RendererServer.h),
[GI settings](../ZenCore/Include/Graphics/RenderCore/V2/VoxelGISettings.h),
[SceneLights](../ZenCore/Include/Graphics/RenderCore/V2/SceneLighting.h),
[SceneShadowRenderer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h),
[DeferredLightingRenderer](../ZenCore/Include/Graphics/RenderCore/V2/Renderer/DeferredLightingRenderer.h), and
[EditorViewport](../ZenEditor/Rendering/Include/Editor/Rendering/EditorViewport.h).
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
| Render window | Output dimensions, follow-window or fixed render resolution, camera projection parameters, present mode and supported device options; Run, Restart and Stop |
| Status | Validation errors, pending changes, requested/applied/running revisions, actual output and fallback reason, GPU memory and available frame timings |

The main toolbar may mirror Run and Stop as actions, but the Rendering panel owns
their configuration. Light markers and other rendering overlays belong here too.
Do not duplicate editable light settings in the Inspector. A selected imported
light may link to this panel while its source properties remain read-only.

Use engine validation and limits rather than independent UI-only ranges. Explain
which controls affect direct light, indirect light, environment lighting or the
skybox. In particular, GI contribution toggles must not be presented as switches
for all direct lighting or specular environment lighting, and the camera light
must be labelled as direct lighting only.

## Configuration and application rules

Introduce a toolkit-independent rendering configuration owned by
`EditorController`, with scene reference, light list, camera light, environment,
algorithm, GI/shadow settings, debug selection and output settings. Track the
editable draft, last applied preview revision, and running snapshot separately.
Widgets dispatch actions; they do not directly mutate GPU resources or the active
`RenderScene`.

Reuse `VoxelGIRuntimeSettings`, `SceneLight` and their validators. Define shared
serializable values and runtime application code below the editor UI so the
editor and render application use the same interpretation. Do not create a
second renderer or duplicate the demo's startup configuration logic.

| Change | Application behavior |
| --- | --- |
| Light values that keep the shadow face count, environment scalar values, cone parameters, camera light, algorithm and debug selection | Validate and stage at a frame boundary; interactive preview may coalesce changes |
| Adding, removing or clearing lights, applying a preset, and changes to enabled, type, casts shadows or a zero/non-zero intensity | Stage as one structural change. When shadows are in use, preflight the resized shadow array and reject the whole edit if it does not fit. Coalesce drags so they do not reallocate every frame |
| Environment texture | Load as a candidate; publish only on success and retain the previous environment on failure |
| Voxel resolution, voxelizer, reflectance resources and shadow resolution | Require Apply after editing; preflight resource limits and perform at most one rebuild per commit |
| Device or presentation options requiring recreation | Mark as requiring restart; record them explicitly in the launch snapshot |

Shadow memory is resolution² × 4 bytes × max(2, shadow faces): one face per
directional or spot light and six per point light, counting enabled shadow
casters with non-zero intensity. Thirty-two shadow-casting point lights at 2048
need about 3 GiB. `ValidateVoxelGIResources` checks only reflectance memory, so
add a shadow preflight beside it.

Run and Restart validate the complete draft, commit preview-compatible changes,
and include restart-only values in the snapshot. They must not silently launch
stale preview settings when an edit is invalid or a resource load is pending.
Reset supports individual sections and the whole setup.

Read `engine.cfg` once to seed defaults; explicit configuration values take
precedence after that. Extend `RendererServer::Init`, and the renderers it
creates, to accept initial `VoxelGIRuntimeSettings`, light marker settings and the
default environment. Otherwise the render application allocates GI and shadow
resources from `engine.cfg` first and then rebuilds them, or fails on an
`engine.cfg` setting that the launch configuration was meant to replace.

The existing GI apply path may wait for outstanding work, destroy resources and
allocate replacements lazily. Account for that behavior explicitly: validation
failure leaves the applied configuration intact, while a later allocation failure
must be reported as an actual fallback or failed application. Do not claim atomic
GPU rollback from the current API. Keep the last successful configuration for
recovery and never retain renderer pointers across a rebuild.

`RendererServer` currently only logs a fallback and retries the GI path every
frame. Add a structured status with the effective algorithm and a reason, such as
incomplete voxel coverage, voxelizer or GI initialization failure, or shadow
allocation failure. Latch failures until the configuration changes. The editor
and the render application report this same status.

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
generator into ZenCore `SceneLighting` so the demo, the editor and the render
application share it. The demo applies the side preset explicitly where it relied
on the automatic fallback, so its output and `boundsPresetLights` behavior are
unchanged.

The rendering setup owns one light list. Each entry records its origin (glTF,
manual, side preset or corner preset) and a configuration ID mapped to a runtime
`LightId`. Imported glTF lights are editable copies; **Reset lights** restores the
file's lights and removes added ones. A list with zero lights is valid.

Apply the list through a new `RenderScene` method that replaces the lights while
keeping `LightId` allocation and light revisions increasing. Do not assign a fresh
`SceneLights` to a live scene, as the demo does today. IDs would restart at 1 and
collide with the imported-light mapping that animation updates. Revisions could
also repeat a value voxel GI has already recorded, which skips relighting (six
fallback lights replaced by six custom lights both reach revision 7). The same
method detaches imported-light animation updates once the list has been edited.
Animation stays frozen in both the editor and the render application in this
release.

Positions and directions use the engine's normalized rendering world, which
scales by one over the largest extent about the bounds center. Preview and
standalone loading apply identical normalization because both build a
`RenderScene`. Any change to the source geometry changes that transform, so
positions stored in normalized units would silently move. The `engine.cfg` light
positions already broke this way after an earlier scale change. Presets therefore
also store the normalization center and scale used when saving, so lights can be
mapped into a changed scene or the mismatch reported.

Opening another scene resets the light list to that scene's glTF lights and
resets the camera. Algorithm, GI, environment, camera light and output settings
carry over; the environment already carries across scenes in the current editor.
An explicitly loaded preset restores its own scene and light list together.
Persistent source node identities and per-material overrides are outside this
scope.

### Camera light

The camera light is a point light at the current camera position that lights
everything around it within its range, like a headlight that radiates in all
directions. Configure enabled, color, intensity and range in the Lighting
section. It follows the camera of whichever view is rendering: the editor camera
in the embedded preview, and the render window's local camera during a run. It
has no marker, since it sits at the eye. In orthographic views it uses the
camera's eye position, so its falloff depends on the orbit distance.

Falloff matches other point lights: inverse square with a smooth cutoff,
`1 - (d / range)^4`, that reaches zero at the range; a range of zero means no
cutoff. Default intensity and range derive from the normalized scene size and
remain editable. Surfaces very close to the camera are much brighter than distant
ones.

The camera moves every frame, so in this release the camera light affects direct
lighting only and casts no shadows. Implement it as a dedicated light that
`RenderScene::Update` writes from the render camera into its own field of the
scene uniform data, evaluated by the deferred and forward direct-lighting shaders.
It is not a `SceneLights` entry. It does not use one of the 32 slots, change light
revisions, enter the shadow array or inject into voxel GI. As a `SceneLights`
entry it would advance the light revision on every camera move, making voxel GI
re-inject radiance and a point-light shadow re-render six faces each frame.

Store the camera light's settings, not its position, in the launch snapshot and
presets.

## Algorithms and debug outputs

Start with the implemented PBR path and PBR with voxel cone tracing GI. Voxel
visualization becomes a debug output rather than a competing lighting algorithm.
Keep compatibility with existing command-line modes at the adapter boundary.
Future algorithms can register capabilities when implemented; hardware ray query
lighting and path tracing are not promised by this editor plan.

Add an engine-owned debug output description containing a stable output ID,
availability, extent, format, interpretation and valid subresources. Build a
visualization pass into the same RDG frame and return its display image to the
editor or render window. The renderer that owns a producer builds its views:
G-buffer attachments are per-frame graph resources looked up by name, so
`DeferredLightingRenderer` builds their visualization. Keep transient graph
resources inside that frame and retain displayed images through GPU completion.
Panels must not cache raw G-buffer or shadow pointers or depend on internal
attachment names.

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

## Run window architecture

**Proposed implementation:** add a small `zen_render` executable launched and
managed by ZenEditor. Each process owns its native window, device, renderer server
and runtime scene. The current `RendererServer` binds one scene and presentation
viewport, so a separate process avoids making simultaneous render contexts and
multi-window graph submission prerequisites.

Share loading, rendering configuration and renderer code through engine-owned
services. Move render-scene preparation into ZenCore: the `SceneData` setup in
`EditorViewport::PrepareScene`, including the default environment, plus the light
setup functions and the launch configuration types. `zen_render` must not link
ImGui, `ZenEditorUI`, `ZenEditorServices` or `ZenEditorRender`. It may link the
toolkit-free `ZenEditorModel`, which depends only on ZenCore, for `ParseScene` and
`EditorCamera`. The editor remains the only settings UI. The render window shows
the chosen output and supports local camera navigation, resize and close.

1. Resolve the successfully loaded scene, assets and environment, capture the
   camera pose/projection and output policy, and validate every setting.
2. Write a versioned, immutable launch manifest with resolved paths and a unique
   run ID. Include the complete light list and camera light settings so child
   startup cannot replace them with unrelated `engine.cfg` values.
3. Launch `zen_render` asynchronously through a Windows/macOS process wrapper.
   Pass arguments as an argument vector and resolve executable/data paths
   independently of the current working directory.
4. The child validates the manifest and creates its renderer with the manifest's
   initial settings rather than `engine.cfg`. It loads the same scene and
   resources, applies the remaining settings in dependency order, and reports
   startup success only after a rendered frame. Return the applied revision,
   effective algorithm and fallback status through a small status channel;
   collect logs and exit status in the editor.
5. Stop requests graceful shutdown. Restart stops the old child before launching
   a new snapshot. Repeated Run focuses the existing run; it does not create
   duplicate render processes. Stop and child close both return the editor to Idle.

Use Idle, Starting, Running, Stopping and Failed states. Disable duplicate launch
actions during transitions. Surface launch/load/device errors in the Rendering
panel and Output log, preserve the editor setup, and permit another attempt.
Editor shutdown must close its managed child and clean up launch files after the
child exits. A closed or crashed child must not terminate the editor.

The separate process duplicates CPU/GPU scene allocations. Suspend embedded scene
rendering while a run is active by default to avoid competing rendering workloads;
`DispatchRenderWorkloads(view, overlay, drawScene)` already supports this and the
editor UI stays responsive. Retained allocations still consume memory, so
resource preflight and out-of-memory recovery must cover the combined workload.
Do not add automatic editor scene unloading in the first implementation.

Default the render camera to a copy of the editor camera. Render into the same
offscreen RGBA8 target format as the embedded preview and letterbox it into the
native window in both resolution modes, so the output matches the preview
regardless of the swapchain format. Fixed resolution keeps its extent;
follow-window mode resizes the offscreen target to the physical framebuffer.
Window resize must not silently change a fixed output resolution. Focus loss
releases captured input. Run is a rendering session; Pause and simulation
controls remain deferred.

## Presets and reproducibility

Use a small versioned rendering preset, provisionally `.zenrender.json`, shared
with the launch schema. Store scene reference, configuration, the light list with
each light's origin, camera light settings, environment, camera, debug/output
selection and the scene normalization center and scale. Save portable asset paths
relative to the preset where possible; resolve launch paths before starting the
child. Save atomically, as editor preferences already do with a temporary file and
rename, and reject malformed or unsupported versions without replacing the
current setup. The bundled simdjson library only parses, so add a small writer
for preset and manifest output.

Presets are separate from source glTF files, `engine.cfg`, docking state and editor
preferences. Import a preset transactionally, including scene/environment loading.
Initialize from engine defaults once; explicit preset and launch values take
precedence thereafter. Record source fingerprints for the scene and referenced
asset files so missing assets and changes since loading or saving can be reported
instead of silently resolving to a different model or light setup. A launch
snapshot freezes configuration; it does not copy or freeze the asset files.

## Implementation milestones

These milestones replace the old documents, gizmos and asset-authoring sequence.
They are planned work, not completed features.

| Milestone | Deliverable | Acceptance |
| --- | --- | --- |
| 1. Rendering configuration | Shared value types and validation; controller draft/applied state; migrate environment and mode controls into Rendering; explicit initial renderer settings instead of `engine.cfg` reads; effective algorithm and fallback status | One source of settings; invalid edits preserve applied state; existing loading and preview still work |
| 2. Run window slice | Launch manifest, `zen_render`, process lifecycle and Run/Restart/Stop for the milestone 1 settings: scene, algorithm, environment, GI values and camera | Standalone scene-target captures match the embedded preview within tolerance; repeated launch/close/failure recovery keeps the editor usable |
| 3. Lights, shadows and GI controls | glTF-only light loading with the engine fallback removed; manual lights and AABB side/corner presets; camera light; shadow memory preflight; GI editing with explicit Apply for resource changes; each new setting added to the manifest | Changes affect direct lighting, shadows and GI as applicable; zero-light scenes, light removal and presets work; rejected allocations and fallbacks are visible; the render window matches the preview |
| 4. Debug output pipeline | Output descriptions and renderer-built visualization passes; final, depth, albedo, normal, shadow and voxel views | Outputs are correct on supported paths, unavailable states are explicit, mode/scene/extent changes never show stale resources, and the render window shows the selected output |
| 5. Presets and acceptance | Save/load/reset UI, portable path handling, normalization records, diagnostics and platform verification | Reopening a preset reproduces the setup; Windows/macOS and inline/threaded RHI checks pass within supported renderer capabilities |

Complete one milestone at a time, with a working end-to-end rendering workflow at
each checkpoint. Establish the launch schema in milestone 1 so milestone 2 uses
the same values. The Run slice comes second so process, configuration-injection
and parity problems surface before more settings must round-trip; each later
milestone extends the manifest and the parity checks with its settings. Add the
preset file workflow in milestone 5. Do not pull deferred scene authoring back
into these dependencies.

## Verification

Use model tests without a GPU for validation, configuration revisions, the light
list (origins, reset, scene-switch reset and carry-over), preset positions (six
sides and eight corners outside the bounds and aimed at the center, rejection
past 32 lights), preset round trips including normalization records, precedence
and launch snapshot completeness. Cover every exposed setting so launch parity
cannot silently omit a control.

Engine tests must cover the light changes. `BuildSceneLights` returns no lights
for a file without glTF lights; update the `SceneImportTests` cases that expect
six fallback lights and the fallback-suppression case. The demo still renders the
side preset. Light replacement keeps IDs and revisions increasing and detaches
imported animation. The camera light follows the camera without changing light
revisions, shadow faces or GI. The shadow preflight rejects arrays that do not
fit.

Use rendering tests and focused native checks for light/shadow/GI invalidation,
resource rebuilds, debug encoding and subresource selection, unavailable forward
outputs, and stale image retirement after resize, algorithm changes and scene
replacement. Reuse small glTF fixtures and Sponza. Compare scene-target captures
from the embedded and standalone renderers at matching camera, resolution and
settings with a suitable image tolerance rather than requiring bit-identical
frames. The current `--capture` option reads the editor back buffer including
the UI, so add a scene-target capture to both executables.

Exercise Run, Stop, Restart, external close, startup failure, crash, minimize,
restore, DPI changes and editor exit on Windows and macOS. Test inline and threaded
RHI modes and ensure the render executable builds without ImGui. Extend only the
affected existing editor, renderer and platform suites. A successful unit test
run does not replace visual inspection of framebuffer views and light response.

The release is complete when a user can open a supported glTF scene, set up its
glTF, manual, preset and camera lights, configure all supported rendering options
in one panel, inspect the primary debug outputs, save the setup, and Run it in a
separate window with matching settings and clear failure reporting.
