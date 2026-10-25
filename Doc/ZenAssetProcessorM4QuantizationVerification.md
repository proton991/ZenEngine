# Optional M4 position quantization verification

**Date:** 23 October 2026. The seven-case numerical and preservation checks
passed at 16 position bits. All source and reference-package GPU captures
matched the frozen M0 references exactly. The two models with changed positions
produced nonzero image differences after quantization, recorded below.

The M0 zero-channel-error requirement belongs to `reference_profile`.
`desktop_profile.quantization` declares position error and preserved-semantics
bounds, but no separate rendered-image tolerance. The visual probe also applied
the reference's exact threshold conservatively to quantized images; its
`quantized_gate_passed: false` and overall `passed: false` describe that exact
comparison, not a violation of the declared numerical position bound or evidence
of a renderer defect. No threshold was loosened and settings were not tuned to
the images. Position quantization remains opt-in. This is not M5 verification.

## Settings and independent numerical audit

The [audit tool](../../ZenAssetProcessor/tools/validation/asset_processor_quantization.py) cooked reference and
desktop packages with the same frozen CLI and two workers. Desktop enabled only
`--position-bits 16`, with vertex-fetch ordering off and both transport and mesh
encoding raw. Textures retained the reference representation. Error settings
came from [the versioned quality policy](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json):
maximum Euclidean world error per primitive instance is
`max(1e-6 meters, 1e-5 * world AABB diagonal)`. The report records the actual
float32 settings accepted by the API.

After successful package validation, a separate Python reader decoded the
explicit little-endian resource fields, without calling `CookScene`. It checked
every vertex's position delta and every primitive instance's ancestor-composed
`T * R * S * baseMatrix` transform. All other vertex bytes and resource payloads
were compared exactly, including indices, topology, mappings, complete
influences, custom attributes, morphs, animation, materials and pixels. Mesh
bounds also remained byte-identical in these seven cases.

**134 primitive instances passed.** There were **28,350 actually changed
vertices**, so this was not a test of flags alone or universal fallback.
All **1,200 excluded vertices** retained their original positions; their skin,
morph and animation inputs were byte-identical too.

| Case | Vertices changed | Excluded vertices | Instances | Maximum world error (m) | Largest fraction of permitted error |
| --- | ---: | ---: | ---: | ---: | ---: |
| DamagedHelmet | 14,556 | 0 | 1 | 0.0000244883730 | 0.744502 |
| SimpleSkin | 0 | 24 | 1 | 0 | 0 |
| AnimatedMorphCube | 0 | 24 | 1 | 0 | 0 |
| SimpleInstancing | 0 | 0 | 126 | 0 | 0 |
| EnvironmentTest IBL | 13,794 | 0 | 3 | 0.000162742623 | 0.761455 |
| FBX sausage | 0 | 1,152 | 1 | 0 | 0 |
| FBX embedded textures | 0 | 0 | 1 | 0 | 0 |

SimpleSkin and the FBX sausage were excluded for skin data; AnimatedMorphCube
was excluded for morph data and an animated instance/ancestor. The two remaining
static cube cases were eligible but their positions did not change. These
cases do not constitute an exhaustive pose or quantization corpus.

Evidence: [CPU report](../build/asset-processor/m4-quantization-cpu/results.json),
including every per-instance error and threshold, package hashes, source and
glTF dependency hashes, commands, diagnostics and process logs.

## Fixed GPU captures

The [visual tool](../../ZenAssetProcessor/tools/validation/asset_processor_quantization_visual.py) rendered
direct source, reference package and audited desktop package for each of the
seven established M0 views: 640×480, three frames, fixed step, ray tracing and
UI disabled, asset caching disabled, synchronization validation enabled, NVIDIA
GeForce RTX 5080. It used the existing frozen camera/environment/exposure
configuration and animation time. No render configuration was adjusted for
quantization.

All seven source/frozen-M0 comparisons and all seven source/reference-package
comparisons were pixel-identical. All 21 processes exited successfully with no
validation errors. Quantized versus reference results were:

| Case | Different RGB channels | Maximum channel error (8-bit) | RGB RMSE (8-bit) | Exact comparison |
| --- | ---: | ---: | ---: | --- |
| DamagedHelmet | 9,216 | 125 | 0.352426793 | Different |
| SimpleSkin | 0 | 0 | 0 | Identical |
| AnimatedMorphCube | 0 | 0 | 0 | Identical |
| SimpleInstancing | 0 | 0 | 0 | Identical |
| EnvironmentTest IBL | 10,513 | 227 | 0.801138513 | Different |
| FBX sausage | 0 | 0 | 0 | Identical |
| FBX embedded textures | 0 | 0 | 0 | Identical |

These maximum and RMS image observations are reported separately from the
numerical position gate. They do not establish a new permitted desktop frame
error or general image-quality acceptance criterion.

Evidence: [GPU report and captures](../build/asset-processor/m4-quantization-visual/results.json).
`Data/engine.cfg` was restored byte-exact, its exclusive smoke lock was removed,
and no external configuration edit was overwritten. Original/restored config
SHA-256: `5df1f9d218a6bdf85ef9cda9d4153c17b75f578571b1f802bccacbb44dd440d2`.

## Reproduction and binary identity

The earlier geometry executable had been replaced by the completed metadata-v5
CLI before freezing. Its package was not downgraded to fit an old reader. A
compatible renderer was rebuilt and frozen; public `SceneImportRequest` now
exposes format settings, so `ZenCore` correctly propagates `ZenAsset::Format`
publicly while retaining its private Reader dependency. The renderer build
verified that downstream UI headers consume that public contract.

- CLI SHA-256: `d2f33d0538ffa998863f3d358c55901ed8ae74775906b978ee6f046e0d691ea9`.
- Renderer SHA-256: `2fedc162dd683c74c328192f94bf23ef10cda4084674cd6d3ca5fd32e7f483b0`.
- Renderer build: `build/asset-integration-release/m4-quant-renderer-build-2.log`.

```powershell
python ../ZenAssetProcessor/tools/validation/asset_processor_quantization.py `
  --processor build/asset-processor/m4-quantization-binaries/zenassetc.exe `
  --reference-dir build/asset-processor/fresh-verification `
  --output build/asset-processor/m4-quantization-cpu --position-bits 16

python ../ZenAssetProcessor/tools/validation/asset_processor_quantization_visual.py `
  --renderer build/asset-processor/m4-quantization-compatible-binaries/scene_renderer_demo.exe `
  --cpu-report build/asset-processor/m4-quantization-cpu/results.json `
  --reference-dir build/asset-processor/fresh-verification `
  --output build/asset-processor/m4-quantization-visual
```

Both tools require a new output directory, preserving prior evidence. The
visual command exits 1 when its conservative exact comparison differs, as it
did in this run; inspect the separately recorded numerical and reference gates.
