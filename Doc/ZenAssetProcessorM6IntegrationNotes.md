# Atlas integration constraints

M6 remains open. The standalone processor has spatial and array construction
primitives, described in `E:/Dev/ZenAssetProcessor/docs/AtlasPacking.md`. GPU tests
rejected the proposed manual logical-mip sampler and guarded native spatial chain
under the existing one-byte channel gate. Array layers, including actual
`PackArrayPages` output sampled through individual 2D views, matched exactly with
16x anisotropy on the tested RTX 5080. See the standalone
`docs/AtlasSamplingEvaluation.md` for fixtures, failed alternatives and scope.
Native GPU array-page loading now shares uploads and binds individual layer views
in the engine. Full-scene and workload acceptance remain open.

The processor already preserves stable texture IDs, per-binding authored UV
transforms and immutable image owners. Atlas page/region metadata must remain
separate from the authored transform and apply afterward. Reuse the existing
binding-role analysis and animation exclusions; do not flatten material variants
or move geometry UVs merely to combine unrelated bindings.

`TextureManager::LoadSceneTextures` shares each immutable array page within one
scene load, uploads all layers once, and caches compatible layer/format views.
Different scene loads remain independently owned. Standalone images retain
their existing per-texture upload and mip-generation behavior. The scene stores
borrowed views and retires each unique base image once through the existing
graph/GPU lifetime path. See the [native checkpoint](ZenAssetProcessorM6RuntimeVerification.md)
for failure, ownership and sampling evidence.

Filtering needs an explicit design before packing. A level-zero gutter shrinks
with every mip and cannot by itself isolate the tail. The chosen representation
must retain tile-local mip content and protect every supported trilinear and
anisotropic footprint, including oblique views and the smallest usable mip.
A sampler LOD clamp alone changes authored minification unless that behavior
is part of a tested eligibility rule. Do not hide this change behind packing.
Compression follows completed page/mip construction, with block alignment at
every stored level and an explicit policy for normal/alpha quality fallback.

Initial eligibility remains the implementation plan's bounded subset: static,
nonrotated tiles; compatible role, color transfer, sampler and mip policy;
effective UVs within the tile domain. Repeat/mirror wrapping, animated UV or
texture state, UV morphs and unsupported filtering stay standalone with reasons.

Verification must include actual packed output, mixed atlas/standalone scenes,
variants and shared instances. The existing policy permits at most one 8-bit
channel step before lossy encoding and zero bleed/wrapping failures. Record
atlas on/off package size, native texture allocation, binding changes, draw
calls and processing time. A packed file alone does not demonstrate GPU memory
or batching benefit.

## Implementation checkpoints

The proposed first bounded implementation was explicitly clamped, single-level,
linear-filtered textures with no anisotropy. Subsequent GPU tests reject the
tested spatial mappings even within that subset: cropped and power-of-two pages
reach two-step channel errors on expanded coordinate grids, and separated
`precise` mapping arithmetic does not resolve them. The array control is exact.
See `E:/Dev/ZenAssetProcessor/docs/SingleLevelAtlasEvaluation.md` for completed
runs, scope and preserved failures. Do not publish spatial metadata or enable
that candidate based on the former evenly spaced grid alone. The sampling design
must pass the original gate before integration; M6's full scope remains open.
Follow-up tile-coordinate software filtering fails as well. Snapping coordinates
before native filtering meets the channel gate on the first expanded grids but
fails additional half-subtexel/adjacent-float boundary cases. Array sampling stays
exact in the same control. Preserve those boundary cases in subsequent designs;
the first grid subset is not sufficient even for an opaque-only spatial path.

A later FP32 half-up candidate is exact across 720 power-of-two, single-level
operations on fourteen-dimension expanded sampling tests with anisotropy disabled.
The 288 other-dimension operations still contain 84 failing comparisons; actual
16x anisotropy fails 526 of all 1,008 comparisons, including power-of-two images.
Array controls remain exact across all dimensions with either sampler setting.
The processor now constructs optional bounded power-of-two pages directly, and
the probe produces identical sampling/allocation records using those pages.
The final default probe uses no FP64 shader capability or enabled device feature.
See `E:/Dev/ZenAssetProcessor/docs/AtlasCoordinatePrecisionEvaluation.md` for
the complete matrix, preserved failures, evidence snapshots and reproduction.

`RenderScene::LoadSceneTextures` copies authored filter/wrap/mipmap settings into
`RHISamplerCreateInfo`, whose anisotropy defaults to disabled. This is relevant
to evaluating the bounded candidate; it does not authorize changing an authored
filter, truncating a mip chain or skipping all-use analysis. Spatial cooking and
runtime metadata remain disabled pending integration/visual evidence. The
construction change passes 1,327 packing checks in each configuration, 33/33
full Release tests and three affected Debug suites. It does not close M6.

