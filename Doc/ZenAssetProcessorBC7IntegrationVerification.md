# BC7 normal refinement integration checkpoint

The 2026-10-24 implementation moves the guarded filter-aware search into the
standalone processor. The actual CLI-produced clearcoat package reproduces the
diagnostic's raster, compute-GI and hardware-GI frames exactly. Production
throughput remains an open issue; this is not completion of M5 or the full plan.

The [performance follow-up](ZenAssetProcessorBC7PerformanceVerification.md)
preserves this algorithm's output while reducing search work and verifying
the rebuilt engine/editor path. This report retains the initial integration
measurements as its baseline.

## Implementation

`ZenAssetProcessor/src/Codecs/BlockEncoding.cpp` generates sixteen bounded
mode-6 candidates using direction error. Image-level sampling policy lives in
`src/Processing/BC7NormalRefinement.cpp`, called after baseline encoding and
before final quality measurement in `TextureImage.cpp`.

Refinement applies to BC7Unorm, filtered normal uses at quality 4. It keeps the
baseline block unless a candidate improves the total score while preserving
each sampled phase's squared angular-error sum, the maximum angle, and the
maximum alpha byte error over valid pixels. The guards use the same numerical
comparison epsilon as the [diagnostic](ZenAssetProcessorBC7FilterSearch.md).
The one-degree RMS and five-degree maximum quality policy remains unchanged.
Undefined bilinear directions receive the final quality gate's 180-degree
penalty. Every completed mip still passes through independent decoding and
the existing whole-image rejection path.

The implementation replaces the probe's full decoded image with at most nine
decoded neighbor blocks. A fixed 64-entry cache compares complete keys, including
valid-texel masks and the original encoded block. Hash collisions affect cost
only. Temporary storage is charged before allocation and refunded before
return; cancellation is checked during block traversal, endpoint-search passes,
and candidate scoring. Failed processing publishes no pixels.

Two reductions avoid unnecessary scoring without changing accepted candidates:
duplicate encoded candidates are skipped, and scoring stops once a partial
nonnegative error sum or maximum exceeds its acceptance bound. Quality 0–3,
nearest-only normal uses, sRGB views and other texture roles retain their
existing encoder selection. No runtime data layout, shader or public setting
changes are required. Compression remains opt-in.

## Verification

- All 35 standalone Release CTests pass. Four affected Debug suites pass.
  The final added regression cases pass in both configurations.
- The new refinement suite checks 84 combinations: seven tiny/odd dimensions,
  four combinations of repeat versus clamp edge behavior, and three alpha
  patterns. An independent whole-image accumulator verifies all sixteen phases,
  maximum angular error and alpha error. These are also the finite edge rules
  used for mirror sampling; this is not an arbitrary-coordinate sampler proof.
- Cancellation is injected at every observed polling position through public
  image processing, verifying that no image or metrics escape and the budget is
  refunded. A constrained-budget run reaches the refinement allocation failure.
- A repeated normal-ramp regression verifies an actual improvement through the
  public processing path. An opposing-direction regression verifies that an
  undefined interpolated normal retains the source image. Lower quality levels
  and nearest-only sampling retain baseline encoded bytes.
- The production CLI's clearcoat package passes package validation and the
  independent Pillow audit across its complete mip chains. Every resource
  payload matches the guarded diagnostic package except build metadata.
- Three new Vulkan validation captures load that production package. Raster,
  compute GI and hardware GI exactly reproduce the earlier guarded frames.
  Each capture reports no validation errors or memory leaks; configuration and
  shader payloads are restored/unchanged.

The inherited clearcoat comparisons remain maximum channel error 86 versus
baseline 113 for raster, and 43 versus 131 for both GI modes. All ten mips of
the refined normal remain BC7. These measurements do not define a new rendered
acceptance threshold.

Captures use the preserved Release renderer
`d077bfb76edd71cf6f83ec9c92ec8bc6b0875b7b161a508b453d4779cd7bff47`, RTX 5080,
640x480, fixed camera/time and sixteen GI frames. They use the existing compiled
SPIR-V; unrelated newer GLSL edits are not rebuilt. Engine/editor cooking
targets have not been rebuilt for this algorithm in this checkpoint.

## Cost and remaining work

Three paired sequential clearcoat cooks compare the preserved prior processor
with the integrated search, using the same input, desktop BC7 quality 4,
offline mips and two-worker setting. Pair order alternates; no builds or
renderer run concurrently. All repeated resource payloads match their respective
expected outputs except build metadata.

| Complete process wall time | Prior processor | Integrated search |
| --- | ---: | ---: |
| Median of three runs | 1.843 s | 36.985 s |

The approximately 20.1x increase is a measured offline regression on this scene,
including import, final fallback decisions and package I/O. It must be addressed
before accepting this path for production throughput. No speed benefit is claimed.

The 1024x1024 normal image that ultimately fails the quality gate has 63,324
distinct source blocks out of 65,536, so repeated-block caching is not a general
solution to search cost. Encoder throughput, broader device/scene coverage,
continuous and cross-mip filtering, and in-engine/editor recooking validation
remain required work. No hosted release or shipping profile acceptance is
claimed.

## Evidence

- [Production CLI and independent image audit](../build/asset-processor/m5-bc7-production-guard-cpu/results.json)
- [Production-package validation captures](../build/asset-processor/m5-bc7-production-visual/results.json)
- [Paired complete-cook timings](../build/asset-processor/m5-bc7-production-timing/results.json)
- [Release suite](../build/bc7-production-integration/ctest-guard-release.log)
- [Affected Debug suites](../build/bc7-production-integration/ctest-debug.log)
- [Final Release regression](../build/bc7-production-integration/ctest-regression-release.log)
- [Final Debug regression](../build/bc7-production-integration/ctest-regression-debug.log)
- [Frozen identities](../build/asset-integration-release/m5-bc7-production-evidence/identities.json)
- [Final checkpoint audit](../build/asset-integration-release/m5-bc7-production-evidence/final-audit.json)
