# BC7 normal encoding experiments

The 2026-10-24 follow-up isolates the remaining clearcoat highlight error to
normal encoding and evaluates seven alternatives to the current BC7 block
selection. The best full-chain experiment reduces raster maximum/RMSE from
113 / 0.547504 to 71 / 0.354858, but fails the existing filtered-normal limit
at mip 3. No candidate is promoted to production. M5 remains open.

## Tested boundary

The preceding [BC4/BC5 checkpoint](ZenAssetProcessorBC45PrecisionVerification.md)
showed that restoring clearcoat's `RibsNormal` removes most of the error;
decoding BC7 to equivalent RGBA8 leaves the rendered result unchanged.
This investigation changes only that image's blocks in diagnostic packages.
It keeps the other resources, image views and bindings equal, apart from
writer metadata. Re-encoding with the current strategy reproduces the original
normal resource byte for byte.

`RibsNormal` is 512x512 with X bytes 18..240, constant Y=128, Z bytes 187..255,
and alpha=255. Its base level has eight distinct 4x4 blocks, each containing
four distinct normal vectors. Some current BC7 values differ by one byte;
the constant Y channel changes in 16,384 of 262,144 base-level texels. The
constant-channel hypothesis therefore warranted a test, but fixing that channel
alone did not remove the highlight error.

The diagnostic consumer uses the installed current-schema Reader/Writer and
the already-pinned `bc7enc_rdo` implementation. Existing production settings
use uniform linear RGBA weights and quality level four. Opaque inputs follow
upstream modes 1/6; forcing the alpha path enables its modes 5/6/7 candidates.
The mode-5 experiment also rotates each channel into the independently encoded
scalar channel. Rotation is part of the
[BC7 format](https://learn.microsoft.com/en-us/windows/uwp/graphics-concepts/bc7-format),
so the package still contains ordinary BC7 blocks and requires no shader changes.

## Candidates and results

Every candidate is decoded independently through Pillow and evaluated with the
unchanged texel and fifteen-phase bilinear normal checks. The table reports
the complete mip-chain CPU gate and the same fixed raster view. Frame numbers
are descriptive, not a new acceptance threshold.

| Strategy | CPU quality gate | Raster maximum / RMSE, 8-bit channels |
| --- | --- | ---: |
| Current production baseline | Pass | 113 / 0.547504 |
| Force upstream alpha modes | Fail at mips 2, 3 | 89 / 0.677040 |
| Weight constant RGB channels by 64 | Fail at mip 2 | 113 / 0.522450 |
| Add mode-5 rotations; choose angular error | Pass | 113 / 0.549300 |
| Add mode-5 rotations; choose RGB error | Pass | 113 / 0.538521 |
| Refine mode-6 endpoints and selectors by normal direction | Pass | 108 / 0.461297 |
| Expand mode-6 endpoint search, then refine | Fail at mip 3 | 71 / 0.354858 |
| Apply expanded search only to the base mip | Pass | 71 / 0.354858 |
| Restore original normal pixels, diagnostic control | Uncompressed normal | 8 / 0.071883 |

The mode-6 experiments retain the original candidate and use angular error to
compare it with a direction-based endpoint/selector search. The expanded
experiment adds sixteen endpoint extension seeds. The base-only experiment
isolates the visible improvement from lower-mip behavior; it is not a proposed
production policy or a solution to the full-chain requirement.

The [inspected highlight comparison](../build/asset-processor/m5-bc7-normal-summary/highlight-comparison.png)
shows that the expanded search reduces differences but leaves localized errors.
The raw-normal control remains substantially closer to the reference.

## Why texel direction alone is insufficient

The expanded search improves base-level angular RMS from 0.242426 to 0.106597
degrees, and maximum error from 0.450706 to 0.314692 degrees. At mip 3 it also
improves texel RMS from 0.584257 to 0.505996 degrees. However, filtered RMS at
that mip rises from 0.649215 to **1.604106 degrees**, exceeding the unchanged
one-degree limit.

The decoded normal-vector lengths explain the failure: at mip 3 the source
lengths span 0.998292..1.000015, while the expanded candidate spans
0.794241..1.088156. Normalizing each texel when scoring it hides those length
changes. Bilinear sampling combines the unnormalized channel values before
the shader normalizes the result, so it can change the resulting direction.
The maximum vector-length change is 0.205405, versus 0.016257 for the baseline.

This rules out promoting a texel-angle-only endpoint objective. The next
encoder experiment must account for interpolated normal channels, including
block boundaries, and retain the existing mip checks. Candidate selection
also needs a broader material corpus and processing-cost measurements before
it can replace the production path. Constant-channel weighting and mode
rotation alone do not solve the observed case.

## Verification and evidence

All eight baseline/candidate packages in the final comparison validate; failed
quality candidates remain explicitly marked as failures. Thirteen raster
captures pass validation and report no memory leaks. Both campaigns reproduce
the previous reference and production-baseline captures exactly. Configurations
are restored after each campaign. No new GI, moving-camera, cross-mip or
device-coverage claim is made.

The renderer is the preserved Release binary with SHA-256
`d077bfb76edd71cf6f83ec9c92ec8bc6b0875b7b161a508b453d4779cd7bff47`, on RTX 5080
at 640x480 with a fixed front camera/time. These diagnostics use the existing
compiled SPIR-V payloads. Later GLSL source edits are recorded for provenance
and are not rebuilt by this experiment. Production engine/library code,
quality policy and shader payloads are unchanged by this investigation.

The probe caches identical input blocks to keep the repeated-pattern experiment
small. Its wall times are not representative encoder-throughput measurements.
An earlier endpoint-seed quantization attempt is retained with its sources and
binary; the corrected build supplies the expanded-search rows above.

- [Combined measurements and candidate decisions](../build/asset-processor/m5-bc7-normal-summary/results.json).
- [Initial candidates](../build/asset-processor/m5-bc7-normal-candidates/results.json),
  [local endpoint refinement](../build/asset-processor/m5-bc7-normal-refinement/results.json),
  and [corrected expanded search](../build/asset-processor/m5-bc7-normal-expanded-corrected/results.json).
- [Initial captures](../build/asset-processor/m5-bc7-normal-candidate-visual/results.json)
  and [expanded-search controls](../build/asset-processor/m5-bc7-normal-expanded-visual/results.json).
- [Diagnostic source](../build/bc7-normal-probe/main.cpp),
  [frozen identities](../build/asset-integration-release/m5-bc7-normal-search-evidence/identities.json),
  and [evidence audit](../build/asset-integration-release/m5-bc7-normal-search-evidence/final-audit.json).

This is evidence for the next M5 encoder change, not a completed compression
quality or release gate.

The subsequent [filter-aware search](ZenAssetProcessorBC7FilterSearch.md) accounts
for neighboring blocks and adds per-phase, maximum-angle and alpha-error guards.
Its clearcoat candidate passes all mips and improves raster and both GI modes.
Production integration, sampler coverage and processing-cost gates remain open.
