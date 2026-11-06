# M6 native array upload and view checkpoint

24 October 2026. Native array pages now reach ZenEngine material bindings through
individual 2D views. This checkpoint verifies upload, view selection, ownership
and sampling. A subsequent cooked-scene checkpoint below verifies one mixed
material fixture. Broader M6 visual and workload acceptance remains open.

## Runtime contract

`TextureManager::LoadSceneTextures` returns borrowed `RHITextureView` pointers.
Within one call, immutable array owners share one native image and one upload
covering all layers of every explicit mip. Repeated layer/format bindings reuse
the view. RGBA8 and BC7 pages allow compatible linear/sRGB views; BC4/BC5 preserve
their typed interpretation. Bounds, byte footprints and format compatibility
are validated before upload. Mutable local overrides use standalone storage.
Different scene loads have independent native images.

`RenderScene::GetSceneTextureViews` supplies these views to every material
consumer. `BindSceneTextureArray` binds them directly rather than substituting
the full array's default view. No material UV or shader sampling change is needed.
Readiness requires one-layer 2D views. Destruction collects unique base images
before releasing them through existing graph/GPU retirement.

Allocation, staging, first-view and subsequent-view failures do not publish a
failed view or poison the per-load cache. An existing page remains valid when
another layer view fails. Failed scene candidates remain subject to the editor's
existing transactional publication checks.

## Verification

Windows, MSVC Release, NVIDIA GeForce RTX 5080, Vulkan validation enabled.
Build directory: `build/asset-integration-release`.

| Check | Result | Evidence |
| --- | --- | --- |
| Full RenderCore suite | 713 passed, 7.063 s | `m6-array-views-rendercore-final-tests.log` |
| Editor render/pick, preview, recook rollback and upload-failure regressions | 8 passed, 5.033 s | `m6-array-editor-regression-tests.log` |
| Native array/standalone sampling, inline and threaded | 2 passed, 0.523 s | `m6-array-views-vulkan-tests-3.log` |
| Editor and scene-renderer demo builds | Passed | `m6-array-apps-build.log` |

Focused mock tests cover one upload per page, exact whole-layer mip bytes,
compatible color views, aliases, separate loads, invalid layers, mutable overrides,
failed allocations/staging/views, and release after both graph and GPU use end.
The editor runs report zero remaining VMA bytes. All three suites report no
tracked CPU leaks. The native sampling run reports no validation errors and
zero remaining VMA bytes in both modes.

`ArraySceneTextureIntegrationTests.cpp` constructs two-layer 5x7 engine images
with three mips for RGBA8, BC7, BC4 and BC5. It samples real production uploads
through `BindSceneTextureArray`, compares all four channels against standalone
images at three coordinates and LODs 0, 0.75 and 1.5, and verifies a repeated
binding. Across both modes, 288 array/reference Vec4 pairs (1152 channels) and
72 alias pairs match exactly. This test uses constructed engine records, not
`CookScene` output. It does not establish material shading or whole-frame parity.
The earlier standalone production-packer anisotropic probe supplies separate
coverage; see `E:/Dev/ZenAssetProcessor/docs/AtlasSamplingEvaluation.md`.

Two earlier native fixture attempts are retained: the first referenced only
sampler slot zero, producing a fixed-size shader declaration rejected by the
bindless interface; the second omitted the required scene-replacement boundary
between cases. The passing fixture uses dynamic sampler indexing and the normal
`PrepareForSceneReplacement` contract. No production safety check was bypassed.

Source, executable, shader and log hashes for this checkpoint are recorded in
`build/asset-integration-release/m6-array-views-identities.json`.

## Cooked-scene checkpoint

`EditorAssetRenderingTest` now runs the source processor, normal forced-recook
v3 publication, package reader, internal scene adapter, native upload and PBR
renderer in one test. It shares setup/readback helpers with `EditorRenderingTest`.
Each temporary source file is deleted after cooking and before loading the package.
The reader therefore runs with its recorded source absent.

