# ZenEngine UI integration review and delivery plan

Reviewed against the current checkout on 30 September 2026. Source proposal:
`ZenEngine_UI_Integration_Research_and_Plan.pdf`, revision 0.1. The original PDF
remains unchanged. Its editor workflow is a useful second track, but the first
delivery should be runtime configuration and diagnostics built on shared UI
support.

## Findings

| Priority | Proposal location | Finding and required change |
| --- | --- | --- |
| High | Pages 1, 2 and 6; executive decision, R6, module boundaries | The proposal defines the product as an editor and scopes ImGui linkage to editor targets. That omits the requested runtime debug/configuration UI. Split shared UI integration, runtime panels, and the future editor. Runtime builds must remain independent of editor models; fully UI-free builds must also work. |
| High | Pages 4 and 8; ADR-UI-001 and Vulkan bridge | The stock Vulkan backend would require a native recording and queue-coordination extension. This checkout already records through `RDGPassCmdEncoder`, submits through `RHICommandListExecutor`, uploads through `StagingUploadQueue`, and tracks retirement. Use those paths for the small initial UI renderer. There is no need to add native Vulkan handles to RenderCore or duplicate submission ownership. |
| High | Pages 6, 9 and 11; frame order and input | Begin UI and resolve actions before camera/scene updates. `GlfwWindowImpl::Update` previously consumed Tab/Escape before any UI capture decision, and `KeyboardMouseInput` retained pending presses. Forward events continuously, filter application actions separately, suppress a captured press until release, and reset on focus loss. |
| Medium | Pages 6 and 10; reusable scene serialization | The inspected scene/system APIs do not provide the proposed command history or a scene save/reload layer. Make document identity, an editable scene format or sidecar, transactions, atomic saving, and failure behavior explicit editor deliverables. Do not assume glTF import supplies authoring persistence. |
| Medium | Pages 7 and 11; embedded viewport | An editor image panel requires texture registration, sampled graph reads, scene extent independent of the OS swapchain, picking coordinates, and input ownership. A runtime overlay proves the shared renderer but does not complete this editor milestone. |
| Medium | Pages 4, 8 and 12; texture/font contract | Pin core and backend together. The first implementation uses the pinned release's supported static atlas mode and advertises no dynamic-texture capability. Dynamic font updates, arbitrary UI images, and generation-safe texture registration remain explicit future work. |

