#pragma once
#include "Math/Math.h"
#include <ufbx.h>
#include <string>

namespace zen::asset
{
inline std::string FbxText(ufbx_string value)
{
    return value.length > 0 ? std::string(value.data, value.length) : std::string();
}

inline Vec3 FbxVector(ufbx_vec3 value)
{
    return Vec3(static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z));
}

inline Quat FbxQuaternion(ufbx_quat value)
{
    return Quat(static_cast<float>(value.w), static_cast<float>(value.x), static_cast<float>(value.y),
                static_cast<float>(value.z));
}

inline Mat4 FbxMatrix(const ufbx_matrix& source)
{
    Mat4 result(1);
    for (uint32_t column = 0; column < 4; ++column)
    {
        result[column] = Vec4(FbxVector(source.cols[column]), column == 3 ? 1 : 0);
    }
    return result;
}
} // namespace zen::asset
