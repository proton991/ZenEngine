# ZenEditor Run, Pause and Quit implementation plan

## Current design: one scene and an in-process play window

30 October 2026: the user replaced the separate-process toolbar design with a
single scene that moves between the editor and a play window. This section
supersedes the historical process-launch design below.

- **Run:** apply the valid pending rendering draft, then create a second native
  window and presentation viewport on the existing render device. Borrow the
  existing SceneRendering, SceneRenderTarget and camera. No second scene, asset reload,
  prepared-package export, child process or renderer-resource release is needed.
- **Editor:** render the responsive shell with a Scene placeholder. Disable scene
  edits/navigation and mesh previews while Play owns scene rendering. Read-only
  inspection and Output stay usable. A minimized editor must not pause Play.
- **Pause/Resume:** stop/continue the scene clock, animation, camera input and
  scene/GI rendering. Continue presenting the retained image and debug controls.
  Resize while paused fits that image; Resume updates the exact render extent.
- **Quit/close:** retire play UI and presentation resources before destroying the
  native window, then resume rendering the same scene in the editor. Preserve
  camera and animation state. Editor shutdown also destroys its play window.
- **Presentation:** RendererServer accepts a destination viewport. Each window's
  render graph is submitted independently; scene work occurs only for Play.
  Default render sizing follows exact framebuffer pixels. Fixed sizing preserves
  aspect ratio and may intentionally letterbox.
- **Diagnostics:** controls, state/frame/time, dimensions, scene CPU statistics,
  shared GPU memory, GI/fallback status and an independently clearable view of
  editor log entries. Log synchronization copies only new entries. The popup
  omits per-pass GPU timings because the shared executor's latest completed
  sample can belong to either window. Both titles identify Debug/Release.
- **Standalone:** ZenApp remains usable from its setup file. Editor IPC,
  process/session launching, immutable package pins and preview-resource release
  have been removed. The play window shares the editor's crash/device-loss boundary.

Presentation, scene clock and navigation live in `ZenApp/RuntimeWindow.cpp`
(`ZenAppRuntime`); optional diagnostics live in `ZenApp/RuntimeOverlay.cpp`
(`ZenAppRuntimeUI`). Both the editor popup and standalone `ZenApp.exe` use these
components, without runtime dependencies on editor or source-import tooling.
`EditorApplication` coordinates its two windows and lends the existing scene,
render target and camera. The popup title is `ZenApp [Debug/Release]`.
The standalone host owns its resources and loads a prepared setup/package;
an export/package UI remains future work.

Windows Vulkan tests cover both RHI modes, resource identity,
PBR/depth/GI image comparison, pause, resize, minimize, repeated sessions and
cleanup. Verification is Windows only, as requested.

Review corrections verified on Windows (30 October 2026): Debug and Release builds
of the editor, ZenApp, demo and affected suites pass. Each configuration passes
728 RenderCoreTest cases, 75 EditorRenderingTest cases, 48 EditorModelTest cases,
26 EditorInputTest cases and 15 RuntimeLaunchTest cases. Nine renderer cases and
two editor rendering cases remain intentionally disabled. The renderer fixture's
unreleased synthetic buffers and retained shader metadata were also corrected;
both complete renderer runs report zero live allocator bytes at shutdown.
Native rendering checks report no Vulkan validation errors. The legacy camera
test checks exact float equality with the original position/target lookAt path;
the separate angle-based navigation path retains pole handling. Standalone
Debug/Release captures in both RHI modes match the saved pre-review image byte
for byte. The Sponza demo also completes PBR and GI captures in both RHI modes
without validation errors; PBR captures match exactly. The two GI captures differ
in 22 of 691,200 color channels by one 8-bit value, so this is a smoke check rather
than a claim of bit-exact GI output across RHI modes. The full historical GI
capture matrix was not rerun. The new log-view test verifies incremental
synchronization, retention, and Clear independence. Layout proportions, title labels and UTF-8 command-line
support remain separate concerns from these corrections; no commits were made.

Shared ZenApp follow-up verification (30 October 2026): Debug editor/runtime builds,
all 71 EditorRendering tests and all 12 RuntimeLaunch/Camera/Clock/Protocol tests
pass. The camera test covers both drag directions for look, pan and orbit, plus
fly modifiers and wheel zoom. An actual Debug toolbar Run opens `ZenApp [Debug]`
in the editor PID, Space pauses, the diagnostics Resume continues, and Quit
restores the original scene. No Vulkan validation errors were reported.
A Release standalone build with editor, asset tools and diagnostics UI disabled
also runs; its fixed-step capture matches Debug/UI-on and the previous standalone
capture byte for byte. The pre-existing Release editor session was left running;
its executable was not replaced by this follow-up. Standalone Debug was also
checked interactively: start-paused holds frame 1 at 0.000 seconds, Space resumes,
and Escape exits cleanly.

Earlier Windows verification for the in-process implementation (30 October 2026):
Debug and Release editor/ZenApp builds pass. All 71 EditorRendering tests pass;
the four targeted play tests also pass after the final controller correction.
Manual Debug checks used the editor toolbar on New Sponza with GI: Run opens
Play in the same PID with no ZenApp child, local Space and editor Pause/Resume
work, paused frame/time remain fixed through maximize, Resume fills the new
framebuffer, and Play keeps advancing with the editor minimized. Quit returns
the scene without reloading. Release UI checks also verified closing Play restores
a minimized editor. EditorInputTest and EditorPreviewTest pass. Vulkan validation
reported no errors in those manual runs.
The displayed allocations were 9666 MiB in the large editor view and 8387 MiB
at 1440x900 Play; those different extents explain the difference and are not a
like-for-like performance measurement.

## Superseded design

The original separate-process proposal was replaced by the shared-scene design
above. Its IPC, child-process lifecycle and package-pin implementation was removed
after review on 30 October 2026. Standalone packaging uses the existing asset writer
and setup-file loader; it does not require an editor session or an inherited pipe.
