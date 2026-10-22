#pragma once
#include "AssetLib/AnimationTarget.h"
#include "Templates/VectorView.h"

namespace zen::sg
{
class Scene;
bool ValidateAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values);
bool ApplyAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values);
// Applies without validating first; the same target and values must already have
// passed ValidateAnimationTarget against this scene.
bool ApplyValidatedAnimationTarget(Scene& scene, const asset::AnimationTarget& target, VectorView<const float> values);
} // namespace zen::sg
