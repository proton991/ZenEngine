#pragma once
#include "AssetLib/SceneAsset.h"

namespace zen::asset
{
bool CompileGltfAnimationTarget(const SceneAsset& asset, const std::string& pointer, AnimationTarget& target);
} // namespace zen::asset