The next standalone checkpoint adds `TextureAsset::atlasRegion` and Texture
schema v4, with strict single-level RGBA8 region/sampler validation and bounded
cropped Reader previews. Raw/Zstandard round trips retain logical IDs, aliases,
page bytes and authored UV transforms. Ordinary texture/array records keep
their existing schemas. The engine adapter now explicitly reports a spatial
region as unsupported before constructing an internal asset; this is a temporary
boundary pending the runtime work below. Automatic spatial cooking remains
disabled. See `E:/Dev/ZenAssetProcessor/docs/SpatialTextureRegions.md`.

That checkpoint passes 33/33 full standalone Release suites, four affected Debug
suites, four Reader-only Release suites and 39 engine adapter/I/O/cache tests
with no tracked leaks. Actual application loads verify the current adapter's
explicit spatial rejection, an unchanged full-image control, and the older
installed reader's unsupported-version response. These are format/CPU boundary
checks, not spatial rendered-image or GPU lifetime acceptance.

Every use in a future spatial implementation must qualify, including
variants and extra UV sets. Unknown/default sampler behavior, repeat/mirror/border
wrapping, animated texture or UV bindings and UV morphs retain standalone storage
with reasons. Node or skeletal animation alone need not disqualify static UVs.
Single-level evidence would not complete the broader M6 mip/filter gate.

Use integer logical dimensions and an interior rectangle on each logical texture;
the image reference points to a shared page. Preserve texture IDs and authored UV
transforms. Emit an explicit one-level page mip array so the engine cannot infer
that it should generate a physical-page mip chain. Deterministic, unrotated packing
orders compatible groups by size and stable texture ID, with fixed tie-breaks,
bounded page dimensions/count and memory. Block-aligned guarded rectangles copy
original pixels and extrude edges. Reject unprofitable groups. Compress completed
pages only after per-tile quality checks, with whole-page raw fallback if needed.

Keep region mapping inside material sampling helpers, after authored UV transforms.
`MaterialTransformedUV` also supplies tangent-basis derivatives; changing its
returned UVs to page coordinates would change normal shading. The direct
`textureLod` alpha test in `RayCandidateAccepted` needs the same region-aware
explicit-LOD helper. Bounded editor previews must crop the logical tile rather
than display the whole page.

Native page sharing is local to one scene load and keyed by immutable owner,
compatible GPU format, dimensions and mip policy. Mutable overrides and failed
uploads cannot enter that cache. Existing release paths deduplicate native texture
pointers; verify aliases, retirement and independent repeated loads. Initial page
sharing can reduce images and uploads while leaving descriptor slots and draw
counts unchanged.

The spatial kernel's separately guarded logical mip rectangles are still useful
construction data, but manual blending is not accepted as the general mipmapped
wire/sampling path. GPU integer-level samples matched exactly, while manual
blending of the original texture's levels reproduced the remaining error.
Increasing the spatial native-chain gutters also exceeded the visual gate and
substantially increased allocation bytes. Preserve these failures rather than
weakening the threshold or clamping authored minification.

For compatible mipmapped images, the next integration path is native array pages.
`ImagePixels::arrayLayers` and `TextureAsset::arrayLayer` are now independent
format fields. Image v2 and Texture v3 serialize them, while ordinary records
retain their earlier schemas. Every level contains complete consecutive layers;
arrays require explicit full or partial chains. The bounded deterministic
`PackArrayPages` primitive preserves RGBA8/BC7/BC4/BC5 bytes, reports singleton
images as standalone mappings, and refunds all storage on failure/cancellation.
Copying finalized compressed layers preserves existing per-image quality and
block isolation without requiring re-encoding. The standalone test suite exercises
layer records, malformed fields with valid checksums and selected-layer previews.

Optional `--atlas array` cooking now analyzes all material uses and copies
eligible finalized layers without changing texture IDs, materials or UVs.
API/manifest settings include bounded dimension/layer/page/input counts and
participate in the processing identity and Metadata v7. Tests cover variants,
node overrides, aliases, multi-page/singleton groups, BC7/BC4/BC5 payloads,
exclusions, cancellation, budget failures and source-free CLI validation. See
the standalone `docs/ArrayCooking.md` for the contract and evidence.

Engine CPU image owners now carry the layer count; texture records and scene
components retain the selected layer. Validation checks whole-page footprints,
explicit mip chains and layer bounds. Legacy v2 writing rejects arrays rather
than dropping their semantics. Editor previews borrow only the selected layer
of the chosen mip; mutable local overrides use a standalone image.

