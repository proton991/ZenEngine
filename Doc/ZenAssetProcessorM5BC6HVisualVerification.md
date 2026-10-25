# M5 BC6H environment verification

23 October 2026. The independent CPU audit passes all 12 inputs and all 90
decoded face/mip measurements. Source-free read/write preserves every resource
exactly. BC6H remains explicitly enabled and disabled by default. Native Vulkan
upload, view compatibility and unsupported-device rejection tests pass.
All 15 rendered captures complete without validation errors. Raster source/raw
and repeated raw inputs are exact. **Full rendered acceptance remains open:**
repeated identical raw GI inputs differ, so the strict GI reference gates fail.
No frame tolerance was added or relaxed.

## Independent CPU audit

[The audit](../../ZenAssetProcessor/tools/validation/asset_processor_hdr_audit.py) reads explicit little-endian
Environment-v2 fields from container-v3 packages, with metadata-v6 and texture-v2.
Transport and mesh compression are disabled. Synthetic fixtures use only the
public `CookScene`/`WriteScene` API through
[a separate fixture program](../../ZenAssetProcessor/tools/validation/asset_processor_hdr_fixtures.cpp).
The real input is Khronos EnvironmentTest's `glTF-IBL` variant.

The decoder is Microsoft's signed
[DirectXTex May 2026 `texconv`](https://github.com/microsoft/DirectXTex/releases/tag/may2026),
version 2026.5.8.1. Its SHA-256 matches the official release asset digest and
Authenticode reports `Valid`. Each compressed face/mip is wrapped in a DX10 DDS
and converted with `-nogpu -f R32G32B32A32_FLOAT -m 1 -dx10`. This is independent
of the processor's bcdec implementation. The audit checks decoded finiteness,
opaque alpha and each face/mip's RGB relative RMSE:
`sqrt(sum((decoded-source)^2) / sum(source^2))`. A zero-energy source requires
zero error. HDR values above 1 remain floating point throughout comparison.

The existing [quality contract](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json) permits
at most 0.02 relative RMSE for each face/mip. The largest measured value is
**0.0141080208**, on EnvironmentTest. The largest absolute RGB error is
0.94702768; this is reported descriptively because the established HDR gate
uses relative RMSE. Six cubes convert, and six fixtures retain their complete
original RGBA32Float cube. All retained payloads are byte-exact.

| Input | Size / authored mips | Result | Raw pixel bytes | Cooked pixel bytes |
| --- | --- | --- | ---: | ---: |
| Tiny | 1 / 1 | BC6H | 96 | 96 |
| Odd 3 | 3 / 1 | BC6H | 864 | 96 |
| Odd 5, partial chain | 5 / 2 | BC6H | 2,784 | 480 |
| Odd 7, full chain | 7 / 3 | BC6H | 5,664 | 576 |
| Black | 4 / 3 | BC6H, exact zero | 2,016 | 288 |
| Dark last tail | 4 / 3 | Quality fallback | 2,016 | 2,016 |
| Negative RGB | 4 / 3 | Unsigned range fallback | 2,016 | 2,016 |
| RGB above 65,504 | 4 / 3 | Half-float range fallback | 2,016 | 2,016 |
| Non-unit alpha | 4 / 3 | Alpha fallback | 2,016 | 2,016 |
| Zero-error requirement | 4 / 3 | Quality fallback | 2,016 | 2,016 |
| Dimension limit | 4 / 3 | Dimension fallback | 2,016 | 2,016 |
| EnvironmentTest | 256 / 5 | BC6H | 8,380,416 | 523,776 |

The five authored EnvironmentTest mip levels are preserved, rather than
extended to a complete 1×1 chain. Cube face ordering, extent and mip count,
light name, intensity, rotation and spherical harmonics remain unchanged.
Every non-environment resource except processing metadata remains exact.
Compressed cubes contain only block faces; no duplicate raw cube is serialized.
Fallback diagnostics explain why the original owner was retained.

All 12 default/reference comparisons and all 12 repeated-cook comparisons are
resource-exact. The copied real source directory is renamed before CLI
validation and the source-free `ReadScene`/`WriteScene` helper execute, then
restored. All **24 raw and cooked roundtrips** preserve every resource, including
metadata. Synthetic fixtures have no external source dependency.

EnvironmentTest's package shrinks from **12,742,547 to 4,885,907 bytes**.
Its cube pixel payload and modeled upload bytes shrink 16×. Tiny 1×1 faces do
not save bytes because both formats occupy 16 bytes per face. Fallback packages
grow slightly because exclusion diagnostics are stored; pixel storage remains
unchanged.

Two diagnostic EnvironmentTest BC6H cooks took 32.703 and 34.000 seconds, with
sampled peak working sets of 25,190,400 and 25,231,360 bytes. These are single
warm diagnostic runs alongside other CPU work, not the plan's repeated
performance benchmark or a performance acceptance result. No GPU allocation
savings are inferred from these CPU memory samples.

- [Complete independent results, commands, input hashes and memory samples](../build/asset-processor/m5-bc6h-cpu-verified/results.json)
- [Fixture outcomes and fallback diagnostics](../build/asset-processor/m5-bc6h-cpu-verified/fixtures.log)
- Root's full standalone Release build passed 27/27 CTest suites before freezing.
- The earlier `m5-bc6h-cpu-final` run stopped on an audit DDS-header assumption:
  DirectXTex defaults to a legacy float DDS header. The corrected run explicitly
  requests DX10 output; the failed attempt remains preserved.

## Native Vulkan boundary

The extended native suite passes **3/3**, with Vulkan validation enabled, no
validation errors and zero VMA or tracked CPU leaks. It verifies:

- Upload/readback equality for BC6H 5×7, 2×3 and 1×1 2D mips, plus a six-face
  5×5, 2×2 and 1×1 cube. Cube bytes total 576, including minimum-sized tails.
- Same-format BC6H views are accepted; incompatible BC5/RGBA views are rejected
  before native view creation.
- An injected unsupported BC6H capability rejects texture creation before any
  image view is published.
- Existing BC4, BC5 and BC7 upload/view checks and native BC4/BC5 channel-sampling
  regression remain passing.

The BC6H test copies block bytes; it does not claim to validate sampled HDR
values on the GPU. Independent HDR decoding is covered by DirectXTex above.
CPU footprint checks include 384 bytes for the six-face 5×5 base level and
96 bytes for six 1×1 faces.

[Native log](../build/asset-processor/m5-bc6h-native-tests.log) and
[binary/process record](../build/asset-processor/m5-bc6h-native-results.json).
The codec integration agent also reports editor rollback/restoration tests
6/6, covering malformed BC6H candidates, discarded pending work and restoration
of authored raw environments in both inline and threaded loading, with no leaks
or validation errors:
[editor log](../build/asset-integration-release/bc6h-editor-rendering-tests.log).

## Rendered comparisons and visual review

[The capture harness](../../ZenAssetProcessor/tools/validation/asset_processor_hdr_visual.py) freezes renderer,
source, audited packages, shaders and CPU audit identities. EnvironmentTest
uses its imported camera, original lighting and authored five-level cube chain.
The environment is visible as the background. Raster uses three fixed steps;
compute and hardware GI use sixteen. Hardware logs confirm the hardware ray
provider. No renderer, GI or shader code is changed for these measurements.

Each backend captures direct source, raw reference, BC6H, then the same raw
reference again. Raster source/raw and raw-repeat comparisons are byte-exact.
All captures are 640×480, validation-clean and nonblack. The source HDR cube
ranges from 0.00802319 to 6.48218393 in this input.

| Capture | Raw/BC6H max display-channel delta | RGB RMSE, 8-bit | PSNR | Pixels with delta >8 |
| --- | ---: | ---: | ---: | ---: |
| EnvironmentTest raster | 24 | 0.667288 | 51.6445 dB | 0.044596% |
| EnvironmentTest compute GI | 24 | 0.669605 | 51.6144 dB | 0.044596% |
| EnvironmentTest hardware GI | 24 | 0.669622 | 51.6142 dB | 0.044596% |
| Colored face/partial-chain raster | 1 | 0.015832 | 84.1401 dB | 0% |

These lossy frame numbers are descriptive, not new acceptance thresholds.
The independent per-face/mip HDR limits remain mandatory. The GI numbers also
include observed run-to-run nondeterminism:

| GI backend | Source/raw differing channels / maximum | Identical raw repeat differing channels / maximum |
| --- | ---: | ---: |
| Compute | 36 / 1 | 37 / 1 |
| Hardware | 95 / 2 | 82 / 2 |

The results retain `automated_checks_passed: false` because these exact reference
gates fail. Compression does not explain differences between identical raw
packages; investigating GI repeatability remains outside this texture change.

The additional orientation fixture reuses EnvironmentTest geometry with six
distinct HDR face colors, identity cube rotation and a deliberately partial
5×5 → 2×2 authored chain. Colors rotate between the two authored levels so
roughness-dependent mip selection is visible. Public CookScene creates its
raw/BC6H pair; a separate DirectXTex audit passes all twelve face/mip comparisons
with **zero decoded error**, exact non-environment resources and exact
source-free roundtrips. The raw repeated frame is exact. BC6H rendering differs
by one display level in 231 channels. Format-dependent GPU sampling is a
plausible explanation, but this capture does not establish that cause. The
fixed view checks visible face placement and mip use, not every camera direction.

Visual inspection of the full frames and the maximum-delta crop finds matching
sphere silhouettes, roughness progression and colored face placement. The real
environment shows subtle background and reflection color changes. Its maximum
channel delta occurs at **(162,122)** in the sky beside a bright feature:
raw RGB `(154,125,105)`, BC6H `(178,147,102)`. The difference images are amplified
8×; they are not the displayed result.

- [Rendered results and exact gates](../build/asset-processor/m5-bc6h-visual/results.json)
- [Raw / BC6H / amplified difference](../build/asset-processor/m5-bc6h-visual/EnvironmentTest-raster-review.png)
- [Maximum-delta crop](../build/asset-processor/m5-bc6h-visual/EnvironmentTest-maximum-delta-crop.png)
- [Colored face / partial-chain comparison](../build/asset-processor/m5-bc6h-visual/face-orientation-raster-review.png)
- [Independent orientation fixture audit](../build/asset-processor/m5-bc6h-orientation/results.json)

Whole-renderer VMA peaks for EnvironmentTest raster fall from **208,666,624 to
141,557,760 committed bytes**, and **141,557,760 to 74,448,896 device-local bytes**.
Both GI backends remain at 653,262,848 committed and 586,153,984 device-local
bytes across raw/BC6H runs. Pool allocation granularity means these are not
per-environment allocation measurements. All runs finish with zero remaining
VMA bytes. [Memory records](../build/asset-processor/m5-bc6h-visual/memory.json)
preserve each capture's values; isolated upload/allocation benchmarking is
separate work.

`Data/engine.cfg` is restored byte-exact with SHA-256
`5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`.
The exclusive smoke lock is absent. Renderer, shaders and CPU audit identities
remain unchanged. The GPU window was released after the final capture.

## Frozen identities

All executables are under `build/asset-processor/m5-bc6h-binaries`, except the
independent decoder under `build/asset-processor/hdr-audit-tools`.

| Binary | SHA-256 |
| --- | --- |
| `zenassetc.exe` | `035aea31d547117d7415d9fee4da5fe6a29821da9e42d0c03524d684dec2d22f` |
| `asset_processor_roundtrip.exe` | `203395113f3dd37ad3bd1376c684b36f8d89ca87d7a5fae3cc08fc1b3a6d49ab` |
| `asset_processor_hdr_fixtures.exe` | `a88a6a7f897368896574119ca24d72996a06e811104d64ed0cb00ec25d1b5466` |
| `texconv.exe` | `dcfdec10244e02cf5037fba089c55fb7e1326b1c8181742d77d15fa5cb5eef06` |
| `VulkanRHIIntegrationTest.exe` | `16891e7b0cd83f8141edb8d589437c2a8d0254b41748c5ada631639e3e309ddc` |
| `scene_renderer_demo.exe` | `7265df67568bc080ec7c275cb83cff081f61d843999652c8de3d43e3c56d1be8` |
| `asset_processor_hdr_orientation.exe` | `0156b953350b6c15f67772d63fb8aa05867f91f94a50b7a4df44f8b23d3f0d47` |
