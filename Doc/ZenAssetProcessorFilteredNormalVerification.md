# Filtered normal-map quality verification

24 October 2026. Normal compression now checks sampled channel values before
normal reconstruction, in addition to decoded texels. This closes a confirmed
BC5 filtering gap. ClearCoatTest's new BC4/BC5 package matches raw rendering
exactly in raster, compute GI and hardware GI. **M5 remains open:** BC7 clearcoat
and BC5 shared-odd highlights still differ despite passing image-space limits.

## Finding and implementation

`MaterialNormalVector` samples texture channels before reconstructing BC5 Z.
Interpolating XY and then reconstructing positive Z does not generally match
interpolating an RGB normal. The old texel-only gate missed that distinction.
An independent NumPy audit of the previously accepted packages found:

| BC5 image, base mip | Texel RMS / maximum angle | Filtered RMS / maximum angle |
| --- | ---: | ---: |
| ClearCoatTest `RibsNormal` | 0.201107° / 0.569120° | 2.910969° / 16.431586° |
| ClearCoatTest `PlasticWrap_normals` | 0.480942° / 3.538337° | 0.450485° / 6.024332° |

The first diagnostic sampled interior cells only; it found the RibsNormal
failure. The subsequent sampler-aware audit includes repeat seams and also
finds the PlasticWrap failure. Both violate the existing **1° RMS / 5° maximum**
limits. The numerical thresholds were not changed. The
[quality policy](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json) now declares the added
sampling coverage; its file hash has changed.

The standalone processor measures 15 phase pairs from `{0, .25, .5, .75}` in
each texel cell, excluding the already checked `(0, 0)` phase. It compares
filtered stored channels, reconstructs BC5 Z where applicable, then normalizes.
Each phase and mip must meet the same limits. Repeat seams are tested; clamp
and mirror edges reuse the edge texel. Nearest-only min/mag sampling keeps
texel checks. Filter and address behavior participate in normal-use grouping
when compression is enabled. Clamp-to-border normals retain original storage
because the runtime-selected border color is outside this quality contract.

A failed mip retains the complete original image owner, including authored
mips. Shared uses can still choose different representations. Two decoded rows
use `48 × width` temporary bytes, charged before allocation and refunded on
every exit. Row-level cancellation propagates without publishing partial data.
This adds offline work; it is not a measured throughput improvement.

No package schema or public settings changed. The processor build fingerprint
changes with the implementation, invalidating processing caches.

## Verification

- Standalone Release: **34/34 suites pass**. The affected Debug suites pass
  **2/2**. Regressions cover opposite near-unit normals, 1D and odd extents,
  block boundaries, authored-tail fallback, shared nearest/linear/border uses,
  allocation refunds and cancellation at every observed callback position.
- Fresh independent BC4/BC5 audit: **15 cases, 46 mip measurements**, with five
  BC4 and two BC5 image conversions. All copied sources are absent during CLI
  validation and installed-consumer Reader round trips; resource bytes match.
- Fresh independent BC7 audit: **11 cases, 111 mip measurements**. Decoders are
  independent of the processor: NumPy for BC4/BC5 and Pillow for BC7. Retained
  image pixels and unaffected resource semantics pass their checks.
- Engine source integration rebuilt `scene_renderer_demo` and
  `EditorAssetRenderingTest` against the changed processor. **76 desktop
  captures pass validation:** 44 current BC4/BC5, 26 current BC7 and six old
  BC4/BC5 controls. All 16 source/raw pairs and six GI reference-repeat pairs
  are exact. Three old/current raw clearcoat comparisons are also exact.
- C++ formatting and scoped whitespace checks pass. Capture configuration is
  restored to SHA-256
  `5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`.

The first audit attempt used a historical roundtrip executable that did not
support newer required resource versions. That attempt stopped; its log is
retained. A fresh installed-package consumer built from the current library
then passed every source-free roundtrip. Reader validation was not weakened.

## Rendered results