The Release CPU checkpoint passes 38 adapter/package-I/O/cache tests and four
preview tests, both with no tracked CPU leaks. These include RGBA8/BC7/BC4/BC5
layer payloads, shared runtime CPU owners, selected-mip/layer previews and legacy
destination preservation. Logs are `build/asset-integration-release/` files
`m6-array-cpu-tests.log` and `m6-array-preview-tests.log`.
Three focused RenderCore mock tests also pass with no tracked CPU leaks, including
rejection of both array layers before native texture creation or upload queuing.
That evidence is `m6-array-gpu-boundary-tests.log`; it verifies the temporary
rejection boundary, not native GPU array support. Source, executable and log
hashes are in `build/asset-integration-release/m6-array-cpu-identities.json`.

The temporary rejection boundary has since been replaced by native array
uploads and explicit per-layer 2D views. `BindSceneTextureArray` now accepts the
scene's views directly; all material consumers use that representation. Existing
sampler2D material shaders retain authored UVs and gradients. The Release suite
passes 713 RenderCore tests, eight editor regressions and two native Vulkan
sampling cases (inline/threaded). RGBA8, BC7, BC4 and BC5 array samples match
standalone references exactly at the tested mips and coordinates. These native
fixtures use constructed engine records. A subsequent source-absent cooked-scene
test verifies 16 exact PBR frame pairs with raw and compressed pages, material
variants, transformed UVs, shared instances and a standalone repeating mask.
The [native checkpoint](ZenAssetProcessorM6RuntimeVerification.md) records both
sets of evidence and the remaining broader visual/workload scope.

Array probing reduced native images from three to one, but dedicated allocation
bytes were equal or slightly larger. Keep optional all-use eligibility and a
measured profitability decision. The 864 distinct GPU queries per array run are
RGBA8 clamp-to-edge probe evidence, not compressed/material/whole-scene coverage.
Spatial eligibility, runtime mixed scenes/variants, wrapping behavior and real
workload measurements still need to satisfy the complete M6 gate.


## Spatial cooking checkpoint

The standalone processor now accepts `--atlas spatial` and equivalent API and
manifest settings. A shared page processor qualifies every material use for
both array and spatial modes. Eligible single-level power-of-two RGBA8 images
receive Texture v4 integer regions and copied/extruded texels; unsupported mip
chains, formats, UV use and sampling remain standalone with diagnostics.
Metadata v8 stores the spatial policy, while default spatial values retain v7.
See `E:/Dev/ZenAssetProcessor/docs/SpatialCooking.md` for limits and verification.

The [engine CPU checkpoint](ZenAssetProcessorM6SpatialCpuVerification.md) now
preserves regions and shared pixel owners through adaptation, package loading
and runtime scene construction. Editor previews crop logical tiles. It passes
42 adapter/I/O/cache tests, 47 editor-model tests, 714 RenderCore mock tests and
three source-absent application load checks. Legacy v2 writing rejects regions.

The [GPU checkpoint](ZenAssetProcessorM6SpatialGpuVerification.md) now shares
native spatial pages and samples per-binding regions separately from authored
UV transforms. Native material and explicit alpha sampling match exactly in both
execution modes; cooked source-absent scenes pass 24 exact frame pairs including
clearcoat normals, variants and grazing views. Invalid material-region/sampler
updates leave published data unchanged. The fixture reduces seven native images
to four but raises VMA allocation from 623,616 to 1,661,952 bytes. General
mip/filter/compression support, full GI coverage and measured workload benefit
remain open; this does not close M6.

The subsequent [cooked spatial ray checkpoint](ZenAssetProcessorM6SpatialRayVerification.md)
verifies production software/hardware alpha traversal in compute and fragment
shader contexts, both execution modes, source-absent packages and material
variants. All 393,216 paired rays match standalone textures exactly. Opaque
material updates close the holes in 393,216 control queries. This adds bounded
ray-query coverage; it does not substitute for full GI rendering or workload
measurements.

The [full GI repeatability diagnosis](ZenAssetProcessorM6SpatialGiRepeatability.md)
finds one-byte frame variation for identical standalone packages in both
providers. Temporarily disabling guide learning removes it, and restoring the
original shader restores the variation. All diagnostic production edits were
reverted. The subsequent [exact guide capture checkpoint](ZenAssetProcessorGuideCaptureVerification.md)
adds an opt-in accumulator that retains guide learning. Its eight spatial/GI
reference pairs and eight identical-package repeats are exact, with both query
providers and execution modes. Broader sampling/device and workload gates remain
open; normal rendering retains its original accumulation cost.
