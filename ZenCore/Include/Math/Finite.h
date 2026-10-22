#pragma once
#include "Math/Math.h"
#include <cmath>
#include <type_traits>

namespace zen
{
// Componentwise finiteness shared by importers, validation and animation.
template <class T>
    requires std::is_floating_point_v<T>
bool IsFinite(T value)
{
    return std::isfinite(value);
}

template <glm::length_t N, class T, glm::qualifier Q> bool IsFinite(const glm::vec<N, T, Q>& value)
{
    bool valid = true;
    for (glm::length_t component = 0; component < N; ++component)
    {
        valid &= std::isfinite(value[component]);
    }
    return valid;
}

template <glm::length_t C, glm::length_t R, class T, glm::qualifier Q> bool IsFinite(const glm::mat<C, R, T, Q>& value)
{
    bool valid = true;
    for (glm::length_t column = 0; column < C; ++column)
    {
        valid &= IsFinite(value[column]);
    }
    return valid;
}

template <class T, glm::qualifier Q> bool IsFinite(const glm::qua<T, Q>& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}
} // namespace zen