The original, self-contained `ZenEditor/Tests/Fixtures/array_scene.gltf` fixture
has five visible instances of three meshes, two material variants, authored
UV transforms, two color maps, two normal maps, two occlusion maps and a repeating
alpha-mask map. `generate_array_scene.py` recreates it without external assets.
The raw case packs three RGBA8 pages. The compressed case packs BC7 color, BC5
normal and BC4 occlusion pages. Each page has two layers; the repeating mask
retains its standalone representation after the alpha-mip coverage rejection.
Logical texture IDs, sampler slots and the five runtime fallback textures remain
unchanged. Authored native image count is seven with packing off and four with
packing on; including fallback images, the totals are twelve and nine.

Both inline and threaded tests pass: **16 array/standalone frame pairs are exact**
at 256x192, across raw/compressed profiles, default/swapped materials, and front/
oblique camera views. This compares 3,145,728 RGBA channels. A nonblank color check
and a required visible variant change prevent empty or ineffective comparisons.
The saved front/default and oblique/swapped compressed captures were also visually
inspected: both layers and shared instances are distinct, and the green mask has
visible holes. Raw-versus-compressed visual quality is not established by these
within-profile comparisons.

The final source-absent run passes 2/2 in 3.802 seconds with no validation errors,
zero remaining VMA bytes in both modes, and no tracked CPU leaks. Shader sources
and SPIR-V hashes are unchanged across the run. After sharing the fixture helpers,
the eight existing editor regressions also pass (8.148 seconds, zero remaining
VMA bytes, no tracked CPU leaks).

Evidence under `build/asset-integration-release`:

- `m6-cooked-arrays-asset-build-4.log`, `m6-cooked-arrays-tests-4.log` and `.xml`;
- `m6-shared-fixture-editor-build.log`, `m6-shared-fixture-editor-tests.log`;
- `m6-cooked-array-captures/`: eight packages, 32 PPM captures and exact PNG
  transcodes, plus `comparison.json` for the 16 saved RGB pairs;
- `m6-cooked-arrays-shader-identities.json`, `m6-cooked-arrays-identities.json`.

Single-fixture package bytes (including metadata and diagnostics) are:

| Profile/mode | Standalone | Array |
| --- | ---: | ---: |
| Raw / inline | 36,953 | 36,585 |
| Raw / threaded | 36,969 | 36,601 |
| BC / inline | 25,753 | 25,401 |
| BC / threaded | 25,753 | 25,417 |

Temporary source-path lengths can affect these package totals; they are recorded
artifacts, not a normalized size or throughput benchmark. The image-count result
does not establish a VRAM saving. Earlier fixture failures are retained in logs:
one assumed authored-only runtime texture counts, and another attempted the legacy
v2 writer. The final fixture accounts for fallback textures, uses normal v3 cache
publication, and tears down GPU state even after an assertion failure.

## Expanded sampling and allocation checkpoint

The subsequent [sampling and allocation measurements](ZenAssetProcessorM6SamplingMeasurements.md)
pass 64 cooked frame pairs, including 128x128 maps under minification and grazing
views. Native gradient/mip-tail comparisons also pass with 16x anisotropy across
RGBA8/BC7/BC4/BC5. Actual resource allocations are equal for 16x16 fixtures and
slightly larger for array pages at 128x128. Texture/sampler binding counts remain
unchanged. This adds sampling evidence without establishing a workload benefit.

## Remaining gate

The mixed fixture establishes cooked-package PBR parity for variants, transformed
UVs, shared instances, array normals and a standalone alpha mask. Array alpha
textures, broader material/corpus coverage and ray-query/GI paths still need
runtime comparisons against packing disabled. The expanded sampling tests above
cover the selected minification/anisotropic cases, not all supported workloads.
Spatial filtering remains unresolved after the recorded candidate failures.
Package size, processing/upload cost and draw counts need comparable workload
measurements. Selected allocation bytes and binding counts are now measured;
reduced VRAM or frame time is not established. Arrays stay optional and disabled
by default, with no editor UI toggle enabled by this checkpoint.
