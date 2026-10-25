# ZenAssetProcessor M4 lossless transport visual verification

**Date:** 23 October 2026.
**Result:** All seven direct-source/Zstandard-package pairs and all seven
frozen-M0 comparisons are pixel-identical: zero differing RGB channels, zero
RMSE, successful process exits, and no validation errors. This verifies the
tested lossless transport path, not the entire M4 milestone.

The [harness](../../ZenAssetProcessor/tools/validation/asset_processor_visual_parity.py) uses the corrected M3
renderer, fixed-step settings and seven visible reference scenes described in
[M3 verification](ZenAssetProcessorM3Verification.md). It selects
`--compression zstd` when cooking, and explicitly disables the automatic asset
cache when rendering the source. Inspection of each package directory confirms
that it actually contains Zstandard chunks. The writer retained raw chunks
where encoding did not save space.

The original engine configuration was restored, no external configuration edit
was overwritten, and both executable hashes remained unchanged throughout.
The machine selected NVIDIA GeForce RTX 5080. All captures and process logs
remain in [the visual result directory](../build/asset-processor/m4-zstd-visual-parity/results.json).

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_visual_parity.py `
  --renderer build/asset-processor/m4-zstd-visual-binaries/scene_renderer_demo.exe `
  --processor build/asset-processor/m4-zstd-visual-binaries/zenassetc.exe `
  --reference-dir build/asset-processor/fresh-verification `
  --output build/asset-processor/m4-zstd-visual-parity `
  --compression zstd
```

The binaries were copied to the verification directory before running:

- Renderer SHA-256:
  `3881221525d2c0eaa03e197763cb8c0f09099b5ee95b75a16ab09ca01ca9591b`.
- Processor SHA-256:
  `c3c576c63dd7d257b7ab3d360f056397770f30c540966c543bd816b8075fdee2`.

Raw packages for the size comparison were cooked with that same processor,
source, and worker count; only transport selection changed. These are complete
package sizes, including directory and metadata, not GPU allocation sizes.
See [paired size evidence](../build/asset-processor/m4-zstd-visual-parity/raw-vs-zstd-sizes.json)
for commands, hashes, and chunk counts.

| Asset | Raw bytes | Zstandard bytes | Percentage of raw | Zstandard / raw chunks |
| --- | ---: | ---: | ---: | ---: |
| DamagedHelmet | 86,119,041 | 21,944,220 | 25.48% | 14 / 7 |
| SimpleSkin | 10,394 | 3,284 | 31.60% | 14 / 1 |
| AnimatedMorphCube | 15,189 | 4,269 | 28.11% | 11 / 1 |
| SimpleInstancing | 40,493 | 21,628 | 53.41% | 133 / 1 |
| EnvironmentTest IBL | 12,742,435 | 1,860,309 | 14.60% | 21 / 5 |
| FBX sausage | 208,494 | 23,803 | 11.42% | 18 / 1 |
| FBX embedded textures | 204,638 | 10,619 | 5.19% | 13 / 5 |

The raw run and its binaries remain separately preserved in
[m3-visual-parity-fixed](../build/asset-processor/m3-visual-parity-fixed/results.json).
The normal-rounding defect found before that run was fixed and has a dedicated
bit-exact regression; no thresholds were loosened.

## Meshoptimizer plus Zstandard follow-up

A rebuilt renderer and refreshed CLI also passed the same seven cases with
`--compression zstd --mesh-compression meshopt`: all seven source/package and
all seven frozen-M0 comparisons were pixel-identical and validation-clean.
Configuration restoration and unchanged executable identities passed again.
See [mesh transport visual report](../build/asset-processor/m4-mesh-visual-parity/results.json)
and [directory encoding audit](../build/asset-processor/m4-mesh-visual-parity/encoding-audit.json).
All seven vertex streams used encoding 1 and all seven index streams encoding
2; the test did not accidentally exercise only raw stream fallback.

The frozen binaries are under `build/asset-processor/m4-mesh-visual-binaries`:
renderer SHA-256 `2d6bf2d7758412805074b7e7b0859b566f9dd3d313b6416a0873e0f421f38bd4`,
CLI SHA-256 `3a5faee37597f6d5c07a0163b2b926257767aa7ecab1ba80d4f5bc7da1418c09`.
The result file retains the complete rerun command and per-case sizes. The size
table above belongs to the earlier transport-only comparison; it must not be
relabeled as meshoptimizer gains. This follow-up establishes visual semantics,
not remapping, quantization or a universal size/performance improvement.

This result covers the recorded view and animation time for these seven scenes.
It does not establish exhaustive animation equivalence, malformed-codec safety,
meshoptimizer remapping, quantization, BC texture quality, or atlas behavior.
The retained host publication times are individual observations, not a
performance distribution or GPU upload execution measurements. Those other
contracts need their own tests and measurements.