Keep the proposal's one-window policy, deferred multi-viewports, model/UI separation,
transaction-based editing, GPU-lifetime requirements, and acceptance-driven phases.
Docking and an embedded scene image belong to the editor track; runtime controls
can be an overlay. The [upstream docking guidance](https://github.com/ocornut/imgui/wiki/Docking)
also distinguishes docking from the optional additional-window feature and keeps
DockBuilder among internal APIs.

## Dependency and ownership split

```text
scene_renderer_demo -> ZenRuntimeUI -> ZenUI -> ZenCore
                                      |
                                      +-> pinned ImGui core and GLFW backend

future ZenEditor -> EditorUI -> ZenUI
                -> EditorModel -> engine scene and command services
```

`ZEN_BUILD_RUNTIME_UI=OFF` removes both optional UI targets from runtime builds.
When the editor exists, add a separate `ZEN_BUILD_EDITOR` option and build shared
`ZenUI` when either consumer needs it. Do not add an empty editor target before
there is an editor application to build.

`UIContext` owns context/platform lifecycle; `UIRenderer` owns draw translation and
GPU resources; `RuntimeDebugUI` owns runtime panels and the settings draft.
`RendererServer` accepts an application-owned `RenderOverlay` and appends it after
scene passes. This keeps toolkit dependencies out of engine headers and avoids
making the server own editor state.

## Runtime milestone

Implemented entry points and contracts are documented in [the UI runbook](../ZenUI/README.md).

1. Pin Dear ImGui `v1.92.5-docking` with a SHA-256 archive check, matching GLFW backend,
   and engine configuration header. Compile no ImGui Vulkan backend.
2. Copy finalized draw data into graph-owned command packets. Upload geometry through
   engine staging, use per-frame buffers, account for index padding and base-vertex
   offsets, transform/clamp clip rectangles, and declare the font read and color
   load/store write in RDG. Never retain ImGui draw-list pointers across UI frames.
3. Provide render mode, GI configuration, fallback status, voxel rebuild, and basic
   diagnostics. Validate a draft and apply at the boundary before building the scene
   graph. Use the existing settings API for expensive resource transitions.
4. Use F1 to switch debug/camera input contexts. Forward all GLFW events to ImGui;
   filter camera/game actions through the input controller. Settings remain
   session-only; no automatic scene or config writes.
5. Verify draw-packet ownership/clipping, captured input, inline/threaded native
   rendering, resize/minimize/restore, shutdown, and UI-disabled builds.

This delivery deliberately excludes scene authoring, undo/redo, file persistence,
texture viewers, arbitrary draw callbacks, dynamic font textures, multi-window UI,
and dock layout management. Those exclusions are feature boundaries, not claims
that the shared backend already satisfies the full editor proposal.

The runtime configuration follow-up adds automatic application (with resource
changes deferred until an edit ends) and GI/Scene/Config reference tabs.
Cone tracing is the sole GI method, with independent analytic/environment/emissive
contribution controls. The former directional method and its budget controls have been removed. Live controls include camera, environment,
lights, light markers and orbit animation through an application-owned scene
control interface. Asset paths are listed as restart-required. The searchable
reference covers all supported configuration keys, including optional light fields.

The reflectance follow-up validates budgets and GPU storage limits before resource
reconfiguration. Selecting averaged reflectance or increasing its resolution sets
at least the required 5/40/320 MiB budget for 64/128/256 grids; manually insufficient
budgets leave the current renderer unchanged. Budget edits in owner mode preserve
the voxel resources, and the panel reports the active reflectance policy separately.

## Editor milestones after runtime UI

| Milestone | Work | Exit evidence |
| --- | --- | --- |
| Shared images | Generation-safe UI texture registry, graph-declared sampled views, retained resource references, per-draw binding strategy, and font update contract. | Delayed GPU completion plus texture replacement/unregistration produces no stale reads or lifetime errors. |
| Editor shell | Separate application/model targets, stable panel IDs, docked default, menu/commands, reset/recovery, versioned per-user layout. | Fresh launch, restart, and close/reopen restore a usable workspace; runtime builds still work without editor linkage. |
| Embedded scene | Independent offscreen scene target, panel-driven extent, camera/picking mapping, modal/text/gizmo/viewport priority. | Splitter resize and fractional DPI preserve aspect and picking; input actions have one owner. |
| Editing loop | Stable document/object identity, explicit transform inspector, transaction history, dirty state, scene serialization, atomic save/reload and failure recovery. | Select, preview, cancel, commit, undo, redo, save, close, and reload the same object; failed saves preserve the prior file and dirty state. |
| Tools and hardening | Assets/materials, render-target/graph viewers, bounded logs/caches, performance and accessibility requirements. | Complete a real development workflow and retain validation, resource-growth, and measured frame-time evidence. |

Physical DPI/IME testing, editor color reference patches, the PDF's 200-resize and
100-panel-cycle stress targets, and performance budgets remain future acceptance
work. Deterministic clip-scaling tests and a short native smoke run do not substitute
for those checks.

## Verification from this implementation

The 2026-09-30 averaged-reflectance follow-up passes all 7 runtime integration
tests in both MSVC Debug and Release, plus 8 focused resource-planning/settings
tests. Native transitions cover 64/128/256 grids, compute/geometry voxelization,
and inline/threaded RHI. Rejected budgets preserve the existing voxel resources;
corrected budgets enable averaged storage and GPU execution. A separate Release
Sponza run rendered 256-cubed averaged reflectance with a 320 MiB budget and UI,
with no application errors, Vulkan VUIDs, or synchronization hazards. The active
config was restored byte for byte. Evidence is under `build/reflectance-live-hang/`.

The reported resource status 5 is a configured-budget rejection, not a device-loss
or VRAM-exhaustion report. The inspected live process continued completing frames;
these checks did not reproduce a persistent GPU hang. Valid structural changes
still synchronously retire and rebuild resources and can briefly pause rendering.

Windows MSVC Debug validation completed with 576 passing tests: 522 RenderCore,
39 Vulkan RHI, 7 input-controller, and 8 UI packet/reflection tests. The UI-enabled
build rendered an inspected Sponza frame with inline RHI, and a threaded 48-frame
smoke run exercised mode changes, resize, minimize, restore, capture, and teardown.
Both UI-enabled and UI-disabled native runs completed. Explicit `--ui` on the
UI-disabled build reported the unsupported build configuration and exited with
failure as intended.

The captured image exposed an existing scalar vertex-input reflection bug:
SPIRV-Reflect reports zero vector components for a scalar, so the previous code
computed a 16-byte stride for ImGui's 20-byte packed vertex. The corrected scalar
size now has a shader-reflection regression test. Newly grown geometry buffers
are initialized over their full capacity before partial updates, matching RDG's
whole-buffer content tracking.

There were no new UI-attributable Vulkan or RDG warnings after these fixes. The
UI-disabled baseline also reports unsupported surface-maintenance capability,
validation DebugPrintf configuration messages, and unknown-content warnings for
the separate capture passes. These baseline messages were retained, not suppressed.
Engine allocation reports returned to zero live allocations on native teardown.