Captures use the same Release renderer, fixed front camera, 640×480 output,
fixed time and RTX 5080. GI uses 16 frames and the opt-in deterministic guide
accumulator; raster uses three frames. A hash-identical `7zFM.exe` alias uses
the existing RTSS no-hook profile, as established in the
[guide capture report](ZenAssetProcessorGuideCaptureVerification.md). Global
overlay settings were not changed. Logs reject validation errors, `VUID-` and
`SYNC-HAZARD` messages. The harness now also enforces requested GI repeat equality
in its pass result, rather than only recording that comparison.

| Clearcoat BC4/BC5 | Previous maximum / RMSE, 8-bit channels | Current maximum / RMSE |
| --- | ---: | ---: |
| Raster | 169 / 1.093303 | 0 / 0 |
| Compute GI | 169 / 0.868949 | 0 / 0 |
| Hardware GI | 169 / 0.868949 | 0 / 0 |

Both clearcoat normal maps now retain raw storage. This restores fidelity at a
storage cost: the current raw package is 10,420,100 bytes and the BC4/BC5 request
produces 10,420,260 bytes, with no normal conversion. The new gate does not
claim a compression benefit for this asset.

Remaining differences are visible in the inspected
[highlight crops](../build/asset-integration-release/m5-filtered-normal-evidence/highlight-crops.png):
BC7 clearcoat reaches **113 / 0.547504** maximum/RMSE in raster and
**131 / 0.401858** in both GI modes. The BC5 shared-odd fixture reaches
**35 / 0.431743** across the three modes; about **0.0583%** of its pixels exceed
eight channel levels. These are descriptive measurements, not newly invented
frame tolerances. No lossy defaults or full visual acceptance are approved by
this checkpoint.

The subsequent [BC4/BC5 precision checkpoint](ZenAssetProcessorBC45PrecisionVerification.md)
fixes fractional endpoint scoring without changing these thresholds. Its
same-renderer controls reduce shared-odd maximum channel error from 35 to 9 in
raster and to 8 in both GI modes. The figures above remain the historical
filtered-normal checkpoint; BC7 clearcoat remains unresolved.

The phase samples do not prove continuous, anisotropic or cross-mip error
bounds. Small angular changes can still move sharp highlights. Broader material
quality, device coverage, production upload/memory measurements and the
remaining [implementation-plan gates](ZenAssetProcessorImplementationPlan.md)
remain required.

The later [adjacent-mip checkpoint](ZenAssetProcessorNormalMipVerification.md)
adds sampled checks between levels and fixes a confirmed 10.48-degree BC5 blend
error missed by individual-mip checks. The figures above remain the historical
within-level checkpoint. Continuous UV/LOD and anisotropic bounds remain open.

## Evidence

- [Combined metrics and retrospective normal measurements](../build/asset-integration-release/m5-filtered-normal-evidence/results.json)
- [Fresh BC4/BC5 audit](../build/asset-processor/m5-bc45-filtered-cpu-final/results.json)
  and [BC7 audit](../build/asset-processor/m5-bc7-filtered-cpu/results.json)
- [BC4/BC5 captures](../build/asset-processor/m5-bc45-filtered-visual/results.json),
  [BC7 captures](../build/asset-processor/m5-bc7-filtered-visual/results.json), and
  [same-renderer previous-package controls](../build/asset-processor/m5-bc45-filtered-control/results.json)
- [Release tests](../../ZenAssetProcessor/build/m5-normal-filter-release.log),
  [Debug tests](../../ZenAssetProcessor/build/m5-normal-filter-debug-tests.log), and
  [engine build](../build/asset-integration-release/m5-filtered-normal-engine-build.log)
- [Frozen identities](../build/asset-integration-release/m5-filtered-normal-evidence/identities.json)
  and [final audit](../build/asset-integration-release/m5-filtered-normal-evidence/final-audit.json)

These files preserve the successful run and earlier diagnostic failures. They
are local verification artifacts, not a published release or a completed M8 gate.
