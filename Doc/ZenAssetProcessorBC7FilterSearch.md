# Filter-aware BC7 normal search checkpoint

The 2026-10-24 follow-up produces a candidate that improves clearcoat while
passing the complete existing mip-quality checks. With guards on sampled
angular and alpha error, maximum rendered channel error falls from 113 to 86
in raster and from 131 to 43 in both GI modes. This is a diagnostic algorithm
ready for integration work; production library code and defaults are unchanged.

The later [production integration checkpoint](ZenAssetProcessorBC7IntegrationVerification.md)
adds bounded scratch storage, cancellation, sampler-edge handling and CLI/runtime
verification. This report preserves the preceding diagnostic evidence.

## Method and ownership

The [previous normal search](ZenAssetProcessorBC7NormalSearch.md) improved texel
directions but failed mip 3 after interpolation. This search scores every
bilinear cell affected by a block, using original source channels and currently
decoded neighboring blocks. It samples the sixteen pairs from
`{0, .25, .5, .75}` in each axis, including texels. Channels are interpolated
before normalizing their resulting directions.

The diagnostic supports repeat wrapping, including cells across block and
texture edges. It deduplicates wrapped anchors on tiny textures; padded texels
do not participate in alpha-error checks. An independent audit compares the
sum of local error changes with whole-image measurements, checking the boundary
accounting rather than trusting the optimizer's own score.

Candidate generation retains the upstream block and considers sixteen mode-6
endpoint extension seeds with bounded coordinate refinement. One experiment
uses the filter score during refinement; a cheaper one uses it to choose among
direction-optimized candidates. The final candidate adds three acceptance guards:

- No increase in squared angular-error sum at any of the sixteen phases.
- No increase in maximum angular error across those samples.
- No increase in maximum alpha byte error over valid texels.

Angular comparisons allow `1e-12` for floating-point accumulation in squared
radians. This is a numerical comparison tolerance; the existing one-degree RMS
and five-degree maximum policy limits are unchanged. Each accepted block updates
the neighborhood for subsequent blocks. Decreasing the combined score alone
does not preserve every maximum; the guarded selection addresses that issue.
It preserves the tested baseline bounds without making failing images acceptable.
Final independent whole-mip checks still apply.

The result remains ordinary BC7 with unchanged dimensions, mip payload sizes,
bindings and non-target resources. No runtime representation or shader changes
are involved.

## Results

| Clearcoat, 8-bit channels | Baseline maximum / RMSE | Guarded candidate maximum / RMSE |
| --- | ---: | ---: |
| Raster | 113 / 0.547504 | 86 / 0.336491 |
| Compute GI | 131 / 0.401858 | 43 / 0.181740 |
| Hardware GI | 131 / 0.401858 | 43 / 0.181740 |

The [inspected raster crop](../build/asset-processor/m5-bc7-filter-summary/comparison.png)
still contains localized differences. These are descriptive measurements, not
a new frame threshold or completed visual acceptance.

Clearcoat base-level texel angular RMS improves from 0.242426 to 0.142890
degrees. Filtered RMS improves from 0.242426 to 0.142849 degrees; filtered
maximum stays at 0.567864 degrees. All ten mips pass. Mip 3 and smaller payloads
remain byte-identical to the baseline, avoiding the previous filtered-error
regression. The complete normal chain remains compressed.

The guarded matrix covers eleven cases and forty mip levels: 1x1, 1x7, 2x3,
3x5, 4x4, 5x7, 9x13, 64x64, 128x128, plus constant and varying alpha on 5x7
images. All per-phase energy, combined maximum-angle and valid alpha-error
comparisons pass. The seven baseline quality passes remain passes; four already
failing cases remain rejected. Clearcoat adds ten mip-level accounting checks.
Cached and uncached clearcoat runs produce identical package resource payloads.

Seventeen validation captures pass: four from the first filter comparison and
thirteen from the guarded comparison. The latter includes two exact GI reference
repeats. Raster and GI reference/baseline frames reproduce the previous campaign
exactly. All captures report no validation errors or leaks, and configurations
are restored.

Captures use the preserved Windows Release renderer
`d077bfb76edd71cf6f83ec9c92ec8bc6b0875b7b161a508b453d4779cd7bff47`, RTX 5080,
640x480, fixed camera/time, and sixteen GI frames with deterministic guide
accumulation. Compiled SPIR-V payloads are fixed. Later GLSL source edits are
recorded for provenance and are not rebuilt by these diagnostics.

## Cost and integration work still required

The repeated-pattern clearcoat diagnostic takes 0.594 seconds with context
caching and 11.453 seconds without it, with identical output. On the less
repetitive 128x128 synthetic case, the guarded run takes 0.812 seconds; its
baseline took 0.047 seconds. Full filter-scored endpoint search took 6.657
seconds on that case, versus 0.907 seconds for selection by combined filter
score. These are single diagnostic process timings including package I/O,
not an encoder-throughput acceptance benchmark.

The probe keeps a decoded image and a context cache capped at 4,096 entries.
Those allocations are not integrated with the processor's working-memory
budget or cancellation. Production integration must fit that contract, cover
the supported sampler modes, preserve other texture roles and presets, and
measure representative processing costs. Add candidate selection as private
processor code, preserving source-record and renderer ownership; do not copy
the diagnostic command-line program into the library. Continuous, anisotropic
and cross-mip sampling remain outside this finite phase audit.

## Evidence

- [Combined results](../build/asset-processor/m5-bc7-filter-summary/results.json).
- [First filter search](../build/asset-processor/m5-bc7-filter-search/results.json),
  [cheaper selection](../build/asset-processor/m5-bc7-filter-selection/results.json),
  and [final guarded/uncached runs](../build/asset-processor/m5-bc7-filter-guarded-final/results.json).
- [Independent whole-image accounting](../build/asset-processor/m5-bc7-filter-audit/results.json)
  and [guarded matrix](../build/asset-processor/m5-bc7-filter-guarded-audit/results.json).
- [Initial captures](../build/asset-processor/m5-bc7-filter-search-visual/results.json)
  and [guarded raster/GI captures](../build/asset-processor/m5-bc7-filter-guarded-visual/results.json).
- [Diagnostic implementation](../build/bc7-filter-probe/main.cpp),
  [filter scorer](../build/bc7-filter-probe/FilterContext.h),
  [frozen identities](../build/asset-integration-release/m5-bc7-filter-search-evidence/identities.json),
  and [evidence audit](../build/asset-integration-release/m5-bc7-filter-search-evidence/final-audit.json).

The complete M5 and release gates in the
[implementation plan](ZenAssetProcessorImplementationPlan.md) remain open.
