#pragma once
#include "AssetLib/SceneAsset.h"
#include "AssetLib/SceneLoader.h"

namespace zen::asset
{
enum class ScenePlacement
{
    PreserveAuthored,
    CenterAndNormalize
};

// Apply placement to an unpublished runtime scene before renderer construction.
// Bind geometry remains in canonical coordinates; posing consumes this placement.
void PrepareScenePlacement(sg::Scene& scene, ScenePlacement placement);

// Creates independent mutable pose/material/geometry state. Decoded image storage
// remains shared and immutable. Failure never exposes a partially built scene.
SceneLoadResult BuildSceneRuntime(SharedPtr<const SceneAsset, MultiThreadCounter> asset, SceneImportControl* control = nullptr);
} // namespace zen::asset
