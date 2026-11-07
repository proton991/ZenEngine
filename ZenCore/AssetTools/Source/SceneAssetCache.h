#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
struct SceneAssetFileMetadata;

// Source-import cache policy lives above the format backends and below runtime
// construction. A hit returns the cached asset; a miss imports the source and
// queues publication on the background writer before returning.
SceneImportResult ImportSceneWithCache(const SceneImportRequest& request, const SceneImportContext& context, bool& cacheHit);

} // namespace zen::asset
