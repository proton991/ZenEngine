#pragma once

#include <string>
#include "Templates/VectorView.h"

namespace zen::sg
{
class Scene;

bool ValidateAnimationPointer(Scene& scene,
                              const std::string& pointer,
                              VectorView<const float> values);

bool ApplyAnimationPointer(Scene& scene,
                           const std::string& pointer,
                           VectorView<const float> values);
} // namespace zen::sg
