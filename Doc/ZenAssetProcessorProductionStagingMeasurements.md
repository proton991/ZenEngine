# Production scene staging measurements

24 October 2026. Production scene publication now reports staging-pool occupancy,
native upload-buffer allocation lifetimes and CPU load/publication costs. The
final campaign completed **80 processes and 160 scene publications**, using
inline and threaded RHI execution with accepted transfer-queue work in every
publication. This closes the missing production staging-memory observation for
these workloads. It does **not** establish GPU upload duration, rendering
throughput or universal compression benefit.

The later [production GPU timing campaign](ZenAssetProcessorProductionGPUTimings.md)
adds direct transfer-queue timestamp measurements for these four workloads.
The CPU figures below remain the original campaign's measurements.

## Implementation and accounting

[StagingBufferManager](../ZenCore/Include/Graphics/RenderCore/V2/StagingUploadQueue.h)
exposes current and peak pool capacity, occupied bytes including padding and
unretired ranges, native allocations owned by the pool, block counts, allocation
flush requests, and CPU wait observations. Reading or resetting statistics does
not submit, reclaim or wait. Reset includes already live blocks in the new
interval. Unknown backend allocation sizes remain explicitly unavailable.

`RHIBuffer::GetAllocationSize()` reports native resource allocation extent;
Vulkan supplies `VmaAllocationInfo.size`. This includes resource alignment, not
allocator block commitment or physical residency. Releasing a pool owner may
leave command/import references alive, so summing only pool-owned buffers can
understate retained native memory. The Vulkan allocator therefore separately
counts **CPU-write transfer-source buffer allocations through actual native
destruction**, including such retained references. Failed allocation/mapping
attempts do not enter those counters. VMA pool commitment is a separate metric.

The production sample emits `SCENE_UPLOAD_METRICS`, `SCENE_UPLOAD_NATIVE` and
`SCENE_LOAD_TIMING` JSON records with `--gpu-memory-stats`. The diagnostic
`--scene-load-repeats=N` option repeats the normal `LoadModel`/`PublishModel`
path before rendering, retaining one published scene while replacing it.
Transfer serials before/after publication confirm accepted work; a capability
message alone is insufficient, especially in threaded mode.

The implementation does not change upload routing, pool budgets, resource
retirement or scene publication policy. Existing `PrepareForSceneReplacement`
waits remain part of publication. Separate transfer queues and GPU timeline
dependencies do not imply that scene publication itself is nonblocking or that
transfer overlaps rendering in this experiment.

## Method

The [runner](../../ZenAssetProcessor/tools/validation/asset_processor_production_upload.py) creates raw desktop
packages with offline mips and corresponding compressed packages, without
transport or geometry compression. It covers 1024×1024 constant-color BC7,
1024×1024 flat-normal BC5, a 4096×4096 BC7 pressure fixture, and the real
NormalTangentTest BC4 role-split case. The controlled images isolate storage and
upload behavior; they are not representative texture-quality coverage.

For each raw/cooked pair and RHI mode:

- One validation-enabled load and capture checks publication, counters and
  rendering. All **16 validation processes** passed without validation errors,
  `VUID-` or `SYNC-HAZARD` messages.
- Three timed fresh processes/devices alternate raw/cooked order. OS file and
  driver caches are uncontrolled; “fresh” does not mean cold storage.
- One timed process performs an initial unreported load followed by five
  measured model replacements. This gives **48 fresh and 80 warm samples**.
- Every process ends with zero native host-upload allocation bytes, zero VMA
  allocation leaks and zero engine memory leaks. Counters are checked against
  pool capacity and native allocation bounds.

Source/dependency hashes, package/binary identities, profiles, command lines,
per-process working-set/private-commit samples, every timing sample, and
min/median/max summaries are retained. Non-texture resource payloads and
material semantics after resolving texture IDs match between each pair.
Shaders, helpers and binaries remained unchanged during the final campaign.
`Data/engine.cfg` was restored byte-for-byte; no capture lock remains.

Hardware: Ryzen 9 9950X3D, 16 cores/32 logical processors; RTX 5080, driver 617.42.
Build: Windows MSVC Release, two cook workers. Captures are 640×480, fixed-step,
one raster frame, RT disabled. The hash-identical frozen `7zFM.exe` selects the
machine's existing RTSS no-hook profile. Global overlay settings were unchanged.
All sampled native transfer serials advance; the driver reports a dedicated
transfer queue and enabled asynchronous submission dependencies.

