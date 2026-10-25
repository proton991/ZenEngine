# ZenAssetProcessor M5 integration notes

**Inspected:** 23 October 2026. **Status:** historical pre-implementation map.
The observations below describe the initial boundary, before the BC7 slice and
subsequent BC4/BC5 work. See [current verification](ZenAssetProcessorM5Verification.md)
and [BC7 visual evidence](ZenAssetProcessorM5VisualVerification.md) for implemented
behavior and remaining gates. The user's active GI changes must be preserved;
this work concerns texture contracts and common material sampling.

## Decision and one cooked scene

Start the desktop profile with **BC7 color textures and complete offline mip
chains**, retain explicit uncompressed fallback per image, then add BC5 normals,
BC4 scalar bindings, and BC6H environment faces through the same storage path.
BC7 retains RGBA sampling, making it the first usable renderer integration.
This order does not remove BC5/BC4/BC6H or the quality gates from M5.

Add `Processor/Cook.h` with `CookSettings` and `CookScene` returning a status,
diagnostics/quality decisions, and an owning `shared_ptr<const Scene>`. Settings
contain an explicit profile, versioned mip/encoding parameters, limits and
cancellation; they are recorded in BuildMetadata/cache identity. Keep
`ImportScene` as source normalization. Both CLI and engine source facade run
`ImportScene -> CookScene`; the engine adapter and async writer receive **the
same cooked scene**. `WriteScene` handles package/storage encodings, not mip,
material or atlas policy. `ReadScene` validates and owns the stored result; it
must not rerun cooking or silently select a quality profile. A future atlas
pass belongs inside this cooking sequence before final mip generation/encoding.

The concrete engine boundary is `AssetTools/Source/SceneSourceImporter.cpp`,
`ProcessedSourceImport.h`, and `SceneAssetCache.cpp`. Replace the retained
imported scene there with the cooked owner before both adaptation and cache
publication. Extend cache settings/build metadata together; a direct desktop
load must not show reference images while its cache hit shows compressed ones.
Test direct cooked adaptation against write/read/adaptation for each profile.

## Current contracts that need changing

| Layer / exact file | Observed contract and required work |
| --- | --- |
| Standalone `include/ZenAsset/Format/Image.h`, `src/Format/Scene.cpp` | Format IDs and block-size helpers already describe BC4/5/6H/7, but this is metadata, not support. `bytes` owns the base level and `mips[0]` duplicates it when mips exist. Require matching base bytes and checked per-level footprints; retain the current ownership convention during M5. |
| Standalone `src/Format/Validation.cpp::ValidImages` | Accepts only RGBA8 and computes `width * height * 4`. Use the existing format/block helpers, validate view compatibility and all mip lengths, and reject unknown layouts before allocation/upload. |
| Standalone `src/Format/SceneSchema.h` / Reader and Writer | Serialize the cooked format and actual mip bytes. Version any new texture interpretation or environment payload fields; keep old raw schemas readable. Package limits must include encoded image storage, previews/decoded fallback allocations, and working mip buffers. |
| Engine `ZenCore/Source/AssetLib/ProcessedSceneAdapter.cpp::Copy(Format&, ...)` | Maps only RGBA8 and rejects other formats. Add explicit mappings, retain immutable pixel ownership, and preserve the cooked interpretation. |
| `ZenCore/Source/AssetLib/SceneAssetValidation.cpp::ValidImages` | Mirrors the RGBA8-only validator. Validate engine formats with the same dimensions/block rules without importing processor policy. |
| `ZenCore/Include/SceneGraph/Texture.h` / `SceneRuntimeBuilder.cpp` | Shared image bytes survive source-owner destruction. Carry the format, offline mips, and required interpretation through this existing path. |
| `ZenCore/Source/Graphics/RenderCore/V2/TextureManager.cpp::LoadSceneTextures` | Has a complete authored-mip upload path with one region per level. Reuse it for BC; never request runtime mip blits for compressed cooked images. The current concatenation has a checked uint32 upload limit. Propagate failures instead of exposing partially uploaded textures. |
| `ZenCore/Include/Graphics/Common/Format.h` | `DataFormat` has no BC entries. Pixel-size helpers and memory budgeting assume individual texels. Add authoritative block width/height/bytes and checked mip size helpers; do not pretend a 16-byte BC block is a 16-byte texel. |
| `ZenCore/Source/Graphics/RenderCore/V2/RDGCopyValidation.h` | Footprints multiply width/height by texel bytes. Validate block-aligned offsets, edge mip extents, padded block rows, buffer alignment, and queue transfer granularity. Use ceiling block counts, including 1x1 mips. |
| `ZenCore/Source/Graphics/RenderCore/V2/RDGResourceManager.cpp::EstimateBytes` | Texture budget estimates sum texels times a fixed byte count. Change to per-mip block counts so pooling budgets remain meaningful. |
| `ZenCore/Source/Graphics/VulkanRHI/VulkanTexture.cpp` | Creation rejects unknown pixel size; view compatibility equates equal byte size for current uncompressed color formats. Add BC format classes and allow only actual compatible linear/sRGB pairs. Query sampled/transfer format support and retain image-format limits. |
| `ZenEditor/Model/Source/SceneAssets.cpp::MakeTexturePreview` | Rejects BC and directly samples RGBA8 base bytes. Add bounded block decoding/downsampling through a decoder-only API. `EditorViewport.cpp` requests 128-pixel previews; preserve that bound. Update `ZenEditor/ImGui/Source/EditorWidgets.cpp` format labels. |

