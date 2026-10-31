# ZenEditor Render Graph Viewer Implementation Plan

Add a **Show RenderGraph** button to the editor's top toolbar. Clicking it opens a
separate, resizable **Render Graph** window showing the compiled render graph for
the active scene and applied rendering settings. Render the graph through the
existing ImGui → UIRenderer → RenderCore → VulkanRHI path.

Status: implemented, with follow-up fixes for idle waiting, frozen source changes,
capture throttling, compact toolbar sizing, and stable pass inspection. This
document records the implemented design and its acceptance scope. Windows tests
cover inline and threaded Vulkan; macOS acceptance remains outstanding.

## Behavior

- Place the labeled button at the right end of `DrawEditorToolbar`, replacing
  the decorative `SCENE VIEWER` badge. Reserve space for Open/Save, Undo/Redo,
  Play/Pause/Stop and their gaps before choosing the graph button's label. If the
  labeled button does not fit at the 800×500 minimum window size and the
  current UI scale, draw it as an icon button whose tooltip is the action label.
  Shift the run controls from center as needed to keep them reachable.
  Do not omit it or add a second toolbar row.
- Register `actions::ShowRenderGraph` (`view.render_graph`) from `EditorApplication`,
  as `actions::Exit` is, because the application owns the window. It is always
  enabled and opens the window even without a scene. Also list it in the View menu,
  in both the native menu model and the ImGui menu bar, so it stays reachable.
- Use one nonmodal native popup window. Further clicks restore and focus it.
  Closing it leaves the editor and any play session running.
- Show the latest captured graph from whichever window renders the shared scene:
  the editor Scene panel, or the Play window while Play is active (the editor then
  dispatches without scene work). Label the source, scene, applied settings
  revision, frame, requested algorithm, effective algorithm, fallback reason when
  present, and whether the graph ran as one graphics submission or as grouped
  multi-queue submissions.
- Interpret **current setting** as the configuration actually used to build that
  frame. The applied revision already advances for live edits (`CommitPreview`),
  Apply (`Commit`), Revert, environment changes and scene changes, so those appear
  in the next capture without user action. Edits awaiting Apply
  (`EditorRenderingApplyStatus::AwaitingApply`) remain pending: the viewer shows a
  notice and never applies settings or rebuilds scene resources. Starting Play
  applies pending edits, and edits are rejected while Play runs.
- Refresh automatically while the viewer is open. Provide **Freeze**, **Refresh**,
  and **Fit**. Refresh requests a capture of the next naturally rendered scene
  frame; it does not advance a paused scene. A frozen view identifies its captured
  revision and retains that snapshot across scene replacement and Play changes,
  with its original source label and a stale marker until explicitly refreshed.
- When no matching snapshot exists, show a waiting or no-scene message. When
  scene rendering is paused, hidden, or minimized, retain the last snapshot and
  identify it as stale. Discard or clearly separate snapshots from a replaced
  scene so an old graph cannot appear to belong to the new scene.

The popup is a native `platform::NativeWindow`, with its own `RHIViewport`,
`ui::UIContext`, and `ui::ImGuiRenderer`, sharing the editor's `RenderDevice`.
This follows the `RuntimeOverlay` play-window pattern without adding another
device or enabling ImGui platform multi-viewport support.

## Existing integration points

