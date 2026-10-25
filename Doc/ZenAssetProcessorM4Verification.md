# ZenAssetProcessor M4 verification

**Updated:** 23 October 2026. Lossless mesh/chunk storage and the fetch-remapping
corpus gate are verified. Quantization has independent numerical and capture
evidence for seven cases. This is
not completion of the desktop texture, atlas, or lossy rendering gates.

## Lossless storage

The standalone writer supports optional Zstandard transport and meshoptimizer
vertex/index-sequence encoding. Both retain raw storage where their individual
encoding would not help. The reader bounds codec workspace, semantic expansion,
record storage, and cancellation intervals. Fixed-version index-sequence coding
preserves point/line/triangle order and provoking vertices; vertex coding
preserves every canonical byte. Runtime builds link decoder targets only.

The standalone mesh suite and imported-fixture tests passed together with the
writer and scene I/O tests (6/6). They cover actual vertex/index directory
encodings, arbitrary index values, floating-point bit patterns, corruption,
block cancellation, expansion budgets, incompressible fallback, and earlier
metadata versions. Logs are in
`E:/Dev/ZenAssetProcessor/build/processor-port/m4-mesh-tests.log`.

All seven meshopt-plus-Zstandard source/package render pairs and comparisons
with the frozen M0 baseline are pixel-identical, with no validation errors.
Evidence is in `build/asset-processor/m4-mesh-visual-parity/results.json`;
`encoding-audit.json` confirms vertex encoding 1 and index encoding 2 in all
seven packages. Frozen binaries and configuration restoration checks are
recorded with the run. The independent installed Reader loaded fourteen copied
raw/Zstandard packages without their source files; see
`build/asset-processor/m4-zstd-installed-reader/results.json`.

A separate bounded ASan/libFuzzer Zstandard run completed **15,324 inputs in
121 seconds** with no sanitizer or invariant failures. It used compressed
scene and animation seeds and instrumented transport decoding; it predates
semantic mesh decoding. Evidence: standalone
`build/reader-fuzz/zstd-verification.json` and `zstd-run.log`. Finite fuzz runs
do not establish exhaustive malformed-input safety.

A subsequent mesh-aware run completed **14,808 inputs in 121 seconds** with
no sanitizer or invariant failures. It added mesh-compressed scene and FBX
skin/animation seeds, with both raw and Zstandard transport. Evidence:
`build/reader-fuzz/mesh-verification.json` and `mesh-run.log`. The initial
Windows launch selected a mismatched ASan DLL and failed before fuzzing;
prepending the matching compiler runtime fixed startup. The successful run is
separate from that startup failure and does not cover future texture cooking.

## Measured tradeoffs

The frozen producer CLI, source hashes, per-run results and process peaks are
recorded in `build/asset-processor/m4-zstd-cpu-memory/results.json`. A separate
Reader/Core benchmark measures `ReadScene` and `AdaptProcessedScene` without
process-startup time or GPU work; it uses one warm-up and seven measured runs
per raw/Zstandard pair. Evidence:
`build/asset-processor/m4-zstd-stage-measurements/results.json`.

| Asset | Raw bytes | Zstandard bytes | Median raw read | Median Zstandard read |
| --- | ---: | ---: | ---: | ---: |
| DamagedHelmet | 86,119,041 | 21,944,220 | 30.35 ms | 67.58 ms |
| EnvironmentTest | 12,742,435 | 1,860,309 | 6.00 ms | 11.48 ms |

DamagedHelmet adaptation medians were 10.12/9.98 ms; combined-process peak
working sets were 170.97/170.70 MiB. EnvironmentTest adaptation medians were
2.06/2.01 ms and peaks 30.70/30.75 MiB. These measurements show storage savings
with extra decode CPU work; they do not show decoded-memory or GPU-geometry
savings. See the JSON for every sample and the smaller scenes.

Mesh encoding is not always an additional package-size win. With the same
frozen current CLI, EnvironmentTest increased from 1,860,309 bytes using
Zstandard alone to 1,899,189 bytes using both codecs. The other six scenes saved
between 16 and 3,648 bytes. Semantic fallback is evaluated before transport
compression; the two combined streams are not exhaustively size-selected.
See `m4-mesh-visual-parity/zstd-vs-mesh-sizes.json`.

## Geometry processing and remaining gates

`CookScene` now separates source normalization from geometry processing and
package writing. It validates input, bounds a copied scene and processing
scratch, keeps immutable images shared, and returns no scene on failure.
Disabled processing shares the original owner. Metadata version 4 records
applied geometry settings; metadata versions 1 through 3 remain readable.

Optional vertex-fetch ordering preserves primitive/corner order and all
associated streams. Optional position quantization measures world-space error
for each eligible static instance against the maximum of the configured
absolute and relative bounds. Skin/morph/animated or unsupported overlapping
cases retain original positions with diagnostics. Normals, UVs, weights and
other attributes are not quantized by this pass.

The final focused Release run passed **6/6** suites: 34 geometry checks,
16 Cook API checks, 31 imported Cook checks, 59 writer/compatibility checks,
CLI tests, and format tests. Evidence:
`E:/Dev/ZenAssetProcessor/build/processor-port/m4-geometry-tests-final.log`.
The CLI requires the development desktop profile for geometry transformations;
the reference profile retains the original records.

The full fetch-remapping corpus gate passed **360/360** accepted sources with
no failures or warnings. It checked 24,673 primitives and 47,242,061 corners;
105 sources changed vertex order, covering 14,904,590 remapped corners. The
comparisons include 1,177,293 full-influence corners and 9,402,864 custom
components, authored morph/custom resolution, all UVs, and unchanged resource
and animation identities. The frozen executable, source hashes, each outcome,
and import/cook/verification timings are recorded in standalone
`build/processor-port/m4-fetch-corpus.json`. Quantization was disabled for this
lossless gate.

A later [native pose checkpoint](ZenAssetProcessorM4PoseVerification.md) passes
30 exact source-free package render pairs after an actual fetch remap, covering
skin, morph and combined animation at keys/midpoints in inline/threaded modes.
Thirty reversed-triangle controls verify sensitivity to stable transparency
ordering, and every clip visibly deforms. This complements the semantic corpus;
it does not establish lossy-quantization or complete M4 acceptance.

The [optional quantization report](ZenAssetProcessorM4QuantizationVerification.md)
records 134 primitive instances, 28,350 actually changed vertices and 1,200
excluded vertices. All numerical bounds and preserved-resource comparisons
passed. All seven source/raw reference captures are exact; the two models with
changed positions have nonzero rendered differences. The policy defines no
desktop frame-error threshold, so those measurements are descriptive and do not
establish broad visual acceptance. Quantization stays opt-in.

Remaining work includes wider lossy visual/pose coverage, the complete texture
pipeline, atlases, additional source formats, batch builds, release pinning,
and the outstanding detailed GPU allocation/upload measurements from M0.
