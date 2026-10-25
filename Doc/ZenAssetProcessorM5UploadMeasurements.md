# M5 isolated upload measurements

Follow-up, 24 October: [production staging measurements](ZenAssetProcessorProductionStagingMeasurements.md)
now cover the real scene loader/publication path in inline and threaded RHI
modes, including native upload-buffer lifetime peaks, pool-pressure waits and
repeated replacements. Production GPU timestamp intervals and broader workload
acceptance remain open. The isolated measurements below retain their own scope.

The bounded native benchmark passed on 2026-10-23 on an NVIDIA GeForce RTX 5080. It measures actual VMA resource allocations and graphics-queue GPU timestamp intervals. It does **not** close production staging-pool, asynchronous transfer-queue, whole-scene GPU memory, or rendering-performance acceptance.

Implementation: [SceneUploadBenchmark.cpp](../../ZenAssetProcessor/tests/integration/zenengine/CommonTest/SceneUploadBenchmark.cpp), [runner](../../ZenAssetProcessor/tools/validation/asset_processor_upload_benchmark.py). The sole production API addition is the const `VulkanBuffer::GetMemoryAllocation()` accessor, matching the existing texture accessor. No renderer or GI algorithm changed.

Evidence is in [results.json](../build/asset-processor/m5-upload-verified/results.json), with individual JSONL files, process logs, source/dependency hashes, cook commands, and a frozen executable under the same directory. Its SHA256 is `0615d0dc3b6260332f3eca5bb0c7c9f1e2540bd2301470d424dccfa5ec63fb86`. The exact executed runner is archived and verified against its recorded start hash; later runner changes only added geometry-equality assertions and automatic runner snapshotting. Geometry equality was also checked against every completed result.

## Method and bounds

Each of five cases has one raw and one cooked package from the same frozen CLI and source, with offline mip generation enabled and transport compression disabled. The three controlled fixtures are 1024×1024, eleven mips, one consumed role: solid sRGB color for BC7, solid R occlusion for BC4, and a flat positive-Z normal for BC5. They isolate format capacity and upload cost; they are not texture-quality coverage. Initial gradient fixtures triggered the existing strict quality fallbacks and were excluded without changing thresholds. EnvironmentTest has the same authored 256×256, five-mip cube in both packages. NormalTangentTest is a separate real-content BC4 role-split case.

For each package the runner performed a separate validation-enabled smoke, three fresh-process/device measurements, and five measured fresh-resource iterations after one unreported warmup in a shared process/device. Raw/cooked fresh-process order alternated. OS file and driver caches were uncontrolled: “cold” means a fresh process/device, not cold storage. Timing samples have validation disabled; all ten validation smokes passed with zero validation errors and zero VMA/engine leaks. All forty timed processes succeeded, yielding thirty fresh-process samples and fifty warm samples. Logs also retain peak process working set/private commit independently of GPU memory.

At this M5 checkpoint, the harness builds the runtime scene and allocates one native image per runtime texture, retaining real color/data view duplication and all five 1×1 scene defaults. It creates the normal sampled default view and the same sampled/transfer-src/transfer-dst usage as scene texture upload. The selected authored environment is uploaded once, matching the skybox/prefilter alias. Raw multi-pixel textures without an offline mip chain are rejected rather than silently omitting runtime mip generation. The subsequent [M6 benchmark](ZenAssetProcessorM6UploadMeasurements.md) adds shared array pages, layer views and view/copy-region accounting while retaining this standalone and cube behavior.

The texture timestamp encloses all buffer-to-image copies and initial/final image transitions. A second, separately submitted/completed interval encloses canonical `asset::Vertex`/32-bit index copies and their final visibility barrier. Existing graphics-queue timestamp queries use top/bottom-of-pipe endpoints; the device reports 1 ns timestamp period. Timings require an `Available` query result after queue completion; unsupported timestamps fail the measurement. CPU read, adaptation, runtime construction, planning, allocation, mapped staging copies, recording, submission, and waiting are separate JSON fields. No CPU duration is labeled GPU execution time.

Geometry usage matches vertex/index/storage/transfer and device-address/acceleration-input flags when supported, but measurements exclude runtime deformation, additional UV/material/instance buffers, acceleration structures, GI resources, external/default wood/environment assets, SH-derived irradiance and BRDF LUT work. Geometry is canonical decoded data, not transport-compressed data. Its allocation and payload match exactly within every pair; there is no claimed geometry-memory saving.