| Area | Existing code | Implementation |
| --- | --- | --- |
| Toolbar, menus and actions | [EditorWorkspace.cpp](../ZenEditor/ImGui/Source/EditorWorkspace.cpp), [EditorActions.h](../ZenEditor/Model/Include/Editor/Model/EditorActions.h) | Draw the button, add the View menu entries, and declare the action ID |
| Window orchestration | [EditorApplication.cpp](../ZenEditor/App/EditorApplication.cpp), [RuntimeWindow.cpp](../ZenApp/RuntimeWindow.cpp), [RuntimeOverlay.cpp](../ZenApp/RuntimeOverlay.cpp), [UIContext.h](../ZenUI/ImGui/Include/ImGui/UIContext.h) | Follow the secondary-window lifecycle, per-window ImGui context, resize and presentation sequence |
| Applied configuration | [EditorRenderingState.h](../ZenEditor/Model/Include/Editor/Model/EditorRenderingState.h), [EditorController.cpp](../ZenEditor/Services/Source/EditorController.cpp) | Read the applied revision, apply status, scene generation and Play state |
| Scene dispatch | [RendererServer.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RendererServer.cpp) | Arm a capture only on dispatches that render the scene |
| Graph and execution | [RenderGraph.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RenderGraph.h), [RenderGraph.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderGraph.cpp), [RenderDevice.cpp](../ZenCore/Source/Graphics/RenderCore/V2/RenderDevice.cpp) | Extract topology, accesses and the final schedule in `RDGExecutor::ExecuteTransaction` |
| Dependencies and resources | [RDGDefs.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGDefs.h), [RDGSchedule.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGSchedule.h), [RDGResourceManager.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGResourceManager.h) | Copy dependency records, resource versions and queue groups |
| Metrics precedent and attribution | [RDGMetrics.h](../ZenCore/Include/Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h), [EditorViewport.cpp](../ZenEditor/Rendering/Source/EditorViewport.cpp) | Mirror the metrics hook; fix GPU timing attribution for a third window |
| GPU UI drawing | [ImGuiRenderer.cpp](../ZenUI/ImGui/Source/ImGuiRenderer.cpp), [UIRenderer.cpp](../ZenUI/Renderer/Source/UIRenderer.cpp) | Render graph geometry and labels through the existing Vulkan-backed UI passes |

`RenderGraph::GetDependencies()` already keeps one record per reason, including
several reasons between the same pair of passes: `AddDependency` appends every
record and deduplicates only the adjacency used for sorting. `GetSchedule()` adds
layout requirements and queue groups. Node and resource details have no public
API, IDs belong to one build, and Begin/Reset clear all of it. The device has one
`frame_rdg` that every window's dispatch rebuilds, so the graph name cannot
identify the scene, and the popup's own dispatch would replace the scene graph
before the UI could read it.

Three compiler behaviors shape the snapshot:

- **Culling** removes every dependency that touches a culled pass. Culled passes
  keep their declared accesses but have no compiled edges.
- **Execution mode.** Without available async compute, `ExecuteRenderGraph` records
  the whole graph on one graphics command list through `ExecutePrepared`; the
  logical `RDGSchedule` groups are computed but not submitted. With async compute,
  `ExecuteScheduledGraph` replaces the plan's schedule with one adjusted for
  submission history before recording.
- **One frame graph.** GI preparation (BLAS/TLAS builds, distance-field
  construction) records into `frame_rdg`, so it appears in the main graph. The only
  other graph executed inside a scene dispatch is the staging upload graph, which
  `ExecuteRenderGraph` flushes between preparing and refreshing the frame plan.

`RDGMetricsSnapshot` is useful for profiling but is not a complete topology
source: `maxNodeDetails` defaults to 32 records, and sampling decides whether a
graph is observed at all. The viewer must work independently of metrics sampling
and GPU timing settings.

## Snapshot capture

Introduce an owned, UI-independent `RDGDebugSnapshot` in RenderCore. Extract it
with friend access to the graph, as `RDGMetrics` already does, rather than
exposing mutable graph internals to editor widgets.

The snapshot contains:

- Graph name, build generation, capture status, execution mode (single graphics
  list or grouped multi-queue), compile statistics, and warnings. The capture hook
  is only armed on scene presentation; there is no transfer-only capture mode.
- Every declared pass: build-local ID, copied name, type, live or culled, execution
  order when live, requested queue preference, async eligibility, planned queue,
  and submission group in grouped mode.
- Resources: logical ID, copied name, texture or buffer description, lifecycle
  (imported, transient, or persistent), and declared versions and access ranges.
  Physical allocation IDs may be copied for diagnostics; GPU pointers are excluded.
