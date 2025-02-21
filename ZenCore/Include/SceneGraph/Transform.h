#pragma once
#include "Component.h"
#include "Common/Math.h"

namespace zen::sg
{
class Node;
class Transform : public Component
{
public:
    Transform(const Node& node) : m_node(node) {}

    Mat4 GetWorldMatrix();

    void SetTranslation(const Vec3& translation)
    {
        m_translation = translation;
        InvalidateWorldMatrix();
    }

    void SetScale(const Vec3& scale)
    {
        m_scale = scale;
        InvalidateWorldMatrix();
    }

    void SetRotation(const Quat& rotation)
    {
        m_rotation = rotation;
        InvalidateWorldMatrix();
    }

    void SetLocalMatrix(const Mat4& mat)
    {
        m_localMatrix = mat;
    }

    void InvalidateWorldMatrix()
    {
        m_updateWorldMatrix = true;
    }

    TypeId GetTypeId() const override
    {
        return typeid(Transform);
    };

private:
    Mat4 GetLocalMatrix()
    {
        if (!m_validLocalMatrix)
        {
            m_localMatrix = glm::translate(Mat4(1.0f), m_translation) * glm::mat4_cast(m_rotation) *
                glm::scale(Mat4(1.0f), m_scale) * m_localMatrix;
            m_validLocalMatrix = true;
        }
        return m_localMatrix;
    }

    void UpdateWorldMatrix();
    // binding node
    const Node& m_node;
    // transformations
    Vec3 m_translation{0.0f};
    Vec3 m_scale{1.0f};
    Quat m_rotation{};
    // node local matrix
    Mat4 m_localMatrix{1.0f};
    // combined transform matrix
    Mat4 m_worldMatrix{1.0f};
    bool m_updateWorldMatrix{false};
    bool m_validLocalMatrix{false};
};
} // namespace zen::sg