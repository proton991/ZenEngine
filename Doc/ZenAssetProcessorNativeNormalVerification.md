# Native normal sampling on two Vulkan devices

24 October 2026. The new native tests pass the unchanged **1-degree RMS /
5-degree maximum** normal limits on the NVIDIA GeForce RTX 5080 and AMD
Radeon(TM) Graphics. The broader run also exposes a spatial-atlas failure on
AMD. **M5, M6 and the overall implementation plan remain incomplete.**

## What the tests exercise

`NormalSceneTextureIntegrationTests.inl` shares the existing native device,
upload and cleanup fixture. Its compute shader includes production
`material.glsl`, samples both original and cooked images with the same sampler,
then calls `MaterialNormalVector` and normalizes the result. The CPU measures
angles between the GPU-returned directions. Assertions check the actual native
image/view formats and complete mip counts, so compressed fixtures cannot pass
through a silent RGBA replacement.

Ten frozen compressed fixtures come from the installed processor with its
default quality limits and quality-4 BC7 encoding: BC5 and BC7 versions of
7x5, 5x7, 1x8, 8x1 and 16x8 authored chains. Their smooth, correlated XY
pattern crosses block boundaries and has positive Z. These are bounded test
fixtures, not a representative material corpus. An initial uncorrelated XY
pattern retained raw pixels in three BC7 cases; the exporter was adjusted to
provide accepted compressed controls without relaxing quality settings.

Two additional controls use the authored chain from the
[adjacent-mip regression](ZenAssetProcessorNormalMipVerification.md): the
current processor's retained RGBA chain and the previous processor's accepted
BC5 bytes. Exported source pixels are identical. The ten accepted compressed
fixtures also have identical old/current payloads. The native target links the
Reader and no Processor library; fixture production is a separate installed
consumer. This checks the upload/sampling boundary, not package loading or
source processing during the native run.

Each explicit-LOD group samples a non-dyadic 17x17 UV grid spanning both borders.
It covers all nearest/linear texel and mip-filter combinations, four independent
repeat/clamp combinations, quarter-level LOD increments, and clamping beyond
the mip tail. Each gradient group uses major axes of 2, 4, 8 and 16 texels,
minor axes one sixteenth as long, and rotations of 0, 45 and 90 degrees.
Anisotropy is both disabled and enabled at 16, with the four wrap combinations.
RMS is bounded separately for each fixture/sampler/LOD or footprint group.

The anisotropy control verifies an actual change in source sampling: 152,268
reference samples change on NVIDIA and 152,270 on AMD in each execution mode.
The tests run in inline and threaded RHI modes; their measured metrics agree
within each device. Across both devices and modes, **5,160,384 normal sample
pairs** execute. This includes repeated queries and controls, not that many
independent UV/LOD locations.

## Results

| Device | Explicit LOD: worst group RMS / maximum | Gradient sweep: worst group RMS / maximum | Legacy rejected-chain maximum |
| --- | ---: | ---: | ---: |
| NVIDIA RTX 5080 | 0.473544 / 0.924207 degrees | 0.400527 / 0.903043 degrees | 10.475082 degrees |
| AMD Radeon(TM) Graphics | 0.482714 / 0.923352 degrees | 0.400505 / 0.903043 degrees | 10.475084 degrees |

The retained chain's maximum is below 0.00001 degrees on both devices. Both
report eight subtexel and eight mipmap precision bits. Raw Vulkan driver-version
values are 2588573696 for NVIDIA and 8388939 for AMD.

| Native suite | NVIDIA | AMD |
| --- | ---: | ---: |
| New normal sampling, including negative control | 6/6 pass | 6/6 pass |
| Existing array-view and anisotropy comparisons | 4/4 pass | 4/4 pass |
| Existing spatial material/alpha comparisons | 2/2 pass | **0/2 pass** |

All 24 tests execute without skips. There are no Vulkan VUID/synchronization
errors or tracked CPU/VMA leaks. Logs retain loader notices, an unavailable
optional instance extension notice, and existing RDG unknown-initialization
coverage warnings; this is not a warning-free run. The initial runner stops
on AMD's nonzero test exit. The separate audit retains that failed outcome and
verifies the normal results without declaring the full suite successful.

## Confirmed spatial-atlas issue

On AMD, each execution mode reports **127,033 differing float channels** and
**142 changed `alpha >= 0.5` decisions**, with a maximum float difference of
0.003906. NVIDIA remains exact. The production material shader, spatial test
shader and spatial test body match preserved earlier checkpoints; the new
normal tests do not change their sampling logic.

The spatial path currently applies half-up rounding in logical texel space
before adding the page offset. Both devices reporting eight precision bits
does not establish that this rule reproduces both native samplers. The
[Vulkan sampling specification](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/chapters/textures.adoc)
defines the retained fractional precision; the earlier NVIDIA measurements
were explicitly a device observation. The exact cause of AMD's mismatch still
needs a focused rounding/coordinate probe. No production sampling rule or
acceptance tolerance was changed in this checkpoint. AMD spatial alpha parity
is now a concrete unresolved M6 requirement, not merely untested coverage.

## Reproduction and evidence

Build `VulkanRHIIntegrationTest` in `build/asset-integration-release`. Set
`VK_LAYER_VALIDATE_SYNC=1`, `VK_LOADER_LAYERS_DISABLE=~implicit~` and
`DISABLE_RTSS_LAYER=1`. Set `ZEN_VULKAN_DEVICE` to the exact device name above,
then run:

```text
--gtest_filter=*NormalSceneTextureIntegrationTest*:*ArraySceneTextureIntegrationTest*
```

The campaign used a byte-identical `7zFM.exe` alias to avoid an overlay hook.
Fixture producer source, installed consumers, output JSONL, fixture generator,
build/link logs, both complete native logs/results and the strict audit are
under `build/native-normal-probe`. The producer uses private processing entry
points only for diagnostic fixture export, with headers matching each installed
library. It changes no public API or processing behavior.

Current processor fingerprint:
`6d13d0abb5d7624e8be26a763af7838e7021b38014db71ea6fa04e570c33ee2e`.
Legacy control fingerprint:
`c1401d534570a5b98a717aa703cfa8a7259fadeac6587a5fc610837a32c6eba8`.
The [frozen identities](../build/asset-integration-release/m5-native-normal-evidence/identities.json)
preserve the inputs and outputs. The
[audit](../build/native-normal-probe/audit.json) explicitly records
`all_native_tests_passed: false` and `full_plan_complete: false`.

These finite device samples do not establish continuous angular bounds,
full-frame highlight/BRDF acceptance, all material patterns or all devices.
Broader M5 quality and throughput, the confirmed AMD spatial failure, atlas
workload benefit and release distribution still require work.