- Pass resource accesses for every pass, culled passes included, and the
  dependency records as the compiler produced them: producer, write-after-read,
  write-after-write, and layout-change reasons with resource and version. Several
  reasons between one pair remain separate records.
- In grouped mode, submission groups, group predecessors, external predecessor
  identifiers, and queue-equivalence information. External producers appear as
  external references; they are not invented passes in the captured graph.

Capture in `RDGExecutor::ExecuteTransaction`, after `BuildGroupBarriers` and beside
the existing `RDGMetrics::Compiled` call, where the refreshed plan and the schedule
used for recording are valid. Build the candidate whether or not metrics observe
the graph. Publish it only in the success branch after submission handoff, next to
`RDGMetrics::End`, and discard it on rollback. A snapshot taken at `End()` would
miss compilation, culling, and scheduling, because `End()` only flattens accesses.
Copy CPU metadata without GPU readback or waiting for GPU completion.

`RendererServer::DispatchRenderWorkloads` accepts a one-shot capture request and
arms the device only when that dispatch renders the scene (`renderScene`). The
device captures only inside `ExecuteRenderGraph(RHIViewport*)`; the upload graph
runs through the other overload and is never captured. Popup dispatches, paused
Play, and editor frames during Play pass `drawScene=false` and cannot consume the
request. The host stamps the source, scene generation, and applied revision when
it requests: settings cannot change between `UpdateRenderingSettings` and the
dispatch in one iteration. After the dispatch it reads `GetRenderingStatus()` for
the requested and effective algorithm and the fallback reason. Editor metadata
stays in an editor-owned envelope around the generic RenderCore snapshot.

In threaded mode `RenderFrameResult::eSubmitted` means accepted for submission;
label it that way and never claim GPU completion. A failure reported later by the
submission ticket blocks every submission on the device, and the editor already
ends its loop on `eBlocked`, so the viewer needs no reconciliation path of its own.
Preserve the previous valid capture and surface errors when Begin, End,
compilation, recording, or submission fails, or when the dispatch is declined for
swapchain recreation.

The main view is the compiled `frame_rdg`, including host passes: the editor UI,
picking, bounds, and mesh preview in the editor; the compositor and Play overlay
in Play. The staging upload graph is excluded from the first version. A static
catalog of all possible passes would not represent the active settings or frame.

## Ownership and refresh

Put the capture policy in a toolkit-independent Model class,
`RenderGraphCaptureState`: open state, pending request, refresh throttle, freeze,
latest and pending snapshots, stale and error state, and source and
scene-generation matching. `EditorModelTest` can cover it without a GPU device; a
Services class would need the Vulkan `EditorRenderingTest`. The application-owned
`RenderGraphWindow` holds this state while the popup exists.
`EditorApplication::RenderIteration` asks it whether to request a capture before
the editor dispatch and before `play.Render`, and hands it the result after each.
RenderCore builds the snapshot on the thread that already owns the graph; the
ImGui view receives immutable data. The RHI worker never reads UI state or holds
graph pointers for this feature.

Capture only while the popup requests it. Coalesce normal live refreshes to a
maximum of four captures per second, including applied revision changes while
dragging live settings. Open, manual Refresh, source/scene changes, and resumed
scene rendering can request immediately. Freeze retains the previous capture
across all automatic source changes. Continue capturing periodically even when
settings are unchanged because GI preparation,
resource reuse, and other conditional work can change the graph between frames.

Bound storage to the latest displayed snapshot and one pending replacement.
Freeze suspends replacement. Closing the window disables capture and releases its
snapshots. Any explicit size limit must report incomplete data and omitted counts;
never silently present a truncated graph as complete.

If Freeze is enabled with no accepted snapshot, allow one successful capture once
the scene can render. Keep initial retries throttled, and stop automatic capture
after success. This does not advance a paused scene or replace an existing frozen
snapshot.

