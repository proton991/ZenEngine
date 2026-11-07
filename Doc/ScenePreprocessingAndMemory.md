# Scene preprocessing and memory

Implemented on 7 November 2026 in ZenEditor, ZenCore/RenderCore and the standalone
ZenAssetProcessor. The requested resident-RAM target covers loading as well as
editing. The final approved one-minute Release ZenEditor check with the five-model
Sponza assembly measured **4.257 GB peak resident RAM** and **3.855 GB at one minute**.
The user authorized two specific native-scene editor checks, then cold-import,
cache-hit and replacement editor checks for the direct glTF fix described below,
as exceptions to the repository's ZenApp-only profiling rule. Other application
performance runs use ZenApp. Private commit and GPU allocation remain above 5 GB; this is not a
universal cap across scenes or memory categories.

## Editor workflow

1. Open **File → Preprocess Models…**.
2. Add one or more glTF, GLB, FBX or OBJ source files using the native picker or
   the path field. Models retain their authored placement.
3. Browse or enter an output folder and a scene name containing letters, digits, `_` or `-`.
4. Choose optional texture compression and import worker count. Lights are preserved;
   runtime light controls belong in Rendering.
5. Choose **Preprocess models**, then **Open processed scene** when it succeeds.

The page owns a `zenassetc` child process. Import/cook memory belongs to that
process. The editor reads its log incrementally, so every progress line counts,
and shows the last 64 KiB. The compiler's per-job and reservation budgets are
three quarters of physical memory, at most 48 GiB, so an oversized model fails
cleanly instead of paging. Cancellation stops the process. Already published
packages remain available; an unfinished build cannot replace the prior assembly
with a partial assembly. The temporary job manifest and log are removed once the
compiler exits; the log tail stays on the page. The editor runs a `zenassetc`
beside its executable when one is present, otherwise the compiler target in the
build tree.

The progress bar follows actual compiler stage events (checking, importing,
optimizing, writing and publishing), not a timer. It reaches 100% only when the
compiler exits successfully. Hover **Import workers** for the CPU-thread and
memory tradeoff; it does not change the sequential model build order.

Output consists of a small `.zenscene` manifest and immutable native packages in
`models/`, rather than one combined package. Cached jobs are reusable. Default
processing uses Zstandard, meshoptimizer transport and vertex-fetch optimization
without changing texture pixels or vertex precision. Optional BC7/BC5/BC4
compression and mip generation use the existing quality guards and fallbacks;
they are not lossless. The external compiler has its own admission budget and
can use more than 5 GB while importing large source models.

## Runtime storage changes

- Native image descriptors retain package handles. Consumers resolve a temporary
  immutable image lease for GPU upload, thumbnail generation or serialization,
  and release it afterward. There is no global resident pixel cache. Validation
  still checks each native image's integrity and contents, and each later lease
  validates its payload. Package data must remain available while the scene is
  open; a retained handle protects against replacing its pathname.
- Static runtime loads transfer geometry into `LoadedScene`, then into
  `RenderScene`. They do not retain an additional canonical graph. Tool callers
  can retain that graph through the existing default `retainCanonicalAsset=true`;
  ZenEditor and ZenApp request runtime-only ownership. Skin/morph scenes retain
  the data needed for deformation.
- Assembly composition converts native vertices directly into the combined
  buffers. Meshoptimizer decoding writes directly into typed native vectors on
  little-endian hosts, avoiding a second expanded mesh byte buffer. Assemblies
  decode geometry largest-first and compose each model's resources in manifest
  order as soon as every earlier model is composed, reducing the overlap of large
  source and destination buffers while preserving node/resource IDs and
  draw-order ties. Only models read ahead of their manifest turn wait.
- Runtime-only source imports and source-cache hits now pass the same mutable
  geometry ownership handoff as native loads. Default runtime glTF imports decode
  and validate at most two textures at once while mesh processing keeps the whole
  worker pool. They retain immutable encoded bytes, resolving decoded pixels only
  for a bounded lease. External images are read by those decodes rather
  than also retaining a parser-wide encoded copy. Runtime source loads await their
  own cache publication, release imported storage, then reopen the package with
  deferred image descriptors before graphics preparation. Canonical tool imports
  retain asynchronous publication. Explicit texture/mesh processing uses the
  existing resident cook path.
