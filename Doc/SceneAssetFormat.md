# Internal scene assets and source cache

`ZenCore` provides package-only `LoadScene` and `SceneLoader::LoadAsync` for
`.zenasset`. Source-enabled applications link `ZenEngineAssetTools` and use
`LoadSourceScene` or `SourceSceneLoader`; these also accept the processor's glTF,
GLB, FBX and OBJ/MTL sources. `GetSceneLoadFormats` describes runtime packages, while
`GetSourceSceneLoadFormats` combines package and source descriptors for catalog
discovery and editor file filters.

`ImportScene` always parses a source and does not accept `.zenasset`. Its path is
`ZenAsset::ImportScene` for source normalization, `CookScene` for explicit
processing, then `AdaptProcessedScene` for the engine's owning `SceneAsset`.
The background cache writer retains that exact immutable cooked scene. It never
reconstructs processor records from the adapted engine representation. Runtime
construction follows adaptation; neither source parsing nor texture decoding
participates in playback.

`SceneImportSettings` controls source normalization. Separate
`SceneImportRequest::processing` and `profile` fields control cooking. The
default `reference` profile preserves source records and rejects enabled geometry
or texture transformations. `desktop` permits explicit processing; unknown
profiles and invalid settings fail before source parsing or cache lookup.
Cook diagnostics are retained in direct results and package metadata. Failed or
cancelled cooking exposes no partial engine scene or cache publication.

The processor's default source-image allowance is 16 GiB of cumulative decoded
and converted buffers, including temporary decoder copies. This is separate from
compressed source file size and final texture storage. New Sponza's 72 RGBA8 4K
images occupy 4.5 GiB and require at least 9 GiB of this allowance when decoder
copies are counted; the former 8 GiB default rejected that scene during import.
Standalone callers can set `SceneImportContext::maxDecodedImageBytes`, and the
CLI exposes `--image-budget`. A budget failure publishes no partial scene.

## Processor package contract

New automatic caches use the independent version 3 container, described in
[PackageContainer.md](../../ZenAssetProcessor/docs/PackageContainer.md) and
[ScenePackageSchema.md](../../ZenAssetProcessor/docs/ScenePackageSchema.md).
ZenCore reads these through `ZenAsset::Reader` and adapts the decoded scene.
Source importers and encoders remain outside the runtime dependency boundary.
Each package contains the selected scene's supported geometry, materials,
texture payloads, hierarchy, and other imported records. Explicit package loading
does not require the original model, buffer, or image files; source files are
needed for recooking and source-cache freshness checks. Separate editor settings
and externally selected environment maps are not bundled with the model.
Package metadata records the profile and all processing settings. Metadata
versions 1 through 8 remain readable; version 5 stores the initial texture
policy, version 6 adds BC4/BC5/BC6H flags and the HDR quality bound, and version 7
adds optional array-page processing and its four limits. Version 8 adds spatial
page processing and its three limits; default spatial values retain v7 output.
Texture version 2 carries the BC5 positive-Z normal interpretation. Environment
version 2 tags typed RGBA32F or BC6H UFLOAT storage while preserving authored
face/mip order; version 1 defaults to typed RGBA32F. Compressed payloads remain
compressed during adaptation and GPU upload. Current verification and visual
limits are recorded in [M5 evidence](ZenAssetProcessorM5Verification.md).

Image version 2 records the array-layer count and Texture version 3 records a
selected layer. Engine CPU assets and scene components preserve the full page
and logical view; previews borrow only the selected layer's bytes. Legacy v2
writing rejects arrays. The GPU loader shares each immutable native page within
one scene load and binds explicit per-layer 2D views; see the
[native runtime checkpoint](ZenAssetProcessorM6RuntimeVerification.md) and
[M6 integration notes](ZenAssetProcessorM6IntegrationNotes.md).

The standalone schema now also defines Texture version 4 for an integer spatial
atlas region, separate from authored UV transforms. Its bounded Reader previews
crop the logical tile from the physical page. ZenEngine now preserves regions in
CPU assets and scene components, and editor previews crop the selected tile.
Native page sharing and material sampling now support the bounded single-level
profile; see the [GPU checkpoint](ZenAssetProcessorM6SpatialGpuVerification.md).
Runtime materials encode the power-of-two physical page dimensions separately
from authored UV coefficients, avoiding bindless image-size queries while
retaining the existing GPU layout and package schema. The
[page extent checkpoint](ZenAssetProcessorAtlasPageExtentVerification.md)
records exact cooked raster/GI/ray comparisons on AMD and NVIDIA.
Ordinary texture and array records retain their
existing schemas and behavior. See the standalone
[spatial region contract](../../ZenAssetProcessor/docs/SpatialTextureRegions.md)
and [optional spatial cooking](../../ZenAssetProcessor/docs/SpatialCooking.md),
plus the [engine CPU checkpoint](ZenAssetProcessorM6SpatialCpuVerification.md).

