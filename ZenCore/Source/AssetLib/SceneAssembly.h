#pragma once
#include "AssetLib/SceneAssetIO.h"

namespace zen::asset
{
SceneImportResult ReadSceneAssemblyAsset(const std::string&                         path,
                                         const SceneAssetReadLimits&                limits,
                                         SceneImportControl*                        control,
                                         SharedPtr<SceneAsset, MultiThreadCounter>* runtimeOwner = nullptr);
}
