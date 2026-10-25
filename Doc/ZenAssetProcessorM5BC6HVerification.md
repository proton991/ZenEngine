# BC6H environment integration verification

The standalone environment records, schema, cook path and engine upload
integration pass their focused checks. Independent HDR quality checks and raster
reference comparisons also pass. GI repeatability and full desktop visual
acceptance remain open. This does not complete M5.

## Ownership and compatibility

`ImageBasedLightAsset::specular` owns an immutable `EnvironmentCubePixels`.
Raw RGBA32F and BC6H UFLOAT use mutually exclusive face arrays. Scene copies
share original pixels; successful compression publishes only block storage.
Authored face order, mip count, spherical-harmonic coefficients, intensity and
rotation are preserved. No ordinary mip filter replaces authored roughness
levels, including partial chains.

Environment resource version 2 adds an explicit format. Version 1 still reads
the original typed floats exactly into a raw cube owner. Container version 3
and metadata version 6 are unchanged. All face footprints and reserved BC6H
block modes are validated before publication. Reader and writer share one schema.

## Standalone checks

The complete Release build and **27/27 CTests passed** after the public record
change, in 10.96 seconds. Evidence:

- `E:/Dev/ZenAssetProcessor/build/processor-port/m5-hdr-full-build.log`
- `E:/Dev/ZenAssetProcessor/build/processor-port/m5-hdr-full-tests.log`
- Earlier focused integration: `m5-hdr-tests.log`, six suites passed.

Public Cook tests report 64 checks, or 79 with imported fixtures. Scene I/O
reports 94 checks, or 119 with imported fixtures. They cover immutable owners,
odd 5×5 faces, partial mips, raw/BC6H source-free packages, processing metadata,
Environment-v1 compatibility, unknown formats, late reserved blocks, malformed
counts, mixed representations and nonfinite source rejection. A 64 KiB cook
budget succeeds with a caller-owned 384 KiB raw cube, demonstrating that the
original cube pixels are not copied during cooking.

Negative/overflow RGB, non-unit alpha, excess dimensions and an unacceptable
final mip each retain the whole original cube with a diagnostic. Cancellation
before, during and after encoding exposes no partial scene. Limits account for
requested new storage and scratch; caller input, allocator bookkeeping and
total process RSS are outside that claim. Existing semantic validation scans
finish before observing cancellation. Two independent read-only reviews found
no actionable issue in the public/schema/cook ownership and budget paths.

## Engine and native checks

All seven affected engine targets built. CPU checks passed 40 CommonTest cases,
43 editor model cases, 705 RenderCore cases and 30 reflection/resource cases.
Logs are `build/asset-integration-release/bc6h-engine-build-2.log` and
`bc6h-{common-tests,editor-model-tests,rendercore-tests,reflection-tests}.log`.
All reported zero engine leaks.

Source/Cook/adapt, explicit compressed-package, cache-miss and cache-hit paths
agree on all environment fields and blocks. The package also loads after source
removal. Legacy v2 retains its old wire layout and rejects compressed environment
writes without replacing the destination. Compressed engine cube owners remain
shared across runtime copies.

Three native Vulkan cases pass on the RTX 5080, including exact six-face BC6H
cubemap readback with odd/tiny mips, incompatible views and unsupported device
formats. Six editor GPU cases pass across both render modes, including malformed
candidate rollback and pending/raw environment restoration. These runs are
validation-clean with no reported GPU leaks. Native evidence is in
`build/asset-processor/m5-bc6h-native-{tests.log,results.json}`; editor evidence
is in the `bc6h-*` logs under the engine build directory.

The frozen renderer is `build/asset-processor/m5-bc6h-binaries/scene_renderer_demo.exe`,
SHA-256 `7265df67568bc080ec7c275cb83cff081f61d843999652c8de3d43e3c56d1be8`.
Upload correctness does not imply rendered quality acceptance.

## Reader closure and bounded fuzzing

Reader-only Debug and ASAN suites each passed 4/4. A fresh installed Reader
consumer passed smoke/previews and loaded 17 source-free seeds. Its link closure
contains Format, Reader and private Block/Zstd/Meshopt decoders, with no Processor,
source importer or encoder libraries.

