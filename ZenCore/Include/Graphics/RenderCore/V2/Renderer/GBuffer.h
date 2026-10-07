#pragma once
#include "Math/Math.h"

namespace zen::rc
{
struct GBufferUniformData
{
    Mat4 inverseViewProjection;
    Vec4 worldOrigin;
};

inline GBufferUniformData BuildGBufferUniformData(const Mat4& projectionView)
{
    glm::dmat4 inverse = glm::inverse(glm::dmat4(projectionView));

    // Use the near-plane center as a nearby origin, including for orthographic cameras.
    // Subtract it before conversion to float so reconstruction avoids large cancelling terms.
    const Vec3 origin(glm::dvec3(inverse[3]) / inverse[3].w);

    for (uint32_t column = 0; column < 4; ++column)
    {
        inverse[column] -= glm::dvec4(glm::dvec3(origin) * inverse[column].w, 0.0);
    }

    return {Mat4(inverse), Vec4(origin, 0.0f)};
}
} // namespace zen::rc
