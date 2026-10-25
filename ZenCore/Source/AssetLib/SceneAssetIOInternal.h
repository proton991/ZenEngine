#pragma once
#include "AssetLib/SceneAssetIO.h"

namespace zen::asset
{
std::string SceneAssetCacheKeyForVersion(const SceneAssetFileMetadata& metadata, uint32_t schemaVersion);
// Writes an asset that ImportScene or the reader already validated. Caches skip
// the durable flush: the payload checksum rejects files left incomplete by a crash.
SceneImportReport WriteValidatedSceneAssetFile(const std::string&            path,
                                               const SceneAsset&             asset,
                                               const SceneAssetFileMetadata& metadata,
                                               SceneImportControl*           control);
} // namespace zen::asset
