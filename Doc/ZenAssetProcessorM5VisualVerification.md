# M5 BC7 texture verification

The subsequent [filtered-normal checkpoint](ZenAssetProcessorFilteredNormalVerification.md)
adds sampler-aware interpolation checks without changing numerical limits.
Fresh BC7 CPU and exact reference/repeat gates pass; the clearcoat highlight
difference persists. That report records the current results separately.

Follow-up: the [exact guide capture checkpoint](ZenAssetProcessorGuideCaptureVerification.md)
now passes the focused source/raw GI and identical-package repeat gates with
learning retained. The clearcoat highlight difference below remains, so this
does not close full lossy visual acceptance. The original failed runs are retained.

23 October 2026. The first BC7 slice passes its independent image/mip checks,
package/fallback audit, Vulkan validation and raster reference parity. **Full
rendered acceptance remains open:** identical raw-package GI captures are not
bit-exact, and BC7 normals shift some sharp glossy highlights despite passing
the image angular limits. This evidence does not enable desktop cooking by
default or claim the remaining BC4/BC5/BC6H work is complete.

## Contract fixed before measurement

The existing [quality policy](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json) remains
unchanged. Source versus raw package and the seven frozen M0 views require zero
8-bit channel error. Every accepted compressed image and mip must satisfy its
role's linear RGB, scalar, normal, alpha and finite-value limits. There is no
desktop frame threshold in that policy; BC7 frame metrics below are descriptive
and are accompanied by visual review. No threshold was inferred or relaxed
after measuring the images.

Each input produced three packages using one frozen CLI: reference, explicit
desktop mip-only RGBA, and explicit desktop mip generation plus BC7. Transport
and mesh encoding were disabled to isolate the texture processing. Geometry
quantization and atlasing were disabled. The mip-only package separates mip
filtering effects from BC7 encoding and whole-image fallback.

## Frozen inputs and repeatable tools

Frozen binaries are in `build/asset-processor/m5-texture-binaries`:

| Binary | SHA-256 |
| --- | --- |
| `zenassetc.exe` | `53319d9afe6af1a4b35d6b17e642bc90389c7450cf089d956d6720a161018500` |
| `scene_renderer_demo.exe` | `eb406e4b5f616a9e61578b56bd9ab5627e628b8793f20158e688911a751e37be` |

The renderer was built in the engine Release integration directory after the
BC7 block validator and normal-scale exclusions landed. Build output is in
`build/asset-processor/m5-renderer-build.log`. The parent separately recorded
the full standalone 25/25 CTest pass before CLI freeze.

- [CPU cooking and independent audit](../../ZenAssetProcessor/tools/validation/asset_processor_texture_audit.py)
  parses validated raw package fields and decodes BC7 with Pillow 12.3.0,
  independently of the cooking encoder and bcdec quality decoder. It checks
  stable material bindings, unaffected payloads, actual formats, every mip
  footprint, duplicate base/mip-zero bytes, and original fallback pixels.
- [Capture harness](../../ZenAssetProcessor/tools/validation/asset_processor_texture_visual.py) uses the existing
  renderer with fixed time, no UI, no asset cache, 640x480 output and Vulkan plus
  synchronization validation. Raster uses three frames; GI uses 16 frames.
- [Memory/report helper](../../ZenAssetProcessor/tools/validation/asset_processor_texture_report.py) summarizes
  package ownership, calculated upload/mip footprints and measured VMA logs.

The bundled Python runtime supplies NumPy and Pillow. CPU evidence is
[m5-texture-cpu/results.json](../build/asset-processor/m5-texture-cpu/results.json),
including CLI arguments, package/source hashes, each image/mip result, process
memory and cook times. `dependencies.json` records glTF external dependencies.
The [machine identity record](../build/asset-processor/m5-texture-identity.json)
records Windows 11 build 26300, Ryzen 9 9950X3D, RTX 5080 driver 32.0.16.1742,
MSVC 14.51.36231, two cooking workers and uncontrolled warm OS-cache conditions.
All three packages remain beside each source case. The original audit and
capture scripts used for the first runs were copied into their evidence folders.

Raster and GI evidence is split between
[initial views](../build/asset-processor/m5-texture-visual/results.json) and
[focused front views](../build/asset-processor/m5-texture-focused/results.json).
The first focused views were too oblique for useful material inspection, so the
second batch uses `camera_position=0,0,1.2` after engine scene normalization.
The earlier results were preserved. Both batches record the full shader and
SPIR-V identity lists; neither changed during capture. The configuration lock,
external-edit guard and exact-byte restoration succeeded in both batches.
Both saved originals and the restored `Data/engine.cfg` have SHA-256
`5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`;
the smoke lock is absent. All recorded glTF dependency hashes still match.
No GI algorithm, shader or user GI source was edited.

## Package and per-image results

All 11 scene audits passed. The packages contain **12 BC7 images with 111
independently measured mip/role records**, and 14 retained RGBA images. Every
accepted BC7 image has a complete mip chain. Quality rejection preserves the
whole authored image, including its original mip behavior; a failing mip is
not mixed into a partly compressed image.

Across the measured accepted payloads: maximum linear RGB RMSE was 0.007637,
minimum finite PSNR was 42.3425 dB, maximum scalar error was 8/255, maximum normal
RMS angle was 0.7287 degrees, and maximum normal angle was 1.0904 degrees. These
aggregate extrema supplement, rather than replace, the per-image/mip records.
Zero-error PSNR is represented as JSON null with RMSE zero.

