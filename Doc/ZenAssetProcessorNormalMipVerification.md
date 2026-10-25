# Normal-map quality between mip levels

The 2026-10-24 follow-up closes a confirmed gap in BC5 normal-map qualification:
individually accurate mip levels can exceed the existing angular limits when
their channels are blended. The processor now checks sampled adjacent-mip
transitions for BC5 and BC7 normals. Numeric limits remain **1 degree RMS and
5 degrees maximum**; unsuitable uses retain the original image and authored mips.
The overall M5 and implementation-plan gates remain open.

## Confirmed failure

An 8x8 authored normal chain has constant RGBA `(216,179,203,255)` at its base
and `(39,179,203,255)` at its 4x4, 2x2 and 1x1 levels. The previous installed
processor accepts BC5 encoding. Each mip passes texel and bilinear checks with
approximately 0.171 degrees of error, but blending the first two levels gives:

| Coarser-level weight | Angular error |
| --- | ---: |
| 0.25 | 9.788475 degrees |
| 0.50 | 10.475082 degrees |
| 0.75 | 9.788477 degrees |

The source blends RGB channels; BC5 blends XY and reconstructs positive Z
afterward. Individual-level checks cannot bound that nonlinear difference.
The baseline probe uses the preserved pre-change private header and installed
library, and records the accepted image and every per-level metric.

Vulkan applies the selected texel filter within each level and then blends the
level results for linear mip filtering. The probe and checker therefore blend
stored channels before shader-style normal reconstruction, following the
[Vulkan sampling specification](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/chapters/textures.adoc).
This CPU model uses ideal filter weights; it does not establish every device's
coordinate/filter precision.

## Implementation

For every adjacent mip pair used by a linearly mip-filtered normal binding,
`TextureImage.cpp` samples both levels at the same normalized UV. The grid uses
each finer texel cell with sixteen phase pairs from `{0,0.25,0.5,0.75}`.
Each level uses the authored minification filter, including nearest selection.
The two channel samples blend at fractions `{0.25,0.5,0.75}` before BC5 Z
reconstruction or RGB normalization. Each of the 48 phase/fraction combinations
has its own RMS limit, and the maximum angle spans all samples.

Repeat seams are included. The tested clamp/mirror edge footprint uses edge
texels; runtime-selected border colors remain excluded by the existing rule.
Nearest-mip and non-mipmapped uses skip the new check. Classification now
distinguishes minification and mip filtering independently, so shared source
images can retain compression for a nearest-mip binding while preserving raw
pixels for an otherwise identical linearly blended binding.

Quality metrics belong to the coarser level. A failed pair follows the existing
whole-image fallback path; no partially compressed chain is published. Undefined
interpolated directions receive the existing 180-degree penalty. Error and
cancellation paths discard candidates and refund temporary storage.

The checker allocates two decoded rows from each mip, charging
`48 * (fineWidth + coarseWidth)` bytes before allocation. Offsets into this
storage use address-sized arithmetic. Cancellation is checked per row. This
adds offline processing; it changes no package schema, public setting or runtime
shader. The processor build fingerprint invalidates previous processing caches.

The [quality policy](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json) now declares the
additional mip fractions and sampler behavior. Existing numerical thresholds
are unchanged. Both independent package auditors now evaluate these samples
and include sampler identity when deduplicating normal-use checks.

## Verification

- All **35 Release** CTest suites and **three affected Debug** suites pass.
  New regressions cover the failing authored chain at square, odd and 1D
  extents, nearest/linear texel filters, nearest-mip controls, a BC7 undefined
  blend, shared sampler uses, allocation failure and cancellation at every
  observed callback position.
- An installed-package consumer exports **192 cases and 360 mip pairs** across
  eight dimensions, BC5/BC7/BC7-sRGB, both texel filters and four repeat/clamp
  combinations. Independent NumPy/Pillow decoding and separable sampling agree
  with processor metrics within **0.000008523 degrees**. The comparison tolerance
  is 0.0001 degrees for decoder precision, not an increased asset-quality limit.
  Loose limits expose all metrics in this diagnostic only.
- Fresh CLI audits pass **11 BC7 cases / 111 mip measurements** and
  **15 BC4/BC5 cases / 46 mip measurements**. Their accepted normal uses exercise
  six and eight adjacent pairs respectively; many real-model samplers select
  nearest mips. BC4/BC5 packages also pass source-absent validation and Reader
  round trips, with unchanged resource bytes.
- An explicit trilinear ClearCoatTest variant exercises another **nine**
  compressed-normal pairs. Both the old and new packages pass the expanded
  audit, with worst RMS **0.492085 degrees** and maximum **0.809460 degrees**.
  The variant changes only the copied source's minification sampler setting.
  Its original source remains untouched.
- The final row-offset arithmetic change is followed by rebuilt tests and
  installed consumers. All **26** final CLI cooked packages match the expanded
  corpus's audited payloads except build metadata. The original audited source
  snapshot and installed executable remain preserved separately.
- The final engine and editor builds have the same source fingerprint as the
  standalone library. All **50 engine tests and six native editor tests** run
  and pass, including actual CLI/editor recook equality for the four-mip BC7
  normal fixture, publication rollback and upload failure. There are no Vulkan
  validation hazards or CPU/VMA leaks; eight expected injected-failure messages
  are checked by test name and count.

Native editor checks use the current shader payloads. No new full-frame corpus
comparison or GPU sampling-precision campaign is claimed here. The trilinear
variant's one-run cook times (17.594 seconds before and 21.344 seconds after)
are diagnostics, not a repeated benchmark or shipping-throughput acceptance.

## Evidence and remaining work

Final standalone/engine source fingerprint:
`6d13d0abb5d7624e8be26a763af7838e7021b38014db71ea6fa04e570c33ee2e`.
Final installed CLI SHA-256:
`dde8696c4652da99ff9389e7acb3bf839d4d113d15a986b428c58f60b919f3a0`.
The first expanded-corpus checkpoint uses fingerprint
`0a911207c0efaee2beabb2896101d87e19355586aef9bfeb082937be50d9c217`;
its only subsequent production change widens row-offset arithmetic.

Probe source, independent measurements and logs are under
`build/normal-mip-probe`. Fresh corpus reports are under
`build/asset-processor/m5-normal-mip-bc7`, `m5-normal-mip-bc45`,
`m5-normal-mip-final` and `m5-normal-mip-final-editor`.
The [frozen identities](../build/asset-integration-release/m5-normal-mip-evidence/identities.json)
preserve tested binaries, sources, packages, policy and reports. The
[checkpoint audit](../build/asset-integration-release/m5-normal-mip-evidence/final-audit.json)
verifies those results and explicitly leaves full-plan completion false.

The finite phase/fraction grid does not prove continuous UV/LOD, anisotropic
or device-independent angular bounds. Rendered quality can remain sensitive
to small normal changes. Broader material/device acceptance, optional atlas
workload benefit, shipping throughput and release distribution remain open.
