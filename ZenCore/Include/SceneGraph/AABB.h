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

    void Update(const Vec3& point)
    {
        m_min = glm::min(m_min, point);
        m_max = glm::min(m_max, point);
    }

    void SetMin(const Vec3& min) { m_min = glm::min(min, m_min); }

    void SetMax(const Vec3& max) { m_max = glm::max(max, m_max); }

    auto GetMin() const { return m_min; }

    auto GetMax() const { return m_max; }

    auto GetCenter() const { return (m_min + m_max) * 0.5f; }

    float GetScale() const { return glm::distance(m_min, m_max); }

    void Transform(const Mat4& transform)
    {
        m_min = m_max = Vec4(m_min, 1.0f) * transform;

        // Update bounding box for the remaining 7 corners of the box
        Update(Vec4(m_min.x, m_min.y, m_max.z, 1.0f) * transform);
        Update(Vec4(m_min.x, m_max.y, m_min.z, 1.0f) * transform);
        Update(Vec4(m_min.x, m_max.y, m_max.z, 1.0f) * transform);
        Update(Vec4(m_max.x, m_min.y, m_min.z, 1.0f) * transform);
        Update(Vec4(m_max.x, m_min.y, m_max.z, 1.0f) * transform);
        Update(Vec4(m_max.x, m_max.y, m_min.z, 1.0f) * transform);
        Update(Vec4(m_max, 1.0f) * transform);
    }

private:
    Vec3 m_min;
    Vec3 m_max;
};
} // namespace zen::sg