| Scene | BC7 images | Retained RGBA | Reference package bytes | BC7 package bytes |
| --- | ---: | ---: | ---: | ---: |
| DamagedHelmet | 1 | 4 | 86,119,089 | 79,129,025 |
| SimpleSkin | 0 | 0 | 10,442 | 10,442 |
| AnimatedMorphCube | 0 | 0 | 15,237 | 15,221 |
| SimpleInstancing | 0 | 0 | 40,541 | 40,541 |
| EnvironmentTest / IBL | 2 | 0 | 12,742,483 | 11,868,883 |
| FBX sausage | 0 | 0 | 208,542 | 208,542 |
| FBX embedded textures | 1 | 2 | 204,686 | 177,662 |
| AlphaBlendModeTest | 1 | 3 | 54,594,732 | 47,605,468 |
| NormalTangentTest | 0 | 3 | 50,991,875 | 50,992,115 |
| ClearCoatTest | 4 | 2 | 10,419,732 | 9,328,036 |
| Generated shared color/data/normal fixture | 3 | 0 | 31,211 | 44,075 |

The normal corpus image rejection, strict alpha-mask mip rejection and exact
blend-alpha retention are intentional, with reasons in cook logs and package
metadata. The shared-role fixture proves actual independent BC7 sRGB color,
linear data and linear normal variants. Unit-scale normals are the only normal
case admitted by this slice: non-unit or animated core/clearcoat scales retain
authored pixels. This exclusion followed a demonstrated probe where a unit-scale
0.635-degree encoding error became about 44 degrees after shader XY scale 100.
Focused classifier tests cover both scales and both animation properties.

## Rendering and visual review

All **96 renderer invocations** completed successfully without validation
errors, synchronization hazards or remaining VMA allocations. All **15 raster
source/raw view pairs** are byte-identical, including all seven frozen M0
comparisons. The device was NVIDIA GeForce RTX 5080. Hardware GI logs explicitly
report `hardware_ready`; compute cases disable hardware ray tracing.

| Raster view | BC7 max 8-bit channel delta | RMSE in 8-bit units | Changed pixels |
| --- | ---: | ---: | ---: |
| DamagedHelmet / M0 view | 10 | 0.1265 | 0.7357% |
| EnvironmentTest / M0 view | 2 | 0.0727 | 1.2939% |
| Other five M0 views | 0 | 0 | 0% |
| AlphaBlendModeTest / front | 3 | 0.0788 | 1.4961% |
| NormalTangentTest / front | 0 | 0 | 0% |
| ClearCoatTest / front | 113 | 0.5475 | 1.4326% |
| Shared-role fixture / front | 0 | 0 | 0% |

Inspection of the [front-view contact sheet](../build/asset-processor/m5-texture-focused/review-contact-sheet.png)
and original-size images found preserved scene layout, material placement,
cutout coverage and shared-role interpretation in these views. There are local
highlight changes on ClearCoatTest's glossy normal-map cases. Its frame PSNR is
53.3631 dB and 0.0752% of pixels have a channel delta above 8, but those averages
do not erase the maximum delta. The
[magnified maximum-delta region](../build/asset-processor/m5-texture-focused/clearcoat-max-delta-crop.png)
shows the local highlight shift. The mip-only/reference comparison is exact
for this view; the change appears with accepted BC7 normal encoding. This needs
a deliberate desktop visual acceptance decision, not automatic threshold tuning.

The four focused compute/hardware GI source/raw pairs failed the exact gate.
Repeating the **same raw package** with identical configuration also changed
443–611 channels for the alpha fixture and 189–192 channels for the shared-role
fixture; every changed channel differed by one. This establishes run-to-run
variation independent of BC7. The reports intentionally retain
`automated_checks_passed: false`. GI output validation and upload succeeded;
exact GI visual parity remains unverified until deterministic capture is
established. No GI implementation changes were made to address that separate
issue.

## Memory and timing evidence

[m5-texture-memory.json](../build/asset-processor/m5-texture-memory.json) separates
three quantities: owned image bytes (including the schema's duplicated base
level), calculated per-texture upload/GPU mip footprints, and actual whole-run
VMA allocation logs. The engine creates a GPU texture per texture record, so
role splitting and retained fallback views can duplicate GPU storage even when
their CPU image owner is shared.

| Scene | Reference calculated GPU mip bytes | BC7 calculated GPU mip bytes |
| --- | ---: | ---: |
| DamagedHelmet | 111,848,100 | 95,070,912 |
| EnvironmentTest | 2,796,200 | 699,104 |
| FBX embedded textures | 262,140 | 196,632 |
| AlphaBlendModeTest | 72,701,264 | 78,293,692 |
| NormalTangentTest | 67,108,860 | 67,108,860 |
| ClearCoatTest | 9,437,176 | 6,815,848 |
| Shared-role fixture | 43,688 | 16,464 |

AlphaBlendModeTest grows from four to eight texture records because distinct
opacity uses retain independent variants even when quality fallback shares the
same original image. Its calculated GPU footprint increases about 7.7% despite
the smaller package. The tiny shared-role package also grows due to independent
role/mip owners. These are remaining memory tradeoffs, not universal savings.

VMA peak allocations usually occupy the same allocator-sized blocks for raw and
BC7: for example DamagedHelmet records 412,614,656 committed bytes and
278,396,928 device-local bytes for both. All runs end with zero remaining bytes.
The calculated footprint reductions must not be presented as measured VMA
reductions. Default textures, environment maps, allocator padding and pooled
staging reservation are excluded from the calculated authored texture table.

Cook runs are single diagnostic samples, not performance benchmarks:
DamagedHelmet BC7 took 7.734 seconds with sampled peak working set 141,070,336
bytes; AlphaBlendModeTest took 7.203 seconds with 102,383,616 bytes. All samples,
publication log times and CPU/private memory observations are retained. The
policy's repeated cold/warm performance campaign, isolated GPU upload execution
time and per-resource native allocation accounting remain unverified here.
