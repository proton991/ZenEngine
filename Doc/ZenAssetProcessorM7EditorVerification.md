# M7 editor profiles and explicit recooking

## Scope and behavior

The editor uses the standalone `ProcessingSettings` and the same `CookScene`
integration as source-enabled tools. Reference is the session default and passes
default settings with every operation disabled. Desktop is explicit: selecting
the profile alone enables no processing. The Assets panel offers vertex-fetch
optimization, mip generation, BC7, BC5 normals, BC4 occlusion and BC6H environment
toggles, plus the public numeric quality and geometry settings. Invalid settings
are rejected with the library validation message.

Draft settings apply to the next source open or explicit **Recook from Source**.
The queued request owns its settings snapshot; edits to the next draft do not
change a running job. Applied settings change only when the editor publishes the
replacement scene. Explicit `.zenasset` files load as stored, clear source recook
provenance, and do not run the current desktop draft against package data.

Recook preserves the applied source's import and cache policy, bypasses an existing
cache hit, and awaits its own publication result on the source worker. Ordinary
source opens retain their existing background cache publication behavior. There
is no UI-thread wait or global cache drain in the interactive path. The editor
shows cooking/cache-saving progress and keeps last-attempt diagnostics separately
from the active scene's diagnostics.

## Publication and cancellation boundary

The source worker receives a future for its cache publication. Explicit recooks
recheck source/dependency hashes and missing preferred inputs after serialization,
immediately before replacement. A same-size, same-timestamp source edit made while
the temporary package is being written is rejected as `SourceUnavailable` with
that input's path; the previous package survives. Both request and shutdown
cancellation are polled by the existing streaming hash loop every 4 MiB. An explicit recook
does not yield a candidate scene when publication fails. A valid complete package
is atomically installed before graphics preparation; cancellation or graphics
failure still keeps the old active scene. Cancellation before the package commit
preserves the previous package and removes the temporary file. Cancellation after
that commit cannot undo the refreshed cache, even when the editor keeps the old
active scene. This follows the writer's existing durability boundary.

`forceRecook` is scheduling intent and is excluded from the cache identity. Source
inputs, import settings, processing settings/profile, processor/build identity and
schema continue to determine cache validity. Changing processing settings chooses
a separate cache; recooking identical settings replaces the same key. A package
request or disabled-cache request with force-recook intent fails explicitly.

## Implementation map

- `SceneImporter.h`: appended engine-only cooking/saving stages and force-recook
  intent; standalone progress enums and processing records are unchanged.
- `AssetTools/Source/SceneAssetCache.cpp`: per-job publication result, forced hit
  bypass, existing asynchronous normal path, explicit publication diagnostics.
- `SceneSourceImporter.cpp` and `SceneTools.cpp`: request validation, cook-stage
  reporting, and stored-package routing.
- `SceneProcessingState.h` and `EditorController`: draft/queued/applied settings,
  source provenance, recook action, failure/cancellation transaction.
- `SceneProcessingWidget`, `AssetsPanel`, `EditorWorkspace` and
  `SceneLoadingDialog`: settings, action, progress and diagnostics.

## Validation

Release build completed for `CommonTest`, `EditorInputTest`, `EditorRenderingTest`
and `zen_editor` with two parallel jobs. The accepted standalone batch/publication
header snapshot was included in the final increment.

| Check | Result | Evidence under `build/asset-integration-release/` |
| --- | --- | --- |
| Source/cache/package/loader CPU regressions | 34/34 pass | `m7-editor-common-final-tests.log` and `.json` |
| Headless ImGui/input regressions | 23/23 pass | `m7-editor-input-tests.log` and `.json` |
| Inline/threaded recook, actions and failed texture upload | 6/6 pass | `m7-editor-rendering-final-tests.log` and `.json` |
| Force-recook publication/cancel/source-change regressions, repeated five times | 15/15 executions pass | `m7-editor-recook-repeat-tests.log` (JSON contains the last iteration) |
| Build | Passed | `m7-editor-final-build.log`, `m7-editor-test-build.log` |
| Broader native editor regressions, including mixed formats and cancellation | 10/10 pass | `m7-editor-broad-final-tests.log` and `.json` |
| Installed CLI batch versus editor source recook, inline/threaded | 2/2 pass | `m7-batch-editor-parity-tests.log` and `.json` |
| Full RenderCore after removing the debug cube's source dependency | 707/707 pass | `m7-cube-rendercore-full-tests.log` and `.json` |

All completed suites reported zero CPU leaks. The broader ten-test native run and
the two batch parity runs reported zero VMA leaks and no Vulkan validation errors.
The earlier executable hashes are recorded in `m7-editor-binary-identities.json`;
the cube/parity build hashes are in `m7-completion-identities.json`. `Data/engine.cfg` was never
modified and remains SHA256
`5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`.

An earlier CPU run (`m7-editor-common-tests.log`) had one assertion failure because
the expected Windows path used backslashes while the correctly rejected input
was reported with normalized forward slashes. The test now compares a generic
path. The production mutation rejection and preserved prior package already
passed in that run.

A broader ten-test native run (`m7-editor-rendering-tests.log`) passed the new
recook cases, then aborted in the existing mixed-format load regression: a debug
`RenderObject` tried to load source `Box.gltf` through runtime-only `LoadScene`.
The runtime now creates its small built-in cube directly. It retains 24 face-split
vertices, 36 outward-wound indices and unit bounds, and handles partial buffer
allocation failure. The debug voxel path needs only those buffers and bounds;
source parsing and unused material/node storage are removed from `RenderObject`.
Two focused tests check staged geometry, winding, failed allocations and retry.
The same broader ten-test filter subsequently passed, as recorded above.

Focused regressions cover valid-cache hit bypass and immediate publication,
settings-preserving cache identity, publication failure, cancellation during an
actual large temporary write, queue snapshot isolation, actual BC7 runtime
textures, cancellation at all editor stages, retained active render state,
package-as-stored behavior and cancellable cooking/saving modal states.

The installed `zenassetc build` compiled `recook_textured.gltf` with vertex-fetch
optimization, mip generation, BC7, Zstandard and meshoptimizer transport. Both
editor execution modes recooked that source with the same processing settings.
Canonical engine-scene serialization matched byte for byte, including actual BC7
payloads, retained RGBA normal pixels and reordered geometry. Profile/settings and
fallback diagnostics also matched. Each test then loaded the batch package as
stored and rendered a frame. This checks this fixture's runtime records rather
than claiming identical compressed container bytes or broad rendered-image parity.
The manifest and CLI report are under `build/asset-processor/m7-editor-batch/`;
the compiler SHA256 is
`4bc2a45b9bb453f265094df42af719f9aab479af40a6f28af408038be93f8285`.
See [batch verification](ZenAssetProcessorM7BatchVerification.md) for build-system
and freshness coverage.

## Limits

The later [BC7 performance/native checkpoint](ZenAssetProcessorBC7PerformanceVerification.md)
extends `recook_textured.gltf` with a filtered 8x8 clearcoat normal map. The rebuilt
editor requires its four-mip BC7 output and matches the updated installed CLI
batch package. The measurements above describe the earlier fixture revision.

Settings are session state, not persisted user presets. The UI exposes currently
supported desktop operations; atlas and portable-profile controls are not added.
The current package/source readers and platform-format checks remain responsible
for supported-format failures and per-image fallbacks. No renderer or GI algorithm
changes are part of this slice.