Do not select the latest global metrics snapshot: the popup itself executes UI
passes and would replace the scene graph. Existing GPU timing attribution has the
same hazard. `EditorViewport::UpdateGPUTimings` keeps GI pass timings only for
graphs named `frame_rdg`, which the popup's UI-only dispatch also is; with GPU
timings enabled, samples landing on popup frames would clear the scene's GI
timings. Carry a scene-dispatch flag or dispatch identity into metrics snapshots
and filter on it instead of the graph name. Play diagnostics read
`GetLastSnapshot()` directly after their own dispatch and are unaffected.

## Graph canvas and Vulkan rendering

Implement a focused `RenderGraphView` in the editor ImGui layer. Use a deterministic
top-to-bottom layered layout based on dependency order, with graphics, compute,
and transfer queue lanes side by side. Curved edges connect spread ports on a
producer's bottom edge to a consumer's top edge, with downward arrowheads and
routing points around intervening cards. In single-list mode, show one
graphics lane and mark async-compute
requests as not applied. In grouped mode, place passes by their group's queue, and
keep logical lanes separate from physical queue identity: an async request or a
logical compute group does not prove independent hardware execution, and queue
aliases share hardware.

Show pass nodes with directed, resource-labeled dependency edges. Draw one edge
per pair of passes and list all of its reasons in the tooltip and inspector.
Selecting a pass opens an inspector with its reads, writes, versions, resource
descriptors, queue decision, and dependency reasons. Culled passes are hidden by
default; when shown, they appear detached, with declared accesses but no compiled
edges and no executed order. A legend explains pass colors and dependency styles.
Ordering-only edges and resource dependencies remain visually distinguishable.
When both connect the same pair, one resource edge carries both kinds of detail.

Support pan, cursor-centered zoom, Fit, pass-name search, and selection. Keep valid
positions and selection for surviving passes across conditional topology changes,
and preserve pan/zoom during refresh. Match passes by name, type and occurrence
among repeated names; build-local IDs remain display details, not persistent
identity. Identical repeated passes are distinguishable only by their occurrence.
Check every live pass in execution order and move its layer below all current
predecessors before reserving retained cells. Place new or displaced passes in free
cells, preserving other valid positions. Recompute lane widths from currently
visible passes so hidden culled and unused queue lanes leave no empty bands.
Adjust pan when necessary to keep the selected pass at its previous screen position.
An absent selected pass remains remembered and is selected again if it returns.
Rebuild layout when its topology fingerprint changes; explicit Fit also discards
retained positions and rebuilds a compact layout before framing it. Clip nodes
and edges to the canvas and reduce label detail when zoomed out.

The [styling implementation](ZenEditorRenderGraphViewerStyling.md) adds queue-tinted
lanes, card nodes, native-size text, selection/trace focus, collision-free focused
edge labels, resource tables, host filtering, a minimap and short Fit/search
animations. Full layout/Fit reduces crossings; routine captures retain valid
positions. Animations request redraws only until they finish.

The rendering path is:

```text
Owned scene graph snapshot
  -> layout and input handling
  -> ImGui draw geometry for nodes, edges, arrows, grid, and text
  -> ImGuiRenderer / UIDrawPacket
  -> UIRenderer RDG graphics passes using UIRenderSP
  -> RenderDevice / RHI command recording
  -> VulkanRHI draws into the popup swapchain
```

CPU work prepares topology, layout, hit testing, and draw geometry. Vulkan performs
canvas rasterization, blending, clipping, and presentation. This satisfies GPU
rendering without requiring a compute-shader layout algorithm. The existing font
atlas is uploaded and sampled by GPU text draws. Do not rasterize the graph to a
CPU bitmap or introduce Graphviz image generation, a browser renderer, software
painting, direct Vulkan calls from widgets, or a separate ImGui Vulkan backend.