`allocation_bytes` is `VmaAllocationInfo.size` for the native resource, including resource alignment. `device_local` comes from that allocation's Vulkan memory-type properties. Every measured destination allocation was device-local; every explicit staging allocation was host-visible, coherent, and non-device-local on this device. Neither value represents physical residency or driver-private/view memory. `vma_committed_*` measures allocator backing blocks, including retained pools, and is reported separately.

Each destination has its own explicit staging buffer. All are live together, so their summed measured allocation is the harness's staging peak. This is deliberately distinct from the production append-only pooled staging allocator and its retirement peaks.

## Measured allocations

Values are bytes, raw → cooked. Texture totals include scene defaults and the selected authored cube; staging totals also include canonical geometry staging. Counts are native textures, including five defaults.

| Case | Texture count | Texture allocation | Explicit staging peak | Canonical geometry allocation |
|---|---:|---:|---:|---:|
| Controlled BC7 | 6 → 6 | 5,596,672 → 1,402,880 | 5,592,848 → 1,398,560 | 768 → 768 |
| Controlled BC4 | 6 → 6 | 5,596,672 → 704,000 | 5,592,848 → 699,504 | 768 → 768 |
| Controlled BC5 | 6 → 6 | 5,596,672 → 1,402,880 | 5,592,848 → 1,398,560 | 768 → 768 |
| EnvironmentTest BC6H | 8 → 8 | 11,207,168 → 3,342,848 | 13,025,776 → 5,169,136 | 1,849,088 → 1,849,088 |
| NormalTangentTest BC4 role split | 8 → 9 | 67,116,544 → 69,915,136 | 67,648,368 → 70,444,592 | 539,648 → 539,648 |

The first four pairs have identical texture dimensions, mip counts, layers, and resource counts. NormalTangentTest requires an additional occlusion image while retaining the shared roughness/metallic image; its texture allocation increases 4.17%. That real duplication cost is not an isolated same-resource codec comparison.

VMA backing-block commitment stays 64 MiB for both variants in the first four cases and 384 MiB for both NormalTangentTest variants. This demonstrates why whole-process allocator commitment cannot substitute for per-resource allocation measurements.

## Measured graphics-queue texture upload

GPU durations are microseconds. Medians are raw → cooked; warm ranges show all five measured samples.

| Case | Fresh-process median (3) | Warm median (5) | Warm raw range | Warm cooked range |
|---|---:|---:|---:|---:|
| Controlled BC7 | 204.768 → 59.328 | 204.480 → 58.656 | 203.968–204.608 | 58.624–59.040 |
| Controlled BC4 | 204.768 → 35.040 | 204.064 → 34.784 | 203.936–204.512 | 34.688–34.880 |
| Controlled BC5 | 204.832 → 59.328 | 203.968 → 58.688 | 203.936–204.544 | 58.656–58.688 |
| EnvironmentTest BC6H | 425.312 → 153.120 | 424.224 → 152.352 | 423.936–424.512 | 151.776–153.152 |
| NormalTangentTest BC4 role split | 2359.680 → 2465.472 | 2359.712 → 2465.280 | 2359.552–2632.864 | 2465.120–2742.848 |

These are isolated, serialized native graphics-queue uploads on one machine. They do not measure production transfer-queue overlap, staging pool pressure, time to a rendered scene, or frame performance. Package sizes, payload bytes, resource allocations, global commitment, CPU stages, and GPU stages remain independent fields in the evidence.

## Reproduction

Build `SceneUploadBenchmark`, then invoke:

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_upload_benchmark.py `
  --benchmark build/asset-integration-release/bin/SceneUploadBenchmark.exe `
  --processor build/asset-processor/m5-bc6h-binaries/zenassetc.exe `
  --hdr-source build/asset-processor/m5-bc6h-cpu-verified/source/EnvironmentTest.gltf `
  --role-split-source build/asset-processor/m5-bc45-cpu-final/normal-source/NormalTangentTest.gltf `
  --output build/asset-processor/new-upload-run
```

Direct harness usage is `SceneUploadBenchmark <package> --iterations=5 --warmup=1 --validation=0 --output=result.jsonl`. Dedicated output avoids interleaving structured data with engine log streams. Each output line has a `ZEN_UPLOAD_RUN`, `ZEN_UPLOAD_RESOURCE`, or `ZEN_UPLOAD_SAMPLE` prefix followed by a JSON object. The runner checks allocation sums, finite available timings, matching controlled texture layouts, compression admission, canonical geometry equality, and binary identity.
