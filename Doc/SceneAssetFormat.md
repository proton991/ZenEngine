# Internal scene assets and source cache

`LoadScene` and `SceneLoader::LoadAsync` accept `.gltf`, `.glb`, `.fbx` and
`.zenasset`. Source backends implement `ISceneImporter` and return an owning
`SceneAsset`. Internal files deserialize that same asset directly. Both paths
validate the asset once and build the runtime scene from it; neither parsing nor
texture decoding participates in playback.

`ImportScene` always runs a source backend and intentionally does not accept
`.zenasset`. `GetSceneImportFormats` describes those backends;
`GetSceneLoadFormats` also includes the internal format. Catalog discovery,
editor file filters and command-line loading use the latter registry.

Every backend finishes with shared processing (`ProcessImportedAsset`): clips
that cannot play (for example non-increasing key times or zero-length rotation
keys) and morph weight lists that do not match their targets are dropped with a
warning naming the object, instead of rejecting the whole scene. The result is
then validated once; a failure names the invalid area.

## File contract

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

Source `LoadScene` requests use a `.zenasset` cache by default
(`SceneImportRequest::useCache`). An empty `cacheDirectory` selects
`Cache/SceneAssets` in the ZenEngine tree (the `ZEN_SCENE_CACHE_PATH` build
definition); the folder is ignored by Git. Explicit `.zenasset` loads work
regardless of this setting.

A miss imports the source, returns the scene immediately, and queues the cache on
a background writer, so serialization never delays a load.
`WaitForSceneAssetCacheWrites` blocks until queued publications finish and
returns warnings for any that failed. A pending publication is cancelled, and its
temporary file removed, when the process exits.

A stable lookup filename hashes the absolute source path, all import settings,
schema, importer and processor versions, and the build fingerprint: a hash of
every source that shapes an imported asset (`ZenCore/Source/AssetLib`, its
kernels, `ZenCore/Include/AssetLib` and the pinned dependency list), generated at
build time by `ZenCore/CMake/SceneImportFingerprint.cmake`. Editing an importer,
decoder or the format therefore invalidates caches without a manual version bump.

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

Hashing, validation and image decoding run in a small library that is compiled
with optimizations in every configuration
(`ZenCore/Source/AssetLib/Kernels`), so Debug builds load large scenes at nearly
Release speed.

## Validation and measurements

`SceneAssetIOTests.cpp` covers semantic round trips, source-free playback,
malformed and truncated files, payload checksum, allocation budgets, atomic write
failure and cancellation, background publication, content, settings and version
invalidation, LRU eviction, escaped external URIs, concurrent mixed-format
requests and the shared processing that drops unplayable clips. The source
corpus harness disables automatic caching so every corpus run exercises the
backends.

`SceneLoadBenchmark <scene> --cache-dir=<dir>` times a cold load, the background
publication and warm loads. On this workstation (Ryzen 9 9950X3D, NVMe, files in
the OS cache), Intel's main Sponza glTF
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
