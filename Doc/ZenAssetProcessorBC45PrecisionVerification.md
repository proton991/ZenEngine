# BC4/BC5 fractional endpoint refinement

The 2026-10-24 checkpoint fixes a precision mismatch in offline BC4/BC5
endpoint selection. On the shared-odd normal fixture, maximum rendered channel
error falls from 35 to 9 in raster and from 35 to 8 in both GI modes. The
normal remains compressed, with the same mip payload sizes. This advances M5;
the BC7 clearcoat issue and the full quality/release gates remain open.

## Cause and implementation

The pinned `rgbcx::encode_bc4_hq` evaluates candidate palettes as eight-bit
integers. ZenAsset's quality decoder uses fractional BC4/BC5 interpolation.
An integer scoring tie can therefore select endpoints that decode less
accurately than another available pair. BC4 UNORM interpolation retains the
fractional result of division by five or seven, and BC5 applies the same
operation to each channel. See the
[Microsoft format description](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression#bc4_unorm).

The fixture's 5x7 normal map has X bytes 124 through 128, Y bytes 125 through
131, and Z=255. Previously decoded X values included 124.4286, 125.2857 and
126.1429, even though the input ramp is exactly representable. The refined
encoding recovers the original XY values under independent fractional decoding.

The change is confined to the sibling library's
[block encoder](../../ZenAssetProcessor/src/Codecs/BlockEncoding.cpp), its
[internal contract](../../ZenAssetProcessor/src/Codecs/BlockEncoding.h), and
[regression test](../../ZenAssetProcessor/tests/BlockCodecTests.cpp).
After upstream encoding, a bounded search scores the original endpoints and
the existing radius-three min/max neighborhood in both interpolation modes.
Scaling byte values by 35 represents both denominators exactly in integer
arithmetic. Each candidate selects the nearest palette entry for every texel;
only a strictly lower squared error replaces the current result.

This cannot increase fractional per-channel squared error relative to the
upstream block. It does not establish a universal angular, filtered or rendered
error bound. Existing image and normal quality gates still apply. The extra
offline search uses stack storage; its throughput cost has not been established
by this checkpoint. There is no cooking-speed claim.

The upstream pin remains `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0`.
BC7/BC6H encoding, package schema, public settings, decoder, shaders and numeric
quality policy are unchanged. Both library and engine builds carry fingerprint
`4f6ed9a518262a717e77dc2091227fdb9135133b4cf925fd90c4a1a749371b88`.

## Image attribution controls

An installed Reader/Writer consumer replaced one image at a time while the
audit checked that unrelated resources, bindings and image views stayed equal.
Sixteen validation captures isolate the previously observed differences:

- Restoring clearcoat's `RibsNormal` pixels reduces raster maximum/RMSE from
  113 / 0.547504 to 8 / 0.071883. Restoring roughness leaves the frame equal
  to the cooked frame; restoring labels or coating leaves the maximum at 113.
- Decoding `RibsNormal` BC7 into RGBA8, retaining its decoded values and mips,
  produces a frame exactly equal to the BC7 frame. Decoding all four BC7 images
  also produces an equal frame. This attributes the dominant error in this
  view to encoded normal values rather than the compressed upload/filter path.
- Restoring the shared-odd normal reduces maximum/RMSE from 35 / 0.433058 to
  1 / 0.038878; restoring only occlusion leaves the maximum at 35.
- Original single-level and raw mip-chain normal replacements give the same
  results in these views. This does not bound mip behavior in other views.

The probe uses the previous installed package, which supports the current
schema. It is diagnostic tooling under `build/texture-highlight-probe`; it
does not introduce runtime production paths. The attribution run used the
pre-refinement renderer. Subsequent old/new package controls use one rebuilt
renderer for both variants.

## Verification and rendered results

The new regression covers four-value ramps at all 253 byte-range positions,
including boundaries, with independent X/Y directions for BC5. It fails against
the previous installed encoder (`narrowBC45=0`) and passes with the refinement.
All **34 Release CTest suites**, **three affected Debug suites**, and **four
editor cooked-array rendering tests** pass. Formatting also passes. The engine
build succeeds with the existing `GLM_ENABLE_EXPERIMENTAL` redefinition warning.

The independent CPU audit passes **15 cases and 46 compressed mip measurements**:
five images become BC4 and two become BC5, matching the previous conversion
counts and GPU mip payload bytes. Raw and mip-only control pixels remain equal
to the previous audit. Packages validate and survive installed Reader/Writer
round trips with copied source inputs hidden. This reuses the current-schema
installed reader; it is not a new installation/release test.

| Shared-odd base mip, degrees | Previous RMS / maximum | Refined RMS / maximum |
| --- | ---: | ---: |
| Texel normal angle | 0.177431 / 0.320809 | 0.000567 / 0.001676 |
| Fifteen bilinear phase samples | 0.172207 / 0.311514 | 0.000501 / 0.001505 |

The fresh visual campaign has 44 validation captures: six raster cases plus
clearcoat and shared-odd in compute and hardware GI. All ten source/reference
pairs and four GI reference repeats are exact. Ten additional controls alternate
old/new packages using the same renderer. Three repetitions of each raster
variant agree exactly; new controls in all three modes agree with the fresh
campaign captures. Combined with attribution, there are **70 validation
captures**, without validation errors or reported leaks.

| Shared-odd render, 8-bit channels | Previous maximum / RMSE | Refined maximum / RMSE |
| --- | ---: | ---: |
| Raster | 35 / 0.433058 | 9 / 0.114806 |
| Compute GI | 35 / 0.431743 | 8 / 0.115268 |
| Hardware GI | 35 / 0.431743 | 8 / 0.115268 |

The inspected [fixed highlight crop](../build/asset-processor/m5-bc45-precision-summary/shared-normal-comparison.png)
shows the reduced difference. One of 307,200 raster pixels still exceeds eight
channel levels; neither GI image has a pixel above eight. These are descriptive
measurements, not a new frame tolerance or visual acceptance threshold.
Clearcoat BC4/BC5 retains raw normals and stays exact in all three modes.
Clearcoat BC7 encoding is unchanged; its remaining highlight error is unresolved.

Captures use Windows Release, RTX 5080, 640x480, fixed camera/time, three raster
frames and sixteen GI frames with the existing deterministic guide option.
The hash-identical `7zFM.exe` alias uses the existing RTSS no-hook profile.
The current renderer SHA-256 is
`d077bfb76edd71cf6f83ec9c92ec8bc6b0875b7b161a508b453d4779cd7bff47`.
Configuration is restored and the capture lock is absent.

## Evidence and remaining scope

- [Combined measurements](../build/asset-processor/m5-bc45-precision-summary/results.json),
  [image attribution](../build/asset-processor/m5-highlight-attribution/results.json),
  [CPU audit](../build/asset-processor/m5-bc45-precision-cpu/results.json),
  [visual campaign](../build/asset-processor/m5-bc45-precision-visual/results.json), and
  [same-renderer controls](../build/asset-processor/m5-bc45-precision-controls/results.json).
- [Release tests](../../ZenAssetProcessor/build/m5-bc45-precision-release-tests.log),
  [Debug tests](../../ZenAssetProcessor/build/m5-bc45-precision-debug-tests.log), and
  [editor tests](../build/asset-integration-release/m5-bc45-precision-editor.xml).
- [Scoped source diff](../build/asset-integration-release/m5-bc45-precision-evidence/changes.diff),
  [frozen identities](../build/asset-integration-release/m5-bc45-precision-evidence/identities.json), and
  [evidence audit](../build/asset-integration-release/m5-bc45-precision-evidence/final-audit.json).

The evidence archive preserves sources, binaries, scripts, logs, configurations
and captures, and checks referenced package/source identities. Historical
binary identities are resolved to preserved binaries, not the overwritten build
paths. This is local evidence, not a published library package.

At evidence finalization, `probe_place.comp` and `probe_temporal.comp` had
changed after the captures. Matching earlier snapshots preserve their tested
contents, alongside the matching SPIR-V payloads. The newer edits are untouched.
The capture results apply to that saved shader set; this checkpoint does not
claim a rendering regression run of the later shader sources.

Broader materials/devices, continuous and cross-mip filtering, encoder throughput,
the unresolved BC7 highlights, and the remaining
[implementation-plan gates](ZenAssetProcessorImplementationPlan.md) still require
work. This checkpoint changes neither lossy defaults nor release acceptance.

The subsequent [BC7 normal search](ZenAssetProcessorBC7NormalSearch.md) evaluates
the remaining clearcoat error. Its best full-chain candidate reduces raster
error but fails a filtered mip check; it remains experimental.
