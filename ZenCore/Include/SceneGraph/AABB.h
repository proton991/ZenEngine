#pragma once

#include "Common/Math.h"

namespace zen::sg
{
class AABB
{
public:
    AABB(const Vec3& min, const Vec3& max) : m_min(min), m_max(max) {}

    AABB()
    {
        m_min = std::numeric_limits<glm::vec3>::max();
        m_max = std::numeric_limits<glm::vec3>::min();
    }

    ~AABB() = default;

    void SetMin(const Vec3& min)
    {
        m_min = glm::min(min, m_min);
    }

    void SetMax(const Vec3& max)
    {
        m_max = glm::max(max, m_max);
    }

    auto GetMin() const
    {
        return m_min;
    }

    auto GetMax() const
    {
        return m_max;
    }

    auto GetCenter() const
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
        auto extent = GetExtent3D();
        return std::max(extent.x, std::max(extent.y, extent.z));
    }

    void Transform(const Mat4& transform)
    {
        m_min = Vec3(transform * Vec4(m_min, 1.0f));
        m_max = Vec3(transform * Vec4(m_max, 1.0f));
    }

private:
    void Update(const Vec3& point)
    {
        m_min = glm::min(m_min, point);
        m_max = glm::min(m_max, point);
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