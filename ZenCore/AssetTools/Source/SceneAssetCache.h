#pragma once
#include "AssetLib/SceneImporter.h"

namespace zen::asset
{
struct SceneAssetFileMetadata;

// Source-import cache policy lives above the format backends and below runtime
// construction. A hit returns the cached asset; a miss imports the source and
// queues publication on the writer. Runtime-only loads await publication and
// reopen the native package so decoded source textures do not remain resident.
SceneImportResult ImportSceneWithCache(const SceneImportRequest&                  request,
                                       const SceneImportContext&                  context,
                                       bool&                                      cacheHit,
                                       SharedPtr<SceneAsset, MultiThreadCounter>* runtimeOwner = nullptr);

} // namespace zen::asset
