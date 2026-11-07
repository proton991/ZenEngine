#pragma once
#include "AssetLib/SceneRuntimeBuilder.h"

namespace zen::asset
{
// BuildSceneRuntime without re-validating: for assets that ImportScene or the
// internal reader returned, which have already passed ValidateSceneAsset.
SceneLoadResult BuildValidatedSceneRuntime(const SharedPtr<const SceneAsset, MultiThreadCounter>& asset,
                                           SceneImportControl*                                    control);
// Consumes a fresh, exclusively owned reader result. Static geometry moves into
// the runtime; callers needing the canonical asset use the immutable API above.
SceneLoadResult BuildConsumedSceneRuntime(SharedPtr<SceneAsset, MultiThreadCounter> asset, SceneImportControl* control);
} // namespace zen::asset