For Vulkan copies, buffer offsets and image regions must satisfy compressed
texel-block rules as well as the selected queue's transfer constraints. Keep
extent values in texels in native copy regions; compute payload sizes in blocks.
Use the [Vulkan copy specification](https://docs.vulkan.org/spec/latest/chapters/copies.html)
to derive validation cases rather than weakening the existing checks.

## Roles, shared views and shader interpretation

Construct a deterministic usage list from the five core material slots, all
17 feature bindings, variants and typed animation targets. A source image can
serve sRGB base color, linear scalar data, normal and clearcoat normal roles.
One shared byte array is not automatically suitable for their different mip
filters. Split cooked images/textures when role, color transfer, channel usage,
alpha cutoff, sampler edge policy or normal treatment differs; rewrite each
affected material binding while retaining node/material IDs and animation
target identities. Compatible BC7 linear/sRGB views may share encoded bytes
only when their complete mip content and processing choices agree.

| Role | Encoding and necessary semantics |
| --- | --- |
| Color / emissive | BC7 sRGB for color transfer, BC7 UNORM for declared linear color. Filter RGB in linear light; encode to the intended stored transfer afterward. Preserve alpha independently. Evaluate actual sampled linear RGB, not encoded-byte PSNR. |
| Normal / clearcoat normal | BC5 UNORM XY, only after shader reconstruction and angular validation. Decode XY to [-1,1], reconstruct positive Z, apply the existing normal scale to XY, then normalize using the existing tangent frame. Authored negative-Z or other nonrepresentable data keeps RGBA storage when the angular gate fails. Both base and clearcoat paths must carry an explicit XY-normal interpretation. |
| Scalar | BC4 for a proven single consumed channel. Extract that channel to R and preserve the binding's channel meaning. Red occlusion is the first simple case. Green/blue/alpha scalar slots need explicit channel replication/remapping in shared sampling or image-view metadata; do not emit BC4 and leave a shader reading zero from G/B. Packed metallic/roughness and unrelated multi-channel uses remain BC7 linear or uncompressed. |
| HDR environment | BC6H UFLOAT for nonnegative RGB, SFLOAT only with signed encoding/decoding tests; alpha must be semantically unused/one. Keep float storage for out-of-range, nonfinite/rejected, or quality-failing payloads. Existing irradiance coefficients remain floats. |

`Data/Shaders/Common/material_surface.glsl::SurfaceNormal` and
`ClearcoatNormal` currently read XYZ directly. Publish the XY-normal marker
from `sg::Material::SetData` through a documented GPU field and use one decoding
helper for both paths. Do not overwrite sampler indices stored in
`textureTransforms[].row0.w`; shader layout/host validation must agree on the
new field. Format alone must not cause arbitrary BC5 data textures to become
normals. Keep `flatNormal` behavior unchanged.

The inspected GI and ray-query material paths are GPU consumers, not CPU image
samplers: `Common/material.glsl` samples the texture heap,
`VoxelGI/voxel_surface.glsl` obtains albedo/occlusion through those helpers,
and `VoxelGI/scene_ray_geometry.glsl` performs alpha occlusion. The search of
current engine `GetBytes`, `GetMipBytes` and raw image storage consumers found
upload and editor preview access, not a separate GI CPU decoder. Compression
must still preserve the alpha decision shared by raster, shadows and ray/GI
occlusion; do not introduce a special GI texture path.

HDR is a separate representation change: `ImageBasedLightAsset` currently owns
`vector<vector<Vec4>> specularMipFaces`; engine `UploadEnvironmentCube` hardcodes
RGBA32F. Add an owning, tagged cube payload (format plus bytes per mip/face),
with exactly six faces at each mip and checked sizes. Adapt old raw face records
to RGBA32F. Upload cooked BC6H faces directly; do not decode back to floats just
to upload them. `LoadSceneEnvironment` can retain its float SH-to-irradiance
generation. Existing GLI environment file loaders explicitly admit only float
cubes and are not the cooked package decoder or BC encoder.

## Selected codec integration

Use small CPU block APIs with deterministic settings and bounded worker jobs:

- **BC7 and BC4/5:** select the scalar `bc7enc.cpp` and `rgbcx.cpp` from
  [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), without its CLI, ISPC or
  RDO machinery. Initialize shared tables once before worker use. The
  [BC7 API's bool result](https://raw.githubusercontent.com/richgel999/bc7enc_rdo/master/bc7enc.h)
  reports alpha presence, not failure. [rgbcx](https://raw.githubusercontent.com/richgel999/bc7enc_rdo/master/rgbcx.h)
  supplies BC4/BC5 block encoders; its initialization changes global state.
- **BC6H:** select the dependency-free
  [bc6h_enc block library](https://github.com/0xc0de/bc6h_enc), derived from
  DirectXTex and exposing unsigned/signed encoders. Validate output with an
  independent decoder and GPU samples before enabling it in the profile.
- **Preview, quality and device fallback decoding:** select
  [bcdec](https://github.com/iOrange/bcdec) behind a decoder-only Reader utility.
  It decodes one block without allocating. Use its precise BC4/5 option and
  an aligned local block buffer. Use our checked ceiling-block size helpers,
  not upstream whole-image macros that assume dimensions divisible by four.

These are selections for implementation, not already pinned dependencies.
Resolve immutable commits, archive hashes and licenses before adding CMake
targets; keep encoders private to Processor. Reader-only builds may link the
small block decoder, never encoders or source importers. The current Basis
v1_60 wrapper compiles only `basisu_transcoder.cpp` for source KTX2 import.
Its encoder internals expose partial compression helpers and broad image/tool
dependencies; they are not a ready standalone BC7/BC6H block API. Retain that
working importer instead of upgrading Basis as part of M5. GLI is useful for
existing container loading, not this encoding boundary.

## Mips, fallback and acceptance

Preserve authored mip levels in the reference profile. Desktop generation is
explicit: keep authored levels and generate missing tails, or rebuild only
under a recorded option. Generate color mips in linear light, normal mips by
averaging decoded directions then renormalizing, scalar mips in linear space,
and alpha-mask mips with coverage validation at each actual cutoff. Shared or
animated cutoffs that cannot satisfy a single baked chain must be split or
retain the existing uncompressed behavior with a diagnostic. A whole cooked
image uses one format across all levels; a failing level rejects compression
for that image, not just that mip. Check overflow, working-memory limits and
cancellation between bounded blocks/rows.

The quality contract remains [asset_processor_quality.json](../../ZenAssetProcessor/tools/validation/asset_processor_quality.json):
linear RGB PSNR >=40 dB and RMSE <=0.01; normal RMS <=1 degree and max <=5
degrees; scalar max error <=8/255; per-mip alpha coverage error <=0.01; HDR
relative RMSE <=0.02; no nonfinite output. Measure each image and mip rather
than averaging away failures. Preserve float/RGBA fallback and a reason in
diagnostics. The reference profile remains bit-exact.

Bound preview output to the requested size, choose a suitable stored mip, and
decode only needed 4x4 blocks into fixed scratch storage; never allocate a full
RGBA copy merely to produce a 128-pixel thumbnail. CPU device fallback is a
separate, explicit full-image budgeted operation before GPU publication. Preserve
normal/channel interpretation across fallback and retain the encoded owner;
unsupported device formats must not silently produce null texture bindings.

## Bounded implementation division

1. **Standalone cooking owner:** Cook API/settings, usage classification,
   deterministic role split, mip generation, BC7 encoder and quality fallback,
   schema/validation and direct-versus-read tests. Begin with BC7; extend the
   same API for BC5/4 and tagged HDR cubes after their consumer gates pass.
2. **Texture/RHI owner:** explicit format mappings, block footprints and memory
   budgets, copy/view validation, authored-mip uploads, capability fallback.
   Limit edits to the mapped files; coordinate ownership before touching shared
   rendering files while GI work is active.
3. **Consumers/acceptance owner:** decoder-only bounded previews, UI labels,
   shared normal/channel shader semantics, quality fixture generation and GPU
   comparisons. Core and clearcoat normals must land together.

Required checks include 1x1/3x5/5x7 textures, full/partial chains, tiny mip block
padding, truncated/mismatched levels, overflow/budgets, cancelled jobs, shared
color/data/normal uses, HDR range, alpha-tested animation, sRGB-view correctness,
unsupported-device fallback, editor scene-switch lifetimes, source-free runtime
loading, and validation-clean uploads. Report package size, actual uploaded
texture bytes, staging bytes, CPU peaks and repeatable timings. BC quality,
memory and first visible publication must pass before enabling the desktop
profile by default; successful format enum compilation alone is insufficient.
