# Spatial sampler qualification checkpoint

The engine now measures the device's spatial sampling rule before publishing a
scene containing spatial regions. NVIDIA RTX 5080 selects half-up; AMD Radeon(TM)
Graphics selects ties-to-even. Production material/alpha sampling is exact on
both devices in inline and threaded execution. No tolerance changed.

This closes the specific mismatch in the
[rounding diagnosis](ZenAssetProcessorAtlasRoundingDiagnosis.md). M6 remains open:
AMD cooked rendering encountered a separate driver access violation while
creating the ordinary G-buffer pipeline. Broader atlas filtering, workload
benefit, quality and release gates also remain open.

## Runtime behavior

`RenderScene::Init` qualifies spatial regions before loading materials or scene
textures. Failure returns initialization failure; the existing editor candidate
path preserves the active scene. Ordinary textures and arrays do not initiate
qualification. Initial materials and live updates receive device-owned
`textureSampling.xy`: subtexel divisions and rule (1 half-up, 2 ties-to-even,
0 unqualified). Raster, feature textures and ray alpha use the shared helper.
The material layout and package schema are unchanged. Processor/Reader targets
gain no GPU dependency.

The calibration shader uses independent red/green ramps in a 2x2 RGBA8 texture.
It evaluates 3,072 queries, including repeated coordinates, at ties and adjacent
representable UVs. Each query samples its original coordinate and both snapped
candidates through the native sampler. Comparing native samples with analytic
weights was rejected during development: NVIDIA UNORM interpolation differed
slightly even when coordinate rounding matched. Native candidate sampling
preserves exact comparison without an epsilon or assumed channel conversion.

Both axes must consistently match exactly one candidate. Empty, malformed,
nonfinite, ambiguous or unmatched observations remain unqualified. Reported
subtexel precision must satisfy the existing 1–23-bit precondition; that does
not promise every such device will qualify. Selection uses measurements, not
vendor IDs. Success is cached for the device and reset at destruction. Failed
shader creation is not cached; later allocation/pipeline failure publishes no
rule and allows a subsequent attempt.

Readback waits for the submitted graphics timeline serial, not device-wide
idle. Temporary requested payloads are a 16-byte texture and two 147,456-byte
buffers, plus staging; these are not native allocation measurements. Resources
use normal deferred retirement. A second successful qualification call causes
no new graphics or transfer submission.

## Executed checks

Windows Release, explicit GPU selection, Vulkan validation with synchronization
validation requested, and disabled implicit layers were used. Both GPUs report
eight subtexel bits. Native executables use a copied `7zFM.exe` alias to avoid
the existing RTSS hook.

| Check | Result |
| --- | --- |
| Classifier, setup/retry failures, existing spatial upload checks | 8/8 CPU tests |
| Production shader reflection/layout | 23/23 |
| Texture/material/normal sampling | 12/12 per GPU |
| Injected native pipeline failure and successful retry | 2/2 per GPU |
| NVIDIA cooked array/spatial raster, GI and ray comparisons | 14/14 |
| NVIDIA failed spatial publication and later successful load | 2/2 |

Four spatial material tests compare 798,768 sample pairs, including repeated
queries, with zero float-channel or alpha-decision differences. Existing array,
compressed-normal, mip-filter and anisotropic controls pass. Normal tests
execute 5,160,384 pairs across the devices/modes; see the earlier
[native normal report](ZenAssetProcessorNativeNormalVerification.md) for scope.

The final native run measured qualification CPU wall times of 20.070/15.301 ms
on NVIDIA and 13.954/5.946 ms on AMD (inline/threaded). These four observations
include shader/pipeline setup, upload, submission and readback; they are not a
representative startup or frame-performance benchmark.

NVIDIA cooked checks include 64 exact array frame pairs, 24 exact spatial frame
pairs, 393,216 exact ray pairs, eight spatial/reference GI comparisons and eight
identical-package GI repeats. They preserve source-absent loading, variants,
minification/grazing views, software/hardware alpha traversal and exact guide
capture. Live material updates cannot replace precision or rounding.

The publication-failure test injects compute-pipeline creation failure while
loading a cooked spatial package. Active scene, generation and selection remain
unchanged; the old scene still renders, and removing the injection permits the
spatial package to load and render. Native failure tests also verify retry after
queued-upload cleanup. Expected injected errors are audited separately.
Successful runs have no Vulkan validation errors or tracked CPU/VMA leaks.

## AMD rendered-scene failure

The AMD cooked campaign hit access violation `0xc0000005` on the first standalone
ordinary-texture render, before spatial qualification. GoogleTest caught the
exception; the following test started but did not complete. That process was
stopped because its post-exception state was unsuitable for comparisons. No
final AMD GoogleTest JSON exists; the campaign is incomplete, not passing.

A Windows debug-event harness reproduces the exception. A separate diagnostic
binary logs pipeline creation and shader overrides without changing production
code or production shader assets. The last pipeline is `GBufferSP`, entering
`vkCreateGraphicsPipelines`; the fault is in `amdvlk64.dll` at module offset
`0x24ea5a4` on this installed driver. Nearest exported stack symbols are not
treated as accurate internal function names.

A control compiles archived pre-qualification `material.glsl` into G-buffer and
forward fragment shaders. Its log confirms loading both overrides; the same
fault reproduces. The new rounding selection is therefore not necessary to
trigger this failure. This does not establish the complete cause or exonerate
all pipeline inputs. Exploratory inlining and single-LOD controls also failed;
none was adopted in production.

AMD cooked raster/GI/ray parity and publication-failure rendering remain
unverified. The next investigation must isolate G-buffer pipeline inputs and
shader compilation, then rerun the complete AMD campaign. No driver update,
threshold relaxation or skipped test substitutes for that gate.

## Evidence and remaining scope

Commands, logs, results, captures and diagnostic sources/binaries are under
`build/spatial-qualification`. Frozen
[identities](../build/asset-integration-release/m6-qualification-evidence/identities.json)
and the [audit](../build/asset-integration-release/m6-qualification-evidence/final-audit.json)
preserve tested artifacts, including failures. The successful 14-case cooked
campaign and later publication tests use separately preserved executables;
intervening changes only added/refactored test code.

The processor fingerprint remains
`6d13d0abb5d7624e8be26a763af7838e7021b38014db71ea6fa04e570c33ee2e`.
`Data/engine.cfg` remains byte-identical to the preceding checkpoint. Spatial
eligibility still requires power-of-two single-level RGBA8, linear clamp and
disabled anisotropy. General spatial mips, NPOT and anisotropy are unqualified.
M4/M5 quality, broader M6 benefit, optional formats and M8 release requirements
remain open in the implementation plan.