Transport compression and mesh semantic encoding are package storage choices.
They do not implicitly run cooking. The engine's automatic writer uses lossless
Zstandard level 12 compression and meshoptimizer stream encoding for every profile,
including Reference. Individual resources retain raw storage when encoding would
not make them smaller. Decoding restores the same cooked scene as a direct import;
GPU texture compression and atlas packing remain separate processing choices.
The writer policy is covered by the tool build fingerprint, so existing raw
caches are regenerated on the next source load.

On 25 October 2026, New Sponza's automatic cache decreased from 5,164,555,104
bytes to 2,107,087,334 bytes (59.2% smaller), compared with 2,171,830,243 bytes
of original model/buffer/image files. All 477 non-mesh, non-provenance resources,
including all 72 image payloads, decoded byte-for-byte identically. The mesh
streams use lossless encoding; canonical scene/cache regression tests and the
runtime reader passed. The editor capture was identical to the raw package.
This machine's first cache publication took 183.4 seconds at level 12; a warm
source-cache load took 6.8 seconds. Compression saves disk space without reducing
decoded GPU texture memory. See the local
[verification results](../build/sponza-cache-compression/final/verification.json)
and [timings](../build/sponza-cache-compression/final/roundtrip.json).

## Legacy version 2 file contract

Schema version 2 is little-endian. The 48-byte header contains:

| Offset | Field |
| --- | --- |
| 0 | Eight ASCII bytes `ZENASSET` |
| 8 | Schema version, unsigned 32-bit |
| 12 | Byte-order marker `0x01020304`, unsigned 32-bit |
| 16 | Three unsigned 64-bit section lengths |
| 40 | XXH64 of the complete section payload, unsigned 64-bit |

The three sections are metadata, semantic records and bulk data, in that order.
The `Fields` and `AssetSection` functions in `SceneAssetIO.cpp` define their
ordered fields and are shared by the reader and writer. Integers use explicit
widths, enums and IDs use unsigned 32-bit values, booleans use one byte, and
IEEE floating-point values are written as their integer bit patterns. Strings
and arrays start with an unsigned 64-bit count. Matrices serialize columns;
quaternions serialize `w, x, y, z`. Invalid optional asset IDs use `UINT32_MAX`.
Arrays of plain numeric records (scalars, vectors, matrices, vertices and joint
influences) are exactly their little-endian memory image, so they move as one
block. Per-vertex texture-coordinate sets are stored as two streams: every
vertex's set count, then all coordinates in vertex order.

Section zero holds only the cache metadata, asset name, source path and
dependency records, so cache lookups inspect it cheaply. Section one starts with
the optional source document, followed by materials and their supported
extension fields, hierarchy, instances, skins, morphs, typed animation targets,
cameras, lights, variants and retained custom attributes. Bulk data contains
vertices, indices, texture-coordinate streams, decoded image pixels and linear
HDR environment maps. Images use RGBA8 storage with UNORM or sRGB views.
Authored image mip arrays include level zero; an empty mip array requests normal
runtime mip generation.

The compatibility writer rejects nondefault texture interpretation and compressed
environment storage, preserving the existing v2 field layout. These resources
use the processor's version 3 package writer.

Files contain no C++ object dumps, addresses, GPU resources, mutable runtime
pose, or preview normalization. Source paths, documents, dependency records,
import diagnostics and cache metadata can be omitted. The runtime payload is
self-contained, including supported animation and decoded textures.

Readers check section arithmetic, counts, booleans, the payload checksum,
references, geometry and topology, node cycles, skin/morph bindings, animation
shapes and targets, image dimensions/mips and environment faces before runtime
creation. The default file and decoded allocation budgets are each 16 GiB;
individual arrays allow up to 512 million elements and strings up to 2 GiB. A
separate 16 GiB budget bounds runtime geometry expansion, including deforming
instances. `SceneAssetReadLimits` can impose smaller budgets. GPU capacity
checks remain the renderer's responsibility. The writer enforces the same
default limits, so a successful write never exceeds the default reader's.

`WriteSceneAssetFile` validates the asset, writes an exclusive, uniquely named
temporary file in the destination directory, checks writes, flushes durably and
then atomically replaces the destination. A failed or cooperatively cancelled
write removes its temporary file and preserves the previous destination. Schema
changes must increment `SceneAssetSchemaVersion`; incompatible explicit files
fail instead of silently consulting their original source.

