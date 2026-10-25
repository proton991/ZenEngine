# M6 spatial GI repeatability diagnosis

Follow-up: the [exact guide capture checkpoint](ZenAssetProcessorGuideCaptureVerification.md)
preserves learning and now passes these bounded comparisons through an opt-in
capture mode. The failed baseline and reverted ablation below remain historical
evidence; default rendering still uses the original FP32 accumulation policy.

24 October 2026. The full GI exact-image gate remains **failed**. The production
learning guide causes run-to-run variation in the tested fixtures, including
when loading the identical standalone package. A controlled ablation isolates
that path; disabling it is not an accepted implementation or an acceptance pass.
The production shader and generated SPIR-V were restored after the experiment.

## Experiment

The diagnostic uses the existing native editor fixture and two original inputs:
`spatial_scene.gltf` for material/normal/clearcoat sampling, and
`spatial_alpha_scene.gltf` for MASK/BLEND and specular/glossiness alpha. It cooks
standalone and spatial packages, deletes the copied inputs, and loads the
packages through the editor. Each capture starts with a fresh device and renders
16 frames at 256x192. A third capture repeats the exact standalone package.

Both software and hardware triangle providers run inline and threaded, using
the default pixel gather, temporal accumulation, filtering, adaptive sampling
and learned guide. The viewport uses a fixed camera, one directional light,
environment intensity 1, a 64-cell voxel grid and a 128-pixel shadow-map setting.
Every frame must report the requested provider, ray bounce, ray shadows and
reflections. Captures must contain more than 512 colored pixels. Switching the
same scene to PBR must change more than 100 channels, proving a visible GI effect.
The only assertions that failed were exact frame comparisons.

The three experiment stages use the same fixture, camera, settings and binary:

1. Original production guide: full four-case matrix, 24 captures.
2. Temporarily empty `GuideLearn` body: the same matrix, 24 captures.
3. Restore the original shader byte-for-byte: both inline providers, 12 captures.

| Stage | Atlas/reference changed RGB channels per pair | Identical-package repeat changed channels | Maximum byte difference |
| --- | ---: | ---: | ---: |
| Original guide | 7-19 | 7-15 | 1 |
| Guide learning disabled | 0 | 0 | 0 |
| Original guide restored, inline | 8-16 | 11-16 | 1 |

There are eight atlas/reference and eight identical-package comparisons in each
full matrix. The restoration stage has four of each. Baseline tests fail 4/4;
the ablation passes 4/4; restored tests fail 2/2. All process runs report zero VMA
leaks and no tracked CPU leaks. No Vulkan validation error was reported.

## Interpretation and next requirement

[`GuideLearn`](../Data/Shaders/VoxelGI/hybrid_trace.glsl) accumulates FP32 values
in shared bins through an `atomicCompSwap` loop. The updates are atomic, but
invocation order is not fixed. Different addition orders can round to different
FP32 sums. `FinishGuide` averages those sums into the next frame's sampling
distribution. The ablation and reversal locate the observed repeatability gap
in that learning path for these fixtures; they do not prove that every GI scene
has the same sole source of variation.

The unchanged exact gate cannot distinguish atlas error from those small
run-to-run differences. The next correction must preserve guide learning while
providing deterministic capture behavior, then repeat both standalone controls
and atlas comparisons. Disabling learning, accepting one-byte differences, or
using the ablation's green result as production acceptance would bypass the
requirement. Any change to accumulation needs numerical, HDR-range, sampling
quality and cost validation in the GI work before the broader asset gate can
close. No production GI fix is included in this checkpoint.

## Retained evidence and cleanup

Under `build/asset-integration-release`, the prefixes
`m6-spatial-gi-probe`, `m6-spatial-gi-guide-off` and `m6-spatial-gi-restored` each
have build logs, execution logs/XML, `-results.json`, `-captures/` and
`-evidence/identities.json`. These retain the tested executable, temporary fixture,
shader sources, generated SPIR-V, packages and captures. Their manifests contain
539, 539 and 519 hash-verified snapshots respectively. The capture analyzer
parses PPM bytes directly and records each comparison without changing thresholds.

The temporary fixture is retained in the ignored `build/spatial_gi_probe.inl`
and evidence snapshots. Its include was removed from
`../ZenAssetProcessor/tests/integration/zenengine/Editor/EditorAssetRenderingTests.cpp` after the diagnostic. It is not
registered in the normal suite. Shader restoration checks the experiment's
current bytes before writing the saved original, preserving unrelated GI work.
Original/restored `hybrid_trace.glsl` SHA-256:
`100160c569a78842643269766cef8d6ce5856a946e083ca84aa452ec9af07793`.

Cleanup verified all 486 saved shader source/SPIR-V hashes and restored the test
wrapper exactly to the ray checkpoint. The rebuilt normal target then passed
all ten cooked array/spatial tests in 6.831 seconds, with no skipped cases,
validation errors or reported leaks. This is recorded in
`m6-spatial-gi-restoration.json` and `m6-spatial-ray-after-gi.{log,xml}`. The two
original GI fixture captures were also viewed to confirm visible surfaces and
alpha holes; that inspection does not waive their failed exact comparisons.

The independently passing [alpha-query checkpoint](ZenAssetProcessorM6SpatialRayVerification.md)
remains valid. It establishes exact traversal/material decisions for its bounded
profile; it does not replace the full GI gate diagnosed here.