Reuse `UIRenderer` geometry buffers, texture bindings, scissors, shaders, and RDG
resource declarations. A distinct renderer instance gives the popup its own font
texture and per-frame-slot buffers while sharing the device. `UIContext` sets
`ImGuiBackendFlags_RendererHasVtxOffset`, so large draw lists need no special
handling. No graph-specific shader or new node-editor dependency is required for
the initial implementation.

## Popup integration and lifetime

Create an application-owned `RenderGraphWindow` to connect the native window,
viewport, UI context, view, and capture state. The toolbar action queues an
open/focus request; process window creation and destruction between UI frames.

Reuse `RendererServer::DispatchRenderWorkloads` with `drawScene=false`, the popup
viewport as the presentation viewport, a view built with `RenderView::FromViewport`,
and an overlay that builds the graph-view UI. This provides the existing
`WorkspaceClear` pass, graph compilation, submission, and presentation without
rendering another scene. Follow each dispatch with `NextFrame()`, as the editor
and Play window do.

The editor runs two frames in flight, and each window's `NextFrame()` advances the
shared frame slot and waits for that slot's previous submission. A third window
presenting every iteration therefore reduces CPU/GPU overlap for the scene. Redraw
the popup on demand: after input, focus or resize changes, a new snapshot, and the
few settle frames ImGui needs after interaction. A new capture alone requests one
draw to update the graph and frame metadata; it does not restart interaction
settling. Skip its dispatch otherwise, so an idle frozen viewer submits nothing.

Include pending popup redraw work in the application's idle decision: when the
editor and Play windows are minimized, `WaitEvents` runs after popup redraws settle,
even if the popup is visible. `UIContext::MakeCurrent` selects the editor context
explicitly before popup creation so it restores to an owner that outlives it.
`UIContext` scopes the ImGui context and
input per window, as `RuntimeOverlay` shows; apply the editor theme inside the
popup's context. `ProcessViewportResize` resizes only the main viewport, so resize
the popup with `ResizeViewport` from its framebuffer extent each frame, as
`RuntimeWindow::Render` does, and use the window UI scale for fonts and logical
geometry. Skip drawing at zero extent and resume on restore. The live-resize
callback redraws through `RenderIteration`, so once the popup is part of that
iteration it redraws during OS modal resize loops without re-entering submission.

On close, stop capture publication, destroy the popup renderer and UI context,
then its viewport and native window; `DestroyViewport` waits for outstanding GPU
work through the existing device lifetime rules. Preserve the shared device until
all windows are closed. Partial initialization failures clean up only acquired
resources, report an editor error, and permit retry. A popup failure leaves scene
rendering active when the shared device remains usable; device-wide failure uses
existing application handling.

## Implementation sequence

1. **Capture contract and extraction.** Add `RenderGraph/RDGDebugSnapshot.h` and
   its implementation, registered in `ZenCore/CMakeLists.txt`. Add the extraction
   hook in `ExecuteTransaction`, the armed capture in
   `ExecuteRenderGraph(RHIViewport*)`, and the one-shot request in
   `RendererServer`. Cover complete topology, culled passes, both execution modes,
   owned lifetimes, and accepted/failed submission states before connecting a
   widget.
2. **Capture policy and attribution.** Add `RenderGraphCaptureState` to the editor
   Model. Connect scene generation, applied revisions, editor/Play source
   selection, bounded refresh, freeze, and stale/error states. Route GPU timing
   results by dispatch so non-scene dispatches cannot replace scene timings.
3. **Canvas.** Add `RenderGraphView` and a small layout helper under editor ImGui.
   Implement the pass/dependency view, resource inspector, navigation, search,
   filtering, and layout caching using the existing draw-list path.
4. **Native popup and toolbar.** Add `RenderGraphWindow` under editor App. Register
   the action, draw the toolbar button and both View menu entries, and integrate
   creation, on-demand drawing, resize, input, close, and shutdown into
   `EditorApplication`. Add the new sources to `ZenEditor/CMakeLists.txt`.
5. **Integration verification and documentation.** Verify settings and Play
   transitions, GPU rendering, frame pacing, and failure cleanup. Document the
   button, controls, applied-setting semantics, and capture limitations in
   `ZenEditor/README.md`.

