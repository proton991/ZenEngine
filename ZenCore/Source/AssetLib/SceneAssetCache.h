#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
struct SceneAssetFileMetadata;

// Source-import cache policy lives above the format backends and below runtime
// construction. A hit returns the cached asset; a miss imports the source and
// queues publication on the background writer before returning.
SceneImportResult ImportSceneWithCache(const SceneImportRequest& request, const SceneImportContext& context, bool& cacheHit);

// Writes an asset that ImportScene or the reader already validated. Caches skip
// the durable flush: the payload checksum rejects files left incomplete by a crash.
SceneImportReport WriteValidatedSceneAssetFile(const std::string&            path,
                                               const SceneAsset&             asset,
                                               const SceneAssetFileMetadata& metadata,
                                               SceneImportControl*           control);
} // namespace zen::asset
