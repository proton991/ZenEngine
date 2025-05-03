#pragma once
#include "Common/Math.h"
#include <glm/gtc/matrix_access.hpp>
#include <array>

namespace zen
{
enum FrustumPlane
{
    Left,
    Right,
    Bottom,
    Top,
    Near,
    Far,
};

class Frustum
{
public:
    void ExtractPlanes(const Mat4& modelView, bool normalize = true)
    {
        // extract frustum planes from the modelView matrix
        // planes are in format: normal(xyz), offset(w)
        const glm::vec4 mRow[4]{glm::row(modelView, 0), glm::row(modelView, 1),
                                glm::row(modelView, 2), glm::row(modelView, 3)};
        m_planes[Left]   = mRow[3] + mRow[0];
        m_planes[Right]  = mRow[3] - mRow[0];
        m_planes[Bottom] = mRow[3] + mRow[1];
        m_planes[Top]    = mRow[3] - mRow[1];
        m_planes[Near]   = mRow[3] + mRow[2];
        m_planes[Far]    = mRow[3] - mRow[2];

        if (!normalize)
        {
            return;
        }

        // normalize them
        for (auto& p : m_planes)
        {
            p = glm::normalize(p);
        }
    }

    const std::array<Vec4, 6>& GetPlanes() const
    {
        return m_planes;
    }

protected:
    std::array<Vec4, 6> m_planes;
};

} // namespace zen