#pragma once

#include "Math/Math.h"

namespace zen::sg
{
class AABB
{
public:
    AABB(const Vec3& min, const Vec3& max) : m_min(min), m_max(max) {}

    AABB() : m_min(std::numeric_limits<float>::max()), m_max(std::numeric_limits<float>::lowest())
    {}

    ~AABB() = default;

    void SetMin(const Vec3& min)
    {
        m_min = glm::min(min, m_min);
    }

    void SetMax(const Vec3& max)
    {
        m_max = glm::max(max, m_max);
    }

    Vec3 GetMin() const
    {
        return m_min;
    }

    Vec3 GetMax() const
    {
        return m_max;
    }

    Vec3 GetCenter() const
    {
        return (m_min + m_max) * 0.5f;
    }

    float GetScale() const
    {
        return glm::distance(m_min, m_max);
    }

    Vec3 GetExtent3D() const
    {
        return glm::abs(Vec3(m_max - m_min));
    }

    float GetMaxExtent() const
    {
        const Vec3 extent = GetExtent3D();
        return std::max(extent.x, std::max(extent.y, extent.z));
    }

    void Transform(const Mat4& transform)
    {
        const Vec3 min = m_min;
        const Vec3 max = m_max;
        *this          = AABB();
        for (uint32_t corner = 0; corner < 8; ++corner)
        {
            const Vec3 point((corner & 1) ? max.x : min.x, (corner & 2) ? max.y : min.y,
                             (corner & 4) ? max.z : min.z);
            Update(Vec3(transform * Vec4(point, 1.0f)));
        }
    }

private:
    void Update(const Vec3& point)
    {
        m_min = glm::min(m_min, point);
        m_max = glm::max(m_max, point);
    }
    Vec3 m_min;
    Vec3 m_max;
};
inline bool operator==(const AABB& lhs, const AABB& rhs)
{
    // Compare the min and max points of the AABBs
    return lhs.GetMin() == rhs.GetMin() && lhs.GetMax() == rhs.GetMax();
}

inline bool operator!=(const AABB& lhs, const AABB& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg
