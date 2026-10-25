#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
inline constexpr uint32_t SceneAssetSchemaVersion    = 2;
inline constexpr uint32_t SceneAssetProcessorVersion = 1;

using SceneAssetFileInput                            = SceneDependency;

struct SceneAssetFileMetadata
{
    std::string sourceFormat;
    uint32_t    importerVersion{0};
    uint32_t    processorVersion{SceneAssetProcessorVersion};
    // Hash of the engine sources that shaped the asset; caches miss when it changes.
    std::string buildFingerprint;
    // v3 package build profile. Legacy v2 files imply the reference profile.
    std::string                       profile{"reference"};
    zenasset::ProcessingSettings      processing;
    SceneImportSettings               settings;
    std::string                       cacheKey;
    HeapVector<SceneAssetFileInput>   inputs;
    HeapVector<SceneImportDiagnostic> diagnostics;
};

struct SceneAssetReadLimits
{
    uint64_t maxFileBytes{16ull * 1024 * 1024 * 1024};
    uint64_t maxAllocationBytes{16ull * 1024 * 1024 * 1024};
    uint64_t maxArrayElements{512ull * 1024 * 1024};
    // Source documents of embedded-buffer glTF files can be hundreds of megabytes.
    uint64_t maxStringBytes{2ull * 1024 * 1024 * 1024};
    uint64_t maxRuntimeGeometryBytes{16ull * 1024 * 1024 * 1024};
};

struct SceneAssetFileResult : SceneImportResult
{
    SceneAssetFileMetadata metadata;
};

// Standalone CPU asset I/O. The reader never invokes a source importer or image
// decoder. The writer replaces the destination only after a complete, flushed
// temporary file has been closed successfully.
SceneAssetFileResult ReadSceneAssetFile(const std::string&          path,
                                        const SceneAssetReadLimits& limits  = {},
                                        SceneImportControl*         control = nullptr);
// Bounded metadata inspection for cache lookup. It does not validate payloads;
// callers must use the full reader before accepting any runtime asset.
SceneAssetFileResult ReadSceneAssetMetadata(const std::string& path, SceneImportControl* control = nullptr);
// Hashes schema, versions, build fingerprint, settings and input fingerprints using
// the same field encoding as the file. Diagnostics and cacheKey are excluded.
std::string       SceneAssetCacheKey(const SceneAssetFileMetadata& metadata);
SceneImportReport WriteSceneAssetFile(const std::string&            path,
                                      const SceneAsset&             asset,
                                      const SceneAssetFileMetadata& metadata = {},
                                      SceneImportControl*           control  = nullptr);
// LoadScene publishes caches on a background writer after returning the scene.
// This blocks until every publication started so far has finished, and returns
// warnings for publications that failed since the previous call.
HeapVector<SceneImportDiagnostic> WaitForSceneAssetCacheWrites();
} // namespace zen::asset
