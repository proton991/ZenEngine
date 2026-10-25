# M6 array upload measurements

24 October 2026. The isolated upload benchmark now handles native array pages
and their layer views. The measured cases transfer the same bytes with fewer
images and copy regions, but more image views. GPU upload medians improve only
slightly, while resource allocation sizes increase. This does not justify
enabling arrays by default or establish a production frame-time benefit.

## Benchmark change and scope

`SceneUploadBenchmark` previously planned one image per runtime texture and
accepted only single-layer mip footprints. It now shares immutable array owners
within one load, validates and copies every layer, permits compatible color
views, and creates each distinct layer/format view once. Mutable/ordinary
standalone bindings retain their previous allocation behavior. Arrays require
explicit mip chains. Cube faces still use single-layer copy regions.

Resource/sample JSON now includes native image-view counts and copy-region
counts. The image count includes the five runtime fallback textures. The view
count includes the full-array default view plus the individual material layer
views, even though the default array view is not used by material sampling.

This remains the [M5 isolated graphics-queue harness](ZenAssetProcessorM5UploadMeasurements.md):
one explicit staging buffer per destination, dedicated texture/geometry timestamp
intervals, and no production staging pool, asynchronous transfer, descriptor heap,
draw calls, scene publication, or frame rendering. CPU allocation includes layer
view creation. It does not measure their driver-private memory. VMA resource
allocation sizes, allocator backing-block commitment, process memory, CPU stages
and GPU intervals remain separate fields.

## Method

Windows, MSVC Release, RTX 5080, driver value 2588573696, 1 ns timestamp period.
The original mixed-material fixture is generated at 128x128 and 1024x1024. Its
repeating material is opaque for this experiment so every image can have an
offline mip chain. The renderer's alpha-mask fixture remains unchanged. The
repeating image stays standalone with an explicit sampler-policy diagnostic.
Raw and BC7/BC5/BC4 profiles each compare packing off/on against the same source.

Each of eight packages has a separate Vulkan-validation smoke, three fresh
process/device samples, and three warm processes with one unreported warmup
followed by five fresh-resource iterations. Off/on order alternates between
repeats. All **56 primary processes** succeed: eight validation smokes and 48
timing processes, yielding 24 fresh and 120 warm measured samples. No validation
errors, remaining VMA bytes or tracked CPU leaks are reported. The runner checks
resource sums, layer/view/region counts, finite GPU timestamps, selected formats,
equal payload bytes and equal canonical geometry within each pair.

OS/driver caches and GPU clocks are uncontrolled. `nvidia-smi` snapshots before
and after the run show 6% and 14% utilization and 1507/1387 MHz graphics clocks;
the GPU was not reserved exclusively. Therefore small timing differences should
not be treated as general throughput gains or statistical significance.
The packages and executable copies are retained with SHA-256 identities.

## Resource results

Totals below include the five default textures; geometry is reported separately.
All destination allocations are device-local. `allocation_bytes` is VMA's
per-resource allocation size, not physical resident VRAM or driver overhead.

| Maps/profile | Texture payload, bytes (both) | Allocation off → array, bytes | Package off → array, bytes |
| --- | ---: | ---: | ---: |
| 128 / raw | 611,680 | 626,176 → 632,320 | 1,095,034 → 1,094,666 |
| 128 / BC | 131,252 | 149,504 → 153,600 | 254,202 → 253,770 |
| 1024 / raw | 39,146,848 | 39,161,344 → 39,167,488 | 68,682,466 → 68,681,954 |
| 1024 / BC | 8,388,788 | 8,407,040 → 8,415,232 | 14,855,746 → 14,855,250 |

Native images decrease **12 → 9**, while image views increase **12 → 15**.
Copy regions decrease **61 → 37** at 128x128 and **82 → 49** at 1024x1024.
Canonical geometry remains 1,416 payload bytes and 1,792 allocation bytes in
every pair. Explicit staging allocation falls by only 16 or 48 bytes; there is
no reduction in copied texture bytes. Package differences are small record and
metadata changes, not texture payload compression caused by packing.

## Timing results

Texture timestamps cover copies and image transitions. Values are warm median
and nearest-rank p95 in microseconds (15 samples per layout). With this sample
count, p95 is the maximum observed value.

| Maps/profile | Off median / p95 | Array median / p95 |
| --- | ---: | ---: |
| 128 / raw | 60.352 / 61.376 | 58.816 / 59.520 |
| 128 / BC | 39.712 / 39.968 | 39.520 / 40.288 |
| 1024 / raw | 1421.632 / 1595.520 | 1415.200 / 1526.848 |
| 1024 / BC | 342.592 / 343.072 | 341.376 / 343.328 |

Warm allocation CPU medians in milliseconds are 0.0359 → 0.0300, 0.0302 → 0.0307,
4.3075 → 4.5601, and 0.0637 → 0.0556 in the same order. They show no consistent
allocation-time win. Read/adapt/runtime/plan/stage/record/submit/wait CPU timings,
fresh-process samples, process memory and all extrema remain in `results.json`.
Cook command wall times are recorded once per package, including parsing,
processing and writing; they are not a repeated or stage-separated cooking benchmark.

## Boundary checks and installed consumer

A separate CMake project consumes the current Release installation solely through
`find_package(ZenAsset CONFIG REQUIRED COMPONENTS Processor)` and
`ZenAsset::Processor`. Its source-free reader/writer adds two explicit texture
aliases to each raw package. This tests valid package records that the glTF
normalizer otherwise coalesces. No ZenCore headers or ZenAssetProcessor
source-checkout paths occur in the consumer's compile/link inputs.

With these manufactured aliases, images are **14 → 9**, views **14 → 15**, copy
regions **77 → 37**, and copied texture bytes **786,440 → 611,680**. The extra
array aliases reuse existing layer views. Resource allocation is **804,352 →
632,320 bytes**. This proves alias handling and removal of redundant uploads for
that constructed package; it is not a measured benefit for a source workload.
The earlier two attempted source fixtures were normalized and did not exercise
distinct aliases; their logs are retained separately.

The existing BC6H environment package also passes a validation regression, with
its allocation/payload totals matching the M5 baseline. A package containing the
render fixture's mip-less alpha-mask image still rejects with an actionable
offline-mip diagnostic, no measurement samples and no leaks. The benchmark does
not silently omit runtime mip generation to accept that case.

## Reproduction and evidence

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_array_upload_benchmark.py `
  --benchmark build/asset-integration-release/bin/SceneUploadBenchmark.exe `
  --processor E:/Dev/ZenAssetProcessor/build/processor-port/Release/zenassetc.exe `
  --output build/asset-processor/new-array-upload-run
```

The output directory must be new. The verified run is
`build/asset-processor/m6-array-upload/`: `results.json`, `run-conditions.json`,
frozen `binaries/` and `sources/`, all per-run JSONL/logs and cooked packages.
Additional checks are in `package-alias-validation/`, `alias-consumer/`,
`cube-regression.json`, and `missing-mips.json`. Install/configure/build logs and
`evidence-identities.json` record the consumer and supplemental evidence.

M6 still requires representative source workloads, production asynchronous
staging/upload/publication costs, draw/frame measurements, broader visual gates
and spatial filtering. Installed consumption is reverified, but release
publication/pinning and the remaining M8 migration checks are not complete.
