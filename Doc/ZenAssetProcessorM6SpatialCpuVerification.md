# M6 spatial regions in engine CPU scenes and previews

The later [GPU checkpoint](ZenAssetProcessorM6SpatialGpuVerification.md) supersedes
the temporary GPU rejection described below. This document retains the CPU
checkpoint and its original evidence.

Verified on 24 October 2026 with the Windows MSVC Release build at
`build/asset-integration-release` and the developer override at
`E:/Dev/ZenAssetProcessor`.

## Implemented behavior

ZenEngine now imports Texture v4 spatial regions into its own `ImageRegion`
record and carries them from `TextureAsset` into each scene-graph `Texture`.
The canonical adapter copies page pixels into an engine owner, then shares that
owner among texture views and runtime instances. Each scene component retains
its own region. Authored material transforms, texture IDs, linear-view aliases
and sampling settings remain independent.

Engine scene validation mirrors the current standalone spatial contract:
single-level RGBA8, one physical layer, power-of-two page/tile extents, contained
four-texel-aligned interiors with gutters, and explicit linear clamp sampling.
It checks bounds before subtracting dimensions. Malformed input publishes no
adapted scene. Legacy v2 writing rejects spatial regions before replacing the
destination; older v2 reads initialize the new field to its full-image sentinel.

Scene-graph width/height and mip byte spans describe physical storage.
`GetAtlasRegion()` supplies the logical view only while immutable shared pixels
are active. Local pixel overrides select their full image, and `Init` clears
stale region metadata. Texture equality includes the effective region.

Editor previews use the Reader's bounded crop operation to sample only the
selected tile. They preserve stored UNORM/sRGB bytes and the existing opaque
preview alpha policy. The asset browser displays logical tile dimensions; its
byte count describes the shared physical storage. Whole-image RGBA and existing
BC/layer preview paths retain their behavior.

## GPU boundary

Spatial packages can load into CPU scenes. GPU rendering remains unfinished.
The renderer's resource preflight reports a named spatial-view error. The texture
loader independently rejects these views before creating a native image or
queuing a copy. Mutable standalone overrides still upload normally.

This replaces the earlier adapter-level rejection with a GPU boundary while
preserving regions for CPU consumers and previews. It does not establish native
page sharing, shader mapping, derivative preservation, alpha parity or workload
benefit. Those remain required M6 integration work.

## Verification

| Check | Result |
| --- | --- |
| Adapter, package I/O and cache tests | 42/42, 5.673 seconds |
| Complete editor-model executable | 47/47, 0.147 seconds |
| Complete RenderCore mock executable | 714/714, 7.396 seconds |
| Source-absent CLI package load smoke checks | 3/3 |

All three test executables reported zero tracked CPU leaks. The RenderCore
executable also contains nine previously disabled tests, which did not run;
714 is the executed count. Its result is mock-backed verification, not native
GPU rendering evidence.

New tests cover owner lifetime after the standalone scene is destroyed,
independent runtime view state, raw/Zstandard package loading and Metadata v8,
invalid rectangles, unsupported mip/sampler/array policies and legacy destination
preservation. Preview tests compare every output byte against standalone tile
references at five bounds in UNORM and sRGB views, and cover invalid storage,
mutable overrides, initialization, equality and asset-browser dimensions.

The GPU boundary test exercises both production preflight and direct texture
loading, checking zero native image/buffer creations and no queued image copies
for the spatial view. Its standalone override remains accepted. Existing array
upload, lifetime and material-resource tests also pass in the full suite.

The application smoke checks use the previous cooker checkpoint's frozen
`spatial-none.zenasset`, `spatial-zstd.zenasset` and `off-zstd.zenasset`, with the
recorded source path absent. The current runtime loads one node, one mesh, nine
vertices and three textures from each package. Region preservation through the
normal package reader and runtime builder is asserted separately by the new
adapter tests.

An initial test compile attempted to copy a noncopyable component; the fixture
now assigns the relevant independent view fields. The first common-test run
also rejected the new fixture because one alias lacked the contract's required
explicit sampler. The fixture now supplies it. Both failures remain in the
logs, and validation requirements were unchanged.

Logs and XML results use `build/asset-integration-release/m6-spatial-cpu-*`.
Application logs and package identities are in `m6-spatial-cpu-runtime`.
Selected source, baseline diff, binaries, logs and packages are frozen under
`m6-spatial-cpu-evidence`; this is scoped evidence, not a release distribution.

## Next integration

Connect the retained logical regions to GPU page/view ownership and material
sampling, including raster, shadow and ray-alpha consumers. Preserve authored
UV derivatives and validate sampler behavior against standalone references.
The standalone single-level sampling probe is a starting point, not a portable
hardware-rounding guarantee or full material-scene acceptance. Broader
mip/filter/dimension/compression coverage and measured workload benefit remain
open in the implementation plan.
