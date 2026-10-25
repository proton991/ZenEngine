# Production scene upload GPU timing

24 October 2026. The production staging path now has measured GPU intervals on
the dedicated transfer queue. The campaign completed **80 processes, 160 scene
publications, 320 staging graphs and 2,420 timed upload nodes**. Every selected
node had an available result; no timing was dropped, omitted or substituted
with CPU time. These measurements complement the
[production staging-memory campaign](ZenAssetProcessorProductionStagingMeasurements.md).

## Implementation and measurement boundary

The Vulkan backend already supported timestamp scopes with deferred readback.
Its command-buffer query reset requires a graphics/compute-capable queue, so
dedicated transfer scopes previously reported `unsupported`.
[Vulkan's command reset contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdResetQueryPool.html)
does not permit that command on a transfer-only family.

The backend now queries and optionally enables the Vulkan 1.2 `hostQueryReset`
feature through its existing feature-negotiation path. Dedicated transfer
recordings use `vkResetQueryPool` once before writing their first timestamp.
Graphics/compute queues retain their command-buffer reset. A device without the
feature, a required function pointer, or valid timestamp support continues to
report `unsupported`; uploads still use the normal route. The feature is core
in the engine's minimum API version, so its former extension name is not
required. See the [feature contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceHostQueryResetFeatures.html).

Each recording exclusively owns its query pool. Pools become reusable after
submission completion and readback, or after unsubmitted recordings are
discarded. Host reset does not race a submitted query or another thread's
query read/reset. These are requirements of
[`vkResetQueryPool`](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetQueryPool.html).
Readback still uses availability bits and never `VK_QUERY_RESULT_WAIT_BIT`.
Pool capacity, submission routing, staging retirement and publication waits
are unchanged. Whole-frame profiling still excludes dedicated transfer-only
buffers; this change does not silently expand that metric's definition.

The demo's existing `--profile=PREFIX` exports GPU pass records. Combining it
with `--gpu-memory-stats` now logs publication begin/end boundaries and the
execution IDs of graphs recorded within each publication. The synchronous
IDs join those boundaries to the deferred profiler output. Results are
collected after the existing publication work without an extra GPU wait.
Constructor initialization graphs and the later rendered frame are excluded
from the publication's upload sum.

The reported value is the **sum of `upload_copy` / `upload_mipmaps` node elapsed
intervals in `staging_upload` graphs**, from TOP to BOTTOM timestamps, including
each node's prologue barriers. It includes geometry and material-buffer uploads
and the environment resources staged by the normal scene path, not just the
changed image. It excludes inter-node/submission gaps and work outside those
nodes. A timestamp may be written at a logically later pipeline stage under
the [timestamp command contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html).
This is an instrumented elapsed-interval sum, not GPU occupancy, a complete
publication envelope, demonstrated overlap or a frame-time improvement.

## Method and results

The [runner](../../ZenAssetProcessor/tools/validation/asset_processor_production_upload.py) adds opt-in
`--gpu-timings`. It requires complete graph/pass joins, zero omitted details,
available finite durations, accepted transfer serials, and at least one actual
transfer pass for every publication. The saved CSV records include logical
queue and physical queue equivalence. All measured upload nodes in this run
used the dedicated transfer queue.

Workloads and repetition match the preceding staging campaign: 1024² BC7 and
flat-normal BC5 fixtures, a 4096² BC7 staging-pressure fixture, and real
NormalTangentTest with BC4 role splitting. Raw/cooked packages use offline mips,
with no geometry or transport compression. Each variant and RHI mode has one
validation capture, three fresh process/device samples with alternating order,
and five replacements after an initial load in one warm process. This gives
48 fresh and 80 warm measured samples; validation and warmup timings are not
included in the medians. OS and driver caches are uncontrolled.

Windows MSVC Release; Ryzen 9 9950X3D; RTX 5080, driver 617.42. Renderer settings:
640×480, fixed step, one raster frame, RT/UI/cache disabled, vsync off. Validation
is enabled for the 16 capture processes and disabled for timing samples. The
frozen hash-identical `7zFM.exe` uses the existing RTSS no-hook profile; global
overlay settings were not changed.

Median summed GPU upload intervals, in milliseconds, raw → cooked:

| Workload | RHI | Fresh | Warm replacement |
|---|---|---:|---:|
| BC7 1024² | Inline | 1.004 → 0.862 | 0.859 → 0.713 |
| BC7 1024² | Threaded | 1.004 → 0.861 | 0.857 → 0.711 |
| BC5 1024² | Inline | 1.004 → 0.861 | 0.854 → 0.713 |
| BC5 1024² | Threaded | 1.004 → 0.861 | 0.855 → 0.712 |
| BC7 4096² pressure | Inline | 3.908 → 1.588 | 3.756 → 1.441 |
| BC7 4096² pressure | Threaded | 3.910 → 1.588 | 3.758 → 1.441 |
| Real BC4 role split | Inline | 3.184 → 3.286 | 3.034 → 3.141 |
| Real BC4 role split | Threaded | 3.183 → 3.286 | 3.035 → 3.138 |

The pressure fixture stages 110,453,892 → 43,345,056 bytes per fresh publication;
its threaded fresh GPU interval sum decreases by about 59%. The role-split case
stages **more** bytes, 88,875,032 → 91,671,248, and has 18 → 19 upload nodes in a
fresh publication. Its measured upload sum increases. Compression therefore
does not establish a universal memory or upload-speed benefit. Full samples and
min/median/max summaries are retained; these small repeated samples are not a
statistical significance study. CPU timings from this instrumented campaign
should not be treated as equivalent to the preceding uninstrumented campaign.

## Verification and evidence

- All 16 validation processes passed without validation errors, `VUID-` or
  `SYNC-HAZARD` messages. All 80 processes ended with zero native host-upload
  bytes and zero VMA/engine allocation leaks.
- Controlled raw/cooked captures are pixel-identical. Real role-split captures
  have maximum channel difference 1. **All 16 current captures are also
  pixel-identical to their corresponding preceding staging-campaign captures.**
- Non-texture payloads and material semantics, after resolving texture IDs,
  match each raw/cooked pair. All recorded GPU data was independently reparsed
  from logs/CSV/JSON. Seven corrupted/incomplete test inputs were rejected by
  the measurement reader rather than counted as valid timings.
- **28 CPU timing/profiling tests passed.** The native timing/upload selection
  has **60 passed and four skipped** tests, with zero failures. All 16 native
  timing tests passed, including the new transfer pool isolation/reuse test
  and missing-feature/function fallback test. The four existing skips are the
  packed-run failure cases with timeline submissions disabled. Native query observers
  verify non-waiting reads. Test teardown reports zero engine leaks.
- The first campaign stopped on malformed boundary JSON before accepting a
  measurement. The logger was fixed and the complete campaign rerun in a new
  directory. A temporary allocation in the new test observer was also removed
  before the final native test run. Earlier logs remain as superseded evidence.

Commands for the final campaign and focused tests:

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_production_upload.py `
  --renderer build/asset-integration-release/bin/scene_renderer_demo.exe `
  --processor E:/Dev/ZenAssetProcessor/build/processor-port/Release/zenassetc.exe `
  --reference-config build/asset-processor/fresh-verification/visual-0-source.cfg `
  --role-split-source E:/Dev/glTF-Sample-Assets/Models/NormalTangentTest/glTF/NormalTangentTest.gltf `
  --output build/asset-processor/m5-production-gpu-final --gpu-timings

RenderCoreTest.exe --gtest_filter=*GPU*Timing*:*RDGProfiling*
VulkanRHIIntegrationTest.exe --gtest_filter=*VulkanGPUTimingIntegrationTest*:*VulkanUploadIntegrationTest*
```

Local artifacts:

- `build/asset-processor/m5-production-gpu-final/results.json`, `summary.json`,
  `previous-capture-comparisons.json`, `parser-checks.json`, and per-process
  profile JSON/pass CSV/frame CSV, logs, configurations, packages and captures.
- `build/asset-integration-release/m5-production-gpu-vulkan-verified.xml` and
  `m5-production-gpu-rendercore-final.xml`, with corresponding logs.
- `build/asset-integration-release/m5-production-gpu-baseline/` and
  `m5-production-gpu-evidence-final/`: before/after sources, scoped diff, identity
  manifest and final audit. Renderer SHA-256:
  `32117bf0b69f8621bb3697210f1cf9aa247a41daead6f9bd8dc283522e39776d`.

Source/dependency and binary identities were verified, the original engine
configuration was restored, and no capture lock remains. Direct production
upload timing is established for these four workloads on this device in both
RHI modes. Overlap, frame benefit, broader devices/workloads, unresolved lossy
visual-quality gates and the remaining M6–M8 delivery work remain open.
