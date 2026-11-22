#pragma once
#include "SceneGraph/SceneAnimationTarget.h"
#include <string>

namespace zen::sg
{
// Source-path convenience for the glTF regression fixtures. Production playback
// accepts typed targets only.
bool ValidateAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values);
bool ApplyAnimationPointer(Scene& scene, const std::string& pointer, VectorView<const float> values);
} // namespace zen::sg
