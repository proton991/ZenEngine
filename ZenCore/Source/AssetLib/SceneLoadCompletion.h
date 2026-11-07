#pragma once
#include "AssetLib/SceneLoader.h"

namespace zen::asset
{
// Shared runtime construction/publication for validated package and source paths.
SceneLoadResult CompleteSceneLoad(const SceneImportRequest&                 request,
                                  SceneImportResult                         imported,
                                  bool                                      cacheHit,
                                  SharedPtr<SceneAsset, MultiThreadCounter> runtimeOwner = {});
} // namespace zen::asset