## Automatic cache

Source `LoadSourceScene` requests use a `.zenasset` cache by default
(`SceneImportRequest::useCache`). An empty `cacheDirectory` selects
`Cache/SceneAssets` in the ZenEngine tree (the `ZEN_SCENE_CACHE_PATH` build
definition); the folder is ignored by Git. Explicit `.zenasset` loads work
regardless of this setting.

A miss imports the source, returns the scene immediately, and queues the cache on
a background writer, so serialization never delays a load.
`WaitForSceneAssetCacheWrites` blocks until queued publications finish and
returns warnings for any that failed. Publication observes both request and
shutdown cancellation through the writer's final commit check. A cancelled write
removes its temporary file and preserves any previous destination. Stale-file
cleanup recognizes both the legacy `.tmp-` and processor `.tmp.` names.

A stable lookup filename hashes the absolute source path, source settings,
profile, every processing option (including disabled-option values), schema,
importer and processor versions, and the engine/processor build fingerprints.
Processing identity uses the format library's field-wise
`ProcessingSettingsHash`, independent of native padding. Engine asset-source
fingerprints are generated by `ZenCore/CMake/SceneImportFingerprint.cmake`;
the processor contributes its own library fingerprint. Editing an importer,
decoder or the format therefore invalidates caches without a manual version bump.
The legacy version 2 key and wire layout exclude the new fields and are unchanged.

Importers record each consumed file with the size and write time at which they
read it (and missing preferred FBX texture paths). Publication hashes those files
with XXH64 only if that revision is still on disk, before and after hashing, and
skips the cache otherwise. A lookup rejects any input whose size changed without
reading it, then compares the content hashes of the rest in parallel on the
processing pool. Content, not modification time, decides validity, so a touched
but unchanged file still hits and an edit that preserves size and time still
misses.

Metadata is read with a 64 MiB allocation budget before stale bulk data is
loaded. Accepting a hit also requires the payload checksum and semantic
validation. A missing, stale or corrupt cache rebuilds from available source.
An automatic cache does not mask missing source files; opening its `.zenasset`
explicitly works without the source. `SceneLoadResult::cacheHit` identifies reuse.

After each publication, least recently used `.zenasset` files beyond
`cacheBudgetBytes` (64 GiB by default) are removed; a hit refreshes a file's
write time. Temporary files older than an hour, left by a crashed writer, are
removed at the same time.

Processor hashing, validation and image decoding use the standalone library's
optimized kernels. Legacy version 2 I/O retains the engine's asset kernels.

## Validation and measurements

`SceneAssetIOTests.cpp` covers semantic round trips, source-free playback,
malformed and truncated files, payload checksum, allocation budgets, atomic write
failure and cancellation, background publication, content, settings and version
invalidation, LRU eviction, escaped external URIs, concurrent mixed-format
requests and the shared processing that drops unplayable clips. The source
corpus harness disables automatic caching so every corpus run exercises the
backends.

The cooking integration tests compare complete engine record serialization for
direct imports, explicit cooked packages, cache misses and hits. Fixtures cover
actual vertex reordering, all supported primitive modes, custom attributes and
FBX skin/morph animation. They check unchanged reference output and version 2
bytes/keys, profile and option invalidation, retained diagnostics, rejected and
cancelled requests, and cancellation during a background package write.

The optional [processor-owned integration suite](../../ZenAssetProcessor/tests/integration/zenengine/README.md)
builds `SceneLoadBenchmark`; it is not part of the normal engine build.
`SceneLoadBenchmark <scene> --cache-dir=<dir>` times a cold load, the background
publication and warm loads. On this workstation (Ryzen 9 9950X3D, NVMe, files in
the OS cache), the earlier version 2 implementation with Intel's main Sponza glTF
(`E:/Dev/main_sponza/NewSponza_Main_glTF_003.gltf`: 72 4K PNG textures, 2.0 M
vertices, 11.2 M indices, a 5.16 GB internal file) measured:

| Build | Cold load | Background publication | Warm load |
| --- | --- | --- | --- |
| Release | 2.4 s | 3.7 s | 1.2 s |
| Debug | 5.5 s | 3.1 s | 1.9 s |

Before this work the Release cold load took 34.5 s (14.1 s import plus 20 s of
synchronous cache writing) and the warm load 20.3 s. The main costs were
whole-file SHA-256 fingerprints, per-scalar file I/O, quadratic growth of
range appends in `HeapVector`, and unoptimized decoding in Debug.
