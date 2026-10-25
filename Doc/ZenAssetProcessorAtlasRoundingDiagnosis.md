# Spatial atlas rounding across NVIDIA and AMD

24 October 2026. The AMD spatial mismatch is traced to the coordinate tie rule
for the tested power-of-two profile. **Round-to-even restores exact AMD
sampling; half-up preserves exact NVIDIA sampling.** Replacing the production
rule globally would introduce an NVIDIA regression. Production sampling remains
unchanged, and M6 remains open pending device-specific qualification/selection.

## Controlled engine experiment

The [previous native campaign](ZenAssetProcessorNativeNormalVerification.md)
found 142 changed alpha decisions on AMD in each execution mode. This experiment
copies the production material shader and changes exactly one expression:
`floor(scaled * divisions + 0.5)` becomes `roundEven(scaled * divisions)`.
Standalone reference coordinates, source pixels, gutters, material transforms,
samplers and test assertions remain unchanged.

An isolated executable recompiles the existing array/spatial test translation
unit with a separate shader directory and links the same engine libraries.
It leaves the normal engine executable and shader assets untouched.

| Device / coordinate rule | Differing float channels per execution mode | Changed alpha decisions per execution mode |
| --- | ---: | ---: |
| NVIDIA / production half-up | 0 | 0 |
| AMD / production half-up | 127,033 | 142 |
| AMD / diagnostic round-to-even | **0** | **0** |
| NVIDIA / diagnostic round-to-even | 122,360 | 830 |

Both inline and threaded modes produce those results. The diagnostic AMD run
passes both spatial tests; the NVIDIA run fails both. No assertion or numerical
tolerance is relaxed. There are no Vulkan validation errors or tracked CPU/VMA
leaks in these runs. Existing loader/RDG warnings remain recorded.

## Expanded standalone experiment

The standalone Vulkan evaluator uses actual packer-produced pages, fourteen
image dimensions, three patterns, UNORM/sRGB views, six UV grids and explicit
LOD/oblique-gradient sampling: 1,008 comparisons per run. The six grids include
exact subtexel ties and the adjacent representable coordinates. Anisotropy is
disabled, as required by the current spatial profile.

| Device / candidate | Power-of-two failures / 720 | Other-dimension failures / 288 | Alpha decisions changed in power-of-two cases |
| --- | ---: | ---: | ---: |
| NVIDIA / half-up | **0** | 84 | 0 |
| AMD / half-up | 76 | 36 | 6,088 |
| AMD / round-to-even | **0** | 44 | 0 |
| AMD / truncation | 216 | 112 | 9,524 |

The two matching power-of-two subsets are exact in floating channels, not just
within the evaluator's byte threshold. Each subset contains 11,981,520 paired
samples, including repeated queries. Every full matrix still exits with a
failure because unsupported non-power-of-two cases remain in the experiment.
All four runs complete with zero validation errors; no missing rows are treated
as passing evidence. AMD half-up's supported-profile failures mostly occur on
the exact-tie grid, with four additional failures on the hashed UV grid.

The final evaluator rebuild repeats the NVIDIA half-up and AMD round-to-even
matrices and compares every JSON record with the diagnostic run. The unmatched
device-name check exits with an execution failure before producing samples.
This verifies that a requested AMD run cannot silently fall back to NVIDIA.

## Tool change and reproduction

Revision 10 of
`ZenAssetProcessor/tools/evaluation/atlas_sampling` accepts the optional
`ZEN_VULKAN_DEVICE` environment variable as a case-sensitive GPU-name substring.
An absent or empty value retains the previous discrete-device selection;
an unmatched nonempty request fails. Layout 14 adds an explicit truncation
candidate; existing layout 9 is round-to-even and layout 12 is half-up.
These are evaluation-only changes. Processor/Reader targets gain no Vulkan
dependency, and the processor source fingerprint remains
`6d13d0abb5d7624e8be26a763af7838e7021b38014db71ea6fa04e570c33ee2e`.

From the standalone checkout in the MSVC development shell:

```powershell
cmake -S tools/evaluation/atlas_sampling -B build/m6-cross-vendor -G Ninja -DCMAKE_BUILD_TYPE=Release -DZENASSET_PROBE_FLOAT64=OFF
cmake --build build/m6-cross-vendor --parallel 2
$env:ZEN_VULKAN_DEVICE = 'AMD Radeon(TM) Graphics'
build/m6-cross-vendor/atlas_sampling.exe build/m6-cross-vendor/atlas_sampling.vert.spv build/m6-cross-vendor/atlas_sampling.frag.spv 1 0 9 0
```

Use device `NVIDIA GeForce RTX 5080` with layout 12 for the matching NVIDIA
candidate. Both full runs are expected to exit 1 because their non-power-of-two
comparisons remain included. MSVC emits its existing API-deprecation warning
for reading the environment with `getenv`; compilation otherwise succeeds.

The engine probe source/commands, isolated binary and shaders, complete native
logs, expanded matrix outputs and final rebuild results are under
`build/amd-atlas-probe`. The
[frozen identities](../build/asset-integration-release/m6-rounding-evidence/identities.json)
preserve this checkpoint, including its intentionally failed comparisons.

## Production implication

Both tested devices report eight fractional texel bits, but that value alone
does not select the matching tie rule. The
[Vulkan sampling specification](https://raw.githubusercontent.com/KhronosGroup/Vulkan-Docs/main/chapters/textures.adoc)
describes coordinate conversion and retained precision; these measurements do
not establish a rule for every device from either vendor.

The next production change must qualify the device's sampler and carry the
result into the shared material sampling path, including explicit ray-alpha
sampling and live material updates. A small native calibration should compare
known tie/neighbor samples with supported candidates, report an unsupported
result when no rule matches, and preserve publication failure behavior. It
must not silently assume a vendor-wide rule from these two devices.

`MaterialData::textureSampling` currently has unused components, while scene
initialization publishes materials before uploading textures; qualification
therefore needs a defined point before material publication. Runtime cost,
failure/cleanup paths and full cooked raster/GI comparisons still need
verification. This diagnosis does not close that work or broaden the supported
spatial profile to non-power-of-two images, mip chains or anisotropy.
