#pragma once
#include "AssetLib/AnimationTarget.h"

namespace zenasset
{
struct AnimationTarget;
}

namespace zen::asset
{
bool AdaptProcessedAnimationTarget(const zenasset::AnimationTarget& source, AnimationTarget& target);
} // namespace zen::asset