## Results

Threaded-mode medians follow. Native peaks include retained references and are
measured per fresh process; MiB means 1,048,576 bytes. CPU publication includes
resource setup, upload submission, production waits and scene replacement.

| Workload | Native upload peak MiB, raw → cooked | Fresh publication ms, raw → cooked | Warm publication ms, raw → cooked | Fresh pool waits, raw → cooked |
| --- | ---: | ---: | ---: | ---: |
| 1024² BC7 | 29.333 → 24.000 | 33.263 → 30.056 | 17.492 → 16.960 | 0 → 0 |
| 1024² BC5 | 29.333 → 24.000 | 31.002 → 31.059 | 16.456 → 15.814 | 0 → 0 |
| 4096² BC7 pressure | 93.333 → 45.333 | 55.594 → 37.256 | 48.583 → 21.220 | 2 → 0 |
| NormalTangentTest BC4 role split | 50.667 → 50.667 | 50.117 → 50.691 | 36.472 → 37.728 | 1 → 1 |

The pressure fixture demonstrates why both memory counters matter. A raw run
owns an 89,478,496-byte native pool buffer at its peak, while native upload
allocations reach 97,867,104 bytes. A block leaving pool ownership does not
guarantee immediate native destruction. BC7 reduces the observed peak and
avoids pool-pressure waits here. The role-split case shows neither a staging
peak reduction nor an observed publication-time benefit. These bounded samples
do not establish a statistically significant small timing difference or a
general performance rule.

Warm native peaks are cumulative over the whole six-load process; they are
recorded once per sequence, not presented as independent per-iteration peaks.
The pool's per-publication reset metrics remain available for individual loads.
The inline-mode samples and all ranges are in the
[summary](../build/asset-processor/m5-production-staging-final/summary.json).
Controlled raw/cooked captures are exact in both modes. NormalTangentTest differs
by at most one 8-bit channel level, consistent with its lossy BC4 role.

## Verification and limits

- **720 enabled RenderCore tests pass**, with nine existing disabled cases.
  Three new tests cover padding/retirement/reuse/reset, allocation failure and
  unknown sizes, and failed waits preserving occupancy until completion.
- **50 Vulkan tests pass**, covering native allocation lifetime, failed native
  allocation/mapping accounting, and existing inline/threaded upload ordering,
  rejection, dependencies and readbacks.
- The renderer and tests build successfully; changed C++ passes clang-format.
  The first campaign is retained separately. The final campaign adds accepted
  serial checks and is the authoritative data above.

Pool wait duration is CPU time inside `WaitForSubmittedAllocations`; it excludes
the preceding flush and other publication waits. Publication time is also CPU
wall time. Neither is a GPU timestamp interval. Native counters exclude driver
private allocations and physical residency; whole-process VMA commitment is
reported separately. Instrumentation overhead is included in these samples.
Source parsing uses the normal synchronous sample load path; this experiment
tests production GPU submission/publication, not background importer scheduling.

Direct production GPU upload intervals, frame/overlap benefits, broader assets,
atlas comparisons, other devices, final Debug/Release release matrices and the
remaining lossy visual gates are still required by the
[implementation plan](ZenAssetProcessorImplementationPlan.md).

## Reproduction and evidence

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_production_upload.py `
  --renderer build/asset-integration-release/bin/scene_renderer_demo.exe `
  --processor E:/Dev/ZenAssetProcessor/build/processor-port/Release/zenassetc.exe `
  --reference-config build/asset-processor/fresh-verification/visual-0-source.cfg `
  --role-split-source E:/Dev/glTF-Sample-Assets/Models/NormalTangentTest/glTF/NormalTangentTest.gltf `
  --output build/asset-processor/new-production-upload-run
```

Use a new output directory. Evidence:
[complete run](../build/asset-processor/m5-production-staging-final/results.json),
[derived checks and summaries](../build/asset-processor/m5-production-staging-final/summary.json),
[RenderCore XML](../build/asset-integration-release/m5-production-staging-rendercore.xml),
[Vulkan XML](../build/asset-integration-release/m5-production-staging-vulkan.xml), and
[source/binary evidence audit](../build/asset-integration-release/m5-production-staging-evidence/final-audit.json).