The Clang ASAN/libFuzzer run completed **16,293 executions in 121 seconds**,
seed 56789, with no sanitizer failure or crash artifact. Maximum input was
256 KiB, input timeout five seconds and RSS limit 1 GiB; peak reported RSS was
446 MiB. Seeds cover actual BC4/BC5 tiny/odd material payloads, BC7, transport
and mesh codecs, old float environments and eight Environment-v2 float/BC6H
tiny/odd/partial/full/black cases. Successful material previews include explicit
BC5 normal interpretations. HDR preview decoding is outside this scope.

Evidence: `E:/Dev/ZenAssetProcessor/build/reader-fuzz/bc456h-verification.json`
and `build/codec-reader/bc456h-closure-verification.json`, with run/seed/preflight
logs. Fuzzer SHA-256:
`aab2d4f389e868eab9d912e29b5a2cc1b10db872ef26b9e62da01fed14b5bba6`.
An initial preflight exposed a harness assumption about unsupported-preview
cancellation priority; it was corrected and preserved in the evidence. It was
not a production decoder failure. Earlier BC7 evidence remains unchanged.
These finite checks do not establish exhaustive malformed-input safety.

## Independent quality and storage evidence

Microsoft DirectXTex CPU decoding independently checks public BC6H output,
separately from the private encoder/decoder quality gate. All **12/12 cases** and
**90/90 authored face/mip measurements** pass; maximum relative RGB RMSE is
0.014108 against the configured 0.02 bound. Default/reference resources match,
repeated cooks are deterministic, and 24 source-free raw/compressed read/write
roundtrips preserve their resources exactly. Six excluded cases keep their full
raw cubes byte-exact. Source copies were restored after the audit.

EnvironmentTest retains its 256-pixel faces and five authored mip levels.
Cube payload falls from 8,380,416 to 523,776 bytes (16× smaller), and its package
from 12,742,547 to 4,885,907 bytes. These are payload/file measurements, not GPU
allocation or upload execution measurements. Process samples are preliminary
single-run diagnostics with an uncontrolled warm OS cache.

Evidence and tool identities are in
`build/asset-processor/m5-bc6h-cpu-verified/results.json` and the dedicated
[CPU/native/visual report](ZenAssetProcessorM5BC6HVisualVerification.md).
The frozen CLI SHA-256 is
`035aea31d547117d7415d9fee4da5fe6a29821da9e42d0c03524d684dec2d22f`.
The independently signed DirectXTex executable SHA-256 is
`dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06`.

All 15 rendered captures are Vulkan-validation-clean. Raster source/raw and
identical-raw repeats match exactly. EnvironmentTest's raw/BC6H raster comparison
has maximum display-channel delta 24, RGB RMSE 0.667288 and PSNR 51.6445 dB;
0.044596% of pixels differ by more than eight channel levels. These are descriptive
lossy measurements, not newly selected acceptance thresholds. A separate colored
six-face 5×5 / 2×2 fixture passes twelve independent face/mip checks with zero
decoded error; its rendered comparison differs by at most one channel level.

The strict GI reference gates remain failed: identical raw repeats differ in
37 channels (maximum one) for compute GI and 82 (maximum two) for hardware GI.
No tolerance has been relaxed. The dedicated report records reviewed frames,
the maximum-delta crop, restored configuration and unchanged shader identities.
Whole-renderer raster VMA commitment falls from 208,666,624 to 141,557,760 bytes;
these pool-granularity values are not isolated environment allocations.
The [isolated upload benchmark](ZenAssetProcessorM5UploadMeasurements.md) now
measures EnvironmentTest's native texture allocation as 11,207,168 → 3,342,848
bytes with matching material mip chains and authored cube levels. Its five-sample
warm graphics-queue texture upload median is 424.224 → 152.352 microseconds.
BC4/BC5 and BC7 visual/GI limits, production staging-pool peaks and asynchronous
transfer behavior remain separate open gates.
