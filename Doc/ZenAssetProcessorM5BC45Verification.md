# M5 BC4/BC5 verification

Follow-up, 24 October: the [filtered-normal checkpoint](ZenAssetProcessorFilteredNormalVerification.md)
finds two previously accepted clearcoat maps exceeding the existing angular
limits after filtering. Both now retain original pixels, and the new BC4/BC5
clearcoat package renders exactly against raw in raster and both GI modes.
Deterministic reference repeats pass. Shared-odd highlight differences remain;
the historical evidence below is preserved, not full visual acceptance.

23 October 2026. The processor slice passes the independent CPU package audit:
15 inputs, 48 decoded mip measurements, source-free Reader loads and exact
read/write roundtrips. BC5 and BC4 remain explicit, disabled-by-default options.
Native upload/sampling tests also pass 3/3 with Vulkan validation enabled.
All 15 raster source/raw comparisons and seven frozen reference views are
exact. **Full rendered acceptance remains open:** repeated identical raw GI
inputs differ, and BC5 visibly shifts some sharp glossy highlights despite
meeting the per-image angular limits. Defaults remain disabled.

## CPU contract and evidence

[The audit](../../ZenAssetProcessor/tools/validation/asset_processor_bc45_audit.py) uses raw transport and raw
geometry. Each input produces default, explicit reference, mip-only desktop,
and BC5-normal/BC4-occlusion desktop packages. The independent decoder implements
[Microsoft's BC4/BC5 endpoint/index interpolation](https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-block-compression);
it does not call the processor's bcdec decoder. Handcrafted blocks exercise both
interpolation modes, all indices, channel order and cropped edge blocks.

BC5 decoded XY reconstructs positive Z and normalizes before comparison with
the normalized original RGB direction. Each mip must meet the unchanged
1-degree RMS and 5-degree maximum limits. BC4 compares only consumed linear R
against the unchanged 8/255 maximum scalar error. General packed data and color
uses retain their complete original pixels. Non-unit normal scales and authored
negative Z retain the original image with explicit diagnostics.

Every reference and default resource is identical. Every copied source
directory is temporarily renamed before CLI `validate` and the isolated
[ReadScene/WriteScene helper](../../ZenAssetProcessor/tools/validation/asset_processor_roundtrip.cpp) run. All
15 loaded packages reproduce every raw resource exactly, including metadata,
interpretation markers and authored pixel bytes. Source directories are then
restored. Original corpus files are never renamed or edited.

The audit checks unaffected resource payloads, material fields after resolving
texture-ID remaps, sampler/UV/scale semantics, format/interpretation agreement,
block footprints and duplicate base/mip-zero ownership. Required mip chains
are complete. ClearCoatTest's non-mipmapped sampler legitimately produces
one-level BC5 images; those compare with the base level of the explicit mip-only
control. An initial harness incorrectly demanded equal chain lengths; that
run stopped before acceptance and remains in `m5-bc45-cpu`. The corrected
sampler-aware run is the evidence below.

- [Complete results, commands, process memory and timings](../build/asset-processor/m5-bc45-cpu-final/results.json)
- [Copied input dependency identities](../build/asset-processor/m5-bc45-cpu-final/dependencies.json)
- [Focused processor tests](../../ZenAssetProcessor/build/codec-processor/bc45-texture-tests.log):
  TextureImageTests and TextureProcessingTests pass; classification reports
  150 checks. These include BC7 regressions, tiny/odd BC4/BC5 mips, quality
  rejection, ownership, disabled policy, channel discard confined to its role,
  aliases, variants, animation identity, scale exclusions, budgets and cancellation.

Frozen binaries in `build/asset-processor/m5-bc45-binaries`:

| Binary | SHA-256 |
| --- | --- |
| `zenassetc.exe` | `a4cc5cc83f1aa6c55d103a57113bac2d0da90499b77bb6565a1fac69ddddb0ec` |
| `asset_processor_roundtrip.exe` | `b59842b5e19bc93b08b37d7be3ec5f339ec494d957c5138bfd44bf06e485357f` |
| `VulkanRHIIntegrationTest.exe` | `b92ca523712f96e1f359311d2cf89af0d3f0f194a1830c2bf73258909444cdf1` |
| `scene_renderer_demo.exe` | `c47ceadf683bb5517c53d17ed11c61ed6a9575532005bca8720f6e22a6051fab` |

CLI and roundtrip identities remain unchanged throughout the audit. The parent also
completed the six schema/Cook/Writer/SceneIO/Format suites and CLI suite before
CLI freeze, including legacy texture-v1 compatibility and invalid marker
rejection. This report's independent evidence is the raw package audit above.

## Results and limitations

The corpus contains the seven earlier reference scenes, AlphaBlendModeTest,
NormalTangentTest, ClearCoatTest and CompareAmbientOcclusion, plus four shared
color/data/normal/clearcoat-normal/occlusion fixtures: flat, odd-sized,
negative-Z and non-unit scale.

Four unique image records convert to BC5 and five to BC4. All 48 image/mip
measurements pass. The largest observed BC5 RMS is **0.480942 degrees** and
maximum is **3.538337 degrees**; maximum BC4 R error is **0.0229692**. These
are per-mip maxima, not averages across the corpus.

The seven reference scenes produce no BC4/BC5 image conversions. In particular,
DamagedHelmet retains its normal image because it contains negative Z and its
occlusion image because R exceeds the error limit. NormalTangentTest retains
its normal image on angular error but converts occlusion. CompareAmbientOcclusion
retains occlusion images on R error. The shared negative-Z and scaled fixtures
retain normal pixels while their independent occlusion variants still convert.

Role splits can increase package and modeled GPU storage, even when an image
is retained. The current engine uploads per texture record; shared CPU pixels
do not imply a shared GPU allocation. The following GPU column sums complete
runtime mip footprints per texture record, with authored chains used as-is.
It is a calculated payload footprint, excluding alignment, staging and driver
allocation overhead.

| Input | Package bytes, reference → BC4/5 | Per-texture GPU mip bytes, reference → BC4/5 |
| --- | ---: | ---: |
| AlphaBlendModeTest | 54,594,828 → 54,595,628 | 72,701,264 → 117,440,500 |
| NormalTangentTest | 50,991,955 → 55,885,747 | 67,108,860 → 69,905,076 |
| ClearCoatTest | 10,420,004 → 7,798,580 | 9,437,176 → 3,757,392 |
| CompareAmbientOcclusion | 51,890,941 → 51,891,229 | 50,331,644 → 72,701,264 |
| Shared flat | 31,999 → 46,959 | 43,688 → 51,920 |
| Shared odd 5×7 | 15,743 → 16,495 | 336 → 480 |

The JSON also records unique image ownership, including duplicated base bytes,
and exact per-image mip payloads. The largest sampled cook working set is
140,988,416 bytes. Each process was sampled once with an uncontrolled warm OS
cache and two cook workers; these are diagnostics, not the prescribed repeated
performance benchmark. Native GPU allocation/upload and rendered comparison
remain separate gates. Existing BC7 evidence is preserved.

## Native checks

Tests extend the existing Vulkan block upload/readback fixture across
BC4, BC5 and BC7; odd 5×7, 2×3 and 1×1 mip regions prove 8-byte BC4 and 16-byte
BC5 block footprints. Unsupported formats must fail before image-view creation,
and incompatible views must be rejected. An existing compute sampling shader
checks native BC4 `(R,0,0,1)` and BC5 `(R,G,0,1)` values directly, independently
of material normal reconstruction. **All three tests pass on the RTX 5080**,
with Vulkan validation enabled and no validation errors or VMA leaks. Injected
unsupported-format failures are expected and occur before view creation.

[Run identity and timing](../build/asset-processor/m5-bc45-native-results.json)
and [full test log](../build/asset-processor/m5-bc45-native-tests.log) preserve
the evidence. The engine configuration was never modified; SHA-256 remains
`5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`.

The engine boundary owner separately completed CommonTest 38/38, reflection
and resource tests 30/30, EditorModel 43/43, RenderCore 703/703 and editor
rollback 2/2. Reflection verifies material stride 1264, normal interpretation
offset 1248 and feature-texture offset 432 in raster, voxelization and probe
tracing shaders. The rollback tests cover malformed BC4 and missing BC5
interpretation in inline and threaded loading. Logs are in
`build/asset-integration-release/bc45-*-tests*.log`.

## Rendered evidence

[Capture results](../build/asset-processor/m5-bc45-visual/results.json) contain
90 validation-clean processes: four profiles for 15 raster inputs, plus source,
raw, mip-only, BC4/5 and repeated raw captures for three scenes under both
compute and hardware GI. Resolution is 640×480, with three fixed-step raster
frames or sixteen GI frames, asset cache disabled. Existing GI algorithms and
settings were preserved. No desktop frame tolerance was invented or adjusted.

All **15/15 raster source/raw pairs** and **7/7 frozen M0 source views** are
pixel-exact. All source/package captures publish successfully and report zero
remaining VMA allocation. Reference inputs, renderer, shader sources/SPIR-V
and audited manifest retain their recorded identities throughout the run.

| Raster case with changed pixels | Maximum 8-bit channel error | RMSE, 8-bit | PSNR, dB | Pixels exceeding 8/255 |
| --- | ---: | ---: | ---: | ---: |
| NormalTangentTest, BC4 occlusion | 1 | 0.101085 | 68.0371 | 0% |
| ClearCoatTest, BC5 normals | 169 | 1.093303 | 47.3560 | 0.2796% |
| Shared flat | 1 | 0.001804 | 103.0050 | 0% |
| Shared odd 5×7 | 58 | 0.480558 | 54.4959 | 0.0977% |

All other raster reference/BC4/5 pairs are exact, including negative-Z and
scaled-normal fallback fixtures. The shared fixture camera is oblique after
automatic asset fitting, so those frames provide limited coverage of planar
material detail; the independent CPU role/channel audit and native sampling
checks provide the stronger evidence for these inputs.

Inspection of the [focused comparison](../build/asset-processor/m5-bc45-visual/focused-review.png)
and [ClearCoatTest maximum-delta crop](../build/asset-processor/m5-bc45-visual/clearcoat-maximum-delta-crop.png)
shows unchanged silhouettes/layout and localized glossy-detail changes in the
base/coat normal-map rows. The maximum occurs at pixel (404,364), blue channel,
inside the coat-normal example. These visible highlight changes prevent a
claim that passing normal-map angular limits alone preserves material
appearance. The crop's third panel amplifies differences eight times.

The GI exact gate fails independently of compressed textures:

| Identical raw-package repeat | Maximum channel difference | Different channels |
| --- | ---: | ---: |
| NormalTangentTest, compute | 3 | 1,839 |
| NormalTangentTest, hardware | 7 | 1,963 |
| ClearCoatTest, compute | 6 | 516 |
| ClearCoatTest, hardware | 2 | 669 |
| Shared flat, compute | 1 | 41 |
| Shared flat, hardware | 1 | 38 |

Consequently `automated_checks_passed` remains **false** in the visual report.
Lossy GI metrics are recorded descriptively, with this baseline nondeterminism
explicit. No GI code was modified or tolerance relaxed to obtain a pass.

[The memory summary](../build/asset-processor/m5-bc45-memory.json) combines the
per-texture payload calculations above with actual whole-process VMA logs.
AlphaBlendModeTest's peak committed allocation increases from 443,202,560 to
462,428,672 bytes, and peak device-local allocation from 308,984,832 to
328,210,944 bytes. Other selected cases retain the same allocator-pool peaks
despite changed payload sizes. These pool-level figures do not replace
per-resource allocation or GPU upload-time measurements.

The capture lock is removed, the engine configuration is restored byte-exact
to the SHA-256 above, and the GPU window is released. BC7 evidence and the
user's GI files remain untouched by this work.