- Static native reads discard redundant per-vertex UV rows only when both sets
  are bit-identical to the vertex fields. Sparse, differing, skinned and morphed
  cases keep their arrays. CPU and GPU UV streams now store two floats per entry
  instead of four; their precision is unchanged. Raster, shadow, voxel and ray
  shader declarations use the same new layout.
- Texture views of the same source image share GPU storage. Generated linear and
  sRGB mip chains remain separate because their filtering differs. Authored mip
  chains use compatible views of shared storage. Resource release deduplicates
  shared images.

These changes preserve the native wire format and work with existing packages.
They remove redundant residency; they do not implement camera-driven GPU mip
streaming, mesh LODs or a hard memory eviction budget. Geometry and GPU textures
still scale with scene size. Large animated scenes, multiple simultaneous scenes,
source formats other than the tested glTF, and explicit processing operations
are not covered by the measurements below.

The ownership approach is consistent with avoiding retained CPU asset copies,
as described by [Unity's mesh readability documentation](https://docs.unity.com/en-us/engine/7000.0/script-reference/unityengine/mesh/isreadable).
[Unreal's texture streaming overview](https://dev.epicgames.com/documentation/unreal-engine/texture-streaming-overview-for-unreal-engine)
describes the visibility-driven GPU residency control that would be needed for a
separate GPU-memory cap. That system is not implemented here.

## New Sponza measurements

The initial performance runs below use the real Release **ZenApp.exe**, RTX 5080, PBR, threaded
RHI, async compute disabled, ray tracing disabled and validation disabled.
Actual framebuffer and render extents are 1280 × 720, windowed. Inputs are the
existing desktop assembly combining main, curtains, ivy, trees and candles:
21,017,819 vertices, 32,373,689 triangles, 139 renderable nodes and 87 textures.
The assembly submits 24 lights; the candle instance retains its 10,000 light
definitions with submission disabled. This is not a 10,024-light benchmark.

GB below means 1,000,000,000 bytes. Working set is process resident RAM; private
commit is recorded separately and must not be interpreted as resident RAM or
reduced by subtracting GPU allocations.

| Run | Frames | Peak working set (GB) | Peak private commit (GB) | Whole process wall time |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 120 | 11.584 | 21.671 | 16.404 s |
| Optimized, first | 120 | 4.647 | 13.785 | 21.619 s |
| Optimized, repeat | 120 | 4.647 | 13.789 | 21.598 s |
| Optimized, sustained | 12,000 | 4.647 | 13.974 | 59.814 s |

The sustained run's samples between 25 seconds after launch and two seconds
before exit range from **3.740 to 3.804 GB** of working set. Loading peak is
4.33 GiB when expressed in binary units. The repeated 120-frame run lowers peak
resident RAM by approximately 60%, at a roughly 5.2-second increase in process
wall time. These are local samples, not cold-disk timing guarantees. Deferred
validation and consumers reread/decode image data instead of retaining it.

The 120-frame optimized run reports 10,054,283,216 bytes of peak VMA GPU allocation
(9,920,065,488 device-local) and 134,217,728 bytes of peak host upload allocation.
GPU residency and process private commit exceed the requested numerical limit;
no claim is made that all memory categories are below 5 GB.

Every baseline and optimized capture, including the 12,000-frame capture, has
the same SHA-256:

`114de213ce8522f7b4e3a07c5b0f65a39cb3e46ffb3f9581ac5ea0b296aa616b`

This proves pixel parity for the tested camera, renderer and already-cooked
assets. It does not establish equivalence for every camera or lossy cook option.
The five model packages remain approximately 2.624 GiB on disk; this runtime
change adds no duplicate package payloads and requires no recook.

### Reproduction and evidence

Evidence is under `build/asset-memory-20261107/`. Each run's JSON records executable
identity/SHA-256, exact arguments, setup and all five input hashes, process
measurements and a 20 ms sample timeline. Logs retain actual render configuration
and GPU allocation counters. `Data/engine.cfg` was not edited for profiling.

```powershell
build/x64-windows-msvc-release/bin/ZenApp.exe `
  --setup=E:/Dev/ZenEngine/build/sponza-multimodel-integration/courtyard.zenrender `
  --frames=120 --gpu-culling=1 --disable-rt --width=1280 --height=720 `
  --fullscreen=0 --rhi=threaded --async-compute=0 --validation=0 --vsync=0 `
  --fixed-step --no-ui `
  --capture=E:/Dev/ZenEngine/build/asset-memory-20261107/optimized-2.ppm
```

The sustained run changes `--frames` to `12000` and the capture filename to
`optimized-sustained.ppm`. `measure.py` records the process metrics with that
command; it does not substitute another executable for ZenApp.

## Verification and publication

- All 43 standalone processor Release CTest suites passed. Coverage includes
  deferred lease lifetime, integrity/descriptor errors, cancellation, allocation
  budgets, serialization, typed mesh decoding and bitwise UV compaction.
- Engine Release targets `ZenApp`, `zen_editor`, `SceneAssemblyTest`,
  `RenderCoreTest`, `EditorModelTest`, `AssetPreprocessorTest` and
  `EditorRenderingTest` built. All five listed test suites passed, including the
  native Vulkan editor suite. The preprocessing suite passed again after adding
  cancellation coverage and finishing the UI.
- The preprocessing service test invokes the actual compiler for one and two
  models with spaces in their paths, checks cache reuse and verifies previous
  assembly preservation after failure and cancellation.
- UI checks exercised the File entry, native source picker, second source path,
  output folder, preprocessing and opening the resulting two-model scene. The
  full Sponza assembly also opened in ZenEditor; camera navigation and deferred
  RGBA/BC texture thumbnails were checked. These are functional checks, not an
  editor performance profile.
- Changed C++/GLSL formatting, whitespace and the engine no-exceptions check pass.
- The preprocessing panel follow-up passed all 43 processor suites and the engine
  `UIDrawPacketTest`, `UIPlatformTest`, `AssetPreprocessorTest` and
  `EditorRenderingTest` suites. Live UI checks verified the output folder picker,
  worker tooltip, processing progress through completion and the persistent
  **GI ready** indicator in a narrow scene viewport. Preparation/refinement and
  unavailable-state status coverage also passes in the UI tests. Logs are under
  `build/preprocess-panel-20261107/`.

Processor changes were committed and pushed from `E:/Dev/ZenAssetProcessor`:
`8cc3758` (deferred images), `9e9fa33` (direct mesh decoding), and `4692910`
(bit-identical static UV compaction). ZenEngine's submodule consumes `a5d4506`
on processor `main`. Its ancestors retain encoded glTF images and add optional
compiler and resource-write progress events; `a5d4506` adds a glTF image decode
limit that is separate from the worker count. The submodule has no local
processor edits.

A later review pass resolves each deferred image once per scene load, releases
an exclusive lease level by level while copying it, makes textures mutable only
for mixed view formats, and frees an unfinished distance field build after a
switch to HW RT. It also fixes `HeapVector::push_back` of one of its own elements
during growth. Debug CTest passes all 25 suites; logs are under
`build/review-fixes-20261107b/`. Runtime source imports first capped the whole
importer at two workers; they now keep every worker and limit only concurrent
image decodes to two (`SceneImportContext::maxConcurrentImageDecodes`).

## GI and editor follow-up

Hardware-only GI used to construct and refine the Compute distance field even
though its preparation was absent from the active-backend status. This consumed
temporary GPU storage and submitted background work while the UI said ready.
It now builds the field only when a Compute feature needs it. An existing field
is retained for a warm backend switch and refreshed against committed edits on
return to Compute. A switch to HW RT while a scene or local build is unfinished
drops that build and frees its construction scratch once its GPU work retires;
selecting Compute again restarts it. Compute construction now targets a 16 ms CPU frame with at
most four cycles per graph, instead of 250 ms and 64 cycles. The adaptive capacity
calculation honors the explicit 1 GiB scratch cap. These are scheduling/capacity
changes; field resolution, coverage and mesh/texture detail are unchanged.

The GI status shares the FPS/frame-time readout above the scene image, with clipped
text and a tooltip in narrow docks. It uses no extra row and is not drawn over the
rendered image. Its reserved width stays constant during preparation/readiness changes.

The following real ZenApp GI runs use the five-model assembly, Vulkan on RTX 5080,
1280 × 720 framebuffer and render extent, windowed, threaded RHI, async compute
off, validation off, VSync off, fixed step, 240 frames and the same courtyard setup.
Working set includes loading. Wall time includes startup and shutdown, not just
the rendered frames. Each is a local sample rather than a cold-disk guarantee.

| ZenApp run | Peak resident GB | Peak private commit GB | Peak GPU allocation GB | Process wall time |
| --- | ---: | ---: | ---: | ---: |
| Hardware GI before field-policy fix | 4.647 | 19.236 | 15.086 | 38.663 s |
| Hardware GI, field on demand | 4.647 | 17.137 | 13.055 | 23.404 s |
| Hardware GI, final geometry load order | 4.147 | 17.140 | 13.055 | 23.872 s |
| Compute GI before bounded preparation | 4.647 | 18.465 | 14.421 | 35.904 s |
| Compute GI, bounded preparation | 4.647 | 16.113 | 11.883 | 25.480 s |

The 240-frame bounded Compute run is still preparing; its shorter wall time is
not a completed-GI speedup. A separate 2,000-frame convergence run completed all
24 scene/local generations with no retries or unavailable cells and with each
reported scratch total within 1 GiB. Compilation overlapped part of that long
run, so it is convergence/resource evidence, not a clean timing comparison.
ZenApp's new `frame_cpu_ms` profile field measures CPU work in `Frame()`, not GPU
execution or presentation interval; `gi_preparing`, `gi_refining` and allocation
counters make continuing preparation visible in the saved profile.

The user's three-model `E:/Dev/ZenAssets/NewSponza_a_c.zenscene` also completed
the hardware-GI check: 1.411 GB peak resident RAM and 7.948 GB peak GPU allocation.

The two explicitly approved Release ZenEditor checks used the same five-model
assembly, `--mode=gi --rhi=threaded`, isolated settings, a maximized 3840 × 2088
framebuffer, UI scale 1.5 and hardware GI. Resident memory was sampled every 20 ms,
including the OS process peak counter. Both ran for one minute from launch.

| Editor check | Peak resident bytes | Resident bytes at one minute |
| --- | ---: | ---: |
| Before geometry load-order fix | 5,013,032,960 | 3,917,438,976 |
| Final | **4,256,587,776** | **3,855,044,608** |

The final sample's private commit was 17,990,098,944 bytes. It is recorded
separately and is not subtracted from GPU allocations or presented as resident
RAM. The UI showed ready above the image; its observed approximately 144 FPS
readout is not a sustained frame-time benchmark. No >10 GB resident working set
reproduced in these fresh native-scene runs. Cold source importing and repeated
scene replacement remain outside this memory check.

Exact executable identities, arguments, setups, input hashes, memory timelines,
logs and test output are in `build/gi-memory-20261107/`. Editor records are
`editor-memory.json` and `editor-memory-final.json`; final ZenApp loading is
`ordered-hardware.json`. The editor framebuffer is recorded in its initialization
log; its exact offscreen scene-image extent was not logged. The final 120-frame
PBR image in `build/asset-memory-20261107/ordered-geometry-parity.ppm` retains the
same SHA-256 shown above, despite the changed physical geometry order.

Release and Debug `ZenApp` and `zen_editor` builds succeeded. Assembly composition
tests cover unequal model sizes, geometry remapping, resource/instance order and
camera/environment selection. The native distance-field suite passed 76 tests;
four optional lantern experiments were skipped. Native source-cache ownership,
renderer switching, UI status and RenderCore regression results are retained
alongside the runs. No processor code or submodule revision changed in this follow-up.

## Direct glTF loading and cache progress follow-up

The reported Release editor was opening the original
`E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf`. The approved inspection of that
live process recorded 11,513,061,376 peak working-set bytes and 5,721,497,600 current
working-set bytes. This was an existing user session, not a controlled launch or
a matched timing baseline. Its 72 source textures are all 4096x4096: their decoded
RGBA8 base levels alone total 4,831,838,208 bytes. The source import retained those
pixels and the engine adapter copied them while asynchronous cache serialization
could still own the imported scene. Native-package-only checks had missed this path.

The processor changes in published commits `7a74c1e` and `b1bc7c4` add opt-in
encoded image ownership and bounded external-image reads. Default runtime glTF
opens enable it, decoding at most two images at once (`a5d4506`) while meshes use
every import worker. Each decoded lease is released
after use; the immutable encoded bytes do not borrow a mutable source file.
Runtime opens await only their own cache write and reopen its deferred native
images before preparing graphics resources. Tests compare every decoded pixel
and mip, verify lease release and cancellation, and preserve geometry through
cold and cached runtime loads. Explicit processing operations retain the existing
resident cook path; this is not a general admission or eviction budget.

Interactive source caches now use lossless Zstandard level 3 instead of level 12.
The loading dialog switches from source-file counts to actual resources written,
shows an animated preparing/finalizing phase, and no longer displays `74 / 74 files`
through cache writing and graphics preparation. The Sponza cache stage measured
51.419 seconds. Its package occupies 2,411,366,604 bytes, compared with
2,107,087,334 bytes for level 12. Texture resolution and pixels are unchanged.
The preprocessing compiler's compression policy is unchanged.

The final Release executable SHA-256 is
`b381581aaeda505ca96ab3dfd6959358d673154f8268ac9a0e207e50cd69432f`.
These runs use the real `build/x64-windows-msvc-release/bin/zen_editor.exe`, with:

```text
--scene=E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf
--settings-dir=E:/Dev/ZenEngine/build/gltf-memory-20261107/editor-settings
--mode=gi --rhi=threaded
```

| Release editor check | Peak resident bytes | Resident bytes 60 seconds after opening | Observed time to open |
| --- | ---: | ---: | ---: |
| Fresh source cache | 3,635,318,784 | 929,886,208 | 115.65 s |
| Valid cache, new process | 971,423,744 | 868,061,184 | 23.12 s |
| Replace the loaded scene with the same cached glTF | 1,559,265,280 | 1,089,282,048 | UI-driven; no end-to-end timing claim |

Sampling uses `GetProcessMemoryInfo` every 20 ms plus the OS lifetime peak
counter. The cold run includes the entire import, cache publication, graphics
preparation and one minute after publication; it is cache-cold, not OS-disk-cold.
GI reached `hardware_ready`. The GPU is an RTX 5080, backend Vulkan, threaded RHI,
async compute off, windowed/maximized framebuffer 3840x2088 and scene render target
2360x1264. The replacement check restored/maximized the window through UI; its
old render target was 872x384 during preparation, returning to 2360x1264 after
publication. It is not a fixed-extent timing comparison. The editor was also
used for camera navigation during the cold run's editing interval.

Private commit at the end of those checks was respectively 9,967,316,992,
9,910,116,352 and 10,049,261,568 bytes; these are not resident-memory results.
The UI's GPU allocation readout remained about 8.5 GiB. No GPU-memory cap is claimed.
The earlier intermediate `cold-gltf` probe exceeded 5 GB and timed out before
scene publication; it is not a successful acceptance run.

Exact executable/configuration identities, all 74 source-file hashes, commands,
logs and memory timelines are in `build/gltf-memory-20261107/`: `cold-gltf-v2.json`,
`warm-gltf.json` and `replace-gltf.json`. `Data/engine.cfg` was not edited.
All 45 processor CTest suites and the Release engine suites `SceneAssemblyTest`,
`EditorModelTest` and `EditorRenderingTest` passed. Release and Debug editor and
ZenApp builds succeeded. The UI was checked during source import, cache-resource
progress and graphics preparation; GI/FPS remained right-aligned above the scene.