Editor layers depend downward, UI → Services → Render → Model. Keep RenderCore free
of editor/ImGui dependencies, the Model free of GPU and UI types, and application
window ownership in App. Follow the project's C++ rules: one return at the end,
explicit types except iterators, short necessary lambdas, project containers such
as `HeapVector`, shared logic instead of duplication, explicit failure handling
without exceptions, and the repository `.clang-format`.

## Validation and acceptance

Extend existing targets rather than creating an unrelated test framework:
`RenderCoreTest` for snapshot extraction (sources registered in
`ZenSamples/CMakeLists.txt`, beside `RDGMetricsTests.cpp`), `EditorModelTest` for
the capture policy, `EditorInputTest` for layout, hit testing and toolbar fit, and
`EditorRenderingTest` for Vulkan editor integration.

- **Snapshot correctness:** producer/read/write/layout dependencies, multiple
  reasons between one pair, resource versions, culled passes with declared accesses
  and no edges, imported and transient resources, single-list and grouped
  execution, multi-queue groups and aliases, more than 32 passes, and independence
  from metrics options. Verify snapshots remain valid after graph Reset, scene
  replacement, and graph destruction.
- **Capture isolation:** identical `frame_rdg` names from editor, Play, and popup
  cannot cross-contaminate results. The upload graph is never captured; paused Play
  and editor frames during Play do not consume requests. Failed and declined
  dispatches keep the previous snapshot and report an error. GPU timings stay
  attributed to the scene with the viewer open, and metrics settings remain intact.
- **Current settings:** compare PBR and GI graphs, applicable shadow/debug options,
  async-compute preferences and fallback, and GI preparation versus steady state.
  Live edits appear in the next capture; edits awaiting Apply show the notice until
  Apply succeeds; starting Play applies pending edits. The displayed revision
  matches the captured dispatch.
- **Interaction and windows:** the button at the minimum window size at 100% and
  200% UI scale (labeled, then icon form), the View menu entry, repeated
  clicks, pan/zoom/search/selection, freeze/refresh, close/reopen, independent
  input focus, DPI and resize, minimize/restore, editor close, and initialization
  failure cleanup. Exercise editor + Play + viewer together, including paused Play.
- **GPU evidence:** run with Vulkan validation in inline and threaded RHI modes.
  Inspect a GPU capture for `UIRenderSP` indexed draws into the popup swapchain,
  including canvas geometry and text, rather than only a CPU-produced image blit.
  Confirm no validation errors or per-refresh GPU waits/readbacks.
- **Cost:** compare viewer closed, open and idle, open while interacting, and
  frozen, using a large real graph. Check editor scene frame time, capture time,
  layout rebuild count, UI draw cost, popup dispatch count, bounded snapshot memory,
  and resource stability across repeated open/close. Closed viewers and frozen
  viewers without input perform no topology copying or popup submissions. A live
  viewer still captures at four per second while a scene renders. If scene
  rendering is unavailable, a settled popup submits nothing and permits idle
  waiting; resize/teardown may use existing lifetime synchronization.

Build `zen_editor` and the affected test targets with the `x64-windows-msvc-debug`
preset, run targeted CTest coverage, and perform native interaction checks.
Validate macOS window/context/presentation behavior separately when available;
record platform gaps instead of claiming unperformed acceptance.

The feature is complete when Show RenderGraph opens a responsive native window,
the graph corresponds to an identified frame of the active applied configuration,
settings changes refresh it correctly, VulkanRHI renders the entire canvas, and
opening or closing the viewer preserves editor and play-session behavior.

## Known limitations of the first version

- The staging upload graph flushed during a dispatch is not shown.
- Culled passes show declared accesses only; the compiler keeps no edges for them.
- The graph includes host passes (editor UI, picking, previews, Play compositor),
  because they share `frame_rdg` with the scene.
- Status reports acceptance for submission, not GPU completion.
