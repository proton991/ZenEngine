#pragma once
#include "Component.h"
#include "Math/Math.h"

namespace zen::sg
{
class Node;
class Transform : public Component
{
public:
    Transform(const Node& node) : m_node(node) {}

    Mat4 GetWorldMatrix();

    const Vec3& GetTranslation() const
    {
        return m_translation;
    }

    const Vec3& GetScale() const
    {
        return m_scale;
    }

    const Quat& GetRotation() const
    {
        return m_rotation;
    }

    const Mat4& GetBaseMatrix() const
    {
        return m_localMatrix;
    }

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
        InvalidateWorldMatrix();
    }

    void SetPrefixMatrix(const Mat4& matrix)
    {
        m_prefixMatrix = matrix;
    }

    const Mat4& GetPrefixMatrix() const
    {
        return m_prefixMatrix;
    }

    void InvalidateWorldMatrix()
    {
        m_validLocalMatrix = false;
    }

    TypeId GetTypeId() const override
    {
        return typeid(Transform);
    };

    // Authored local TRS/matrix composition, before normalization and parent transforms.
    Mat4 GetLocalMatrix()
    {
        if (!m_validLocalMatrix)
        {
            m_cachedLocalMatrix = glm::translate(Mat4(1.0f), m_translation) *
                glm::mat4_cast(m_rotation) * glm::scale(Mat4(1.0f), m_scale) * m_localMatrix;
            m_validLocalMatrix = true;
        }

        return m_cachedLocalMatrix;
    }

private:
    void UpdateWorldMatrix();
    // binding node
    const Node& m_node;
    // transformations
    Vec3 m_translation{0.0f};
    Vec3 m_scale{1.0f};
    Quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    // node local matrix
    Mat4 m_localMatrix{1.0f};

    // Scene normalization is independent of authored (and animated) local TRS.
    Mat4 m_prefixMatrix{1.0f};
    Mat4 m_cachedLocalMatrix{1.0f};
    // combined transform matrix
    Mat4 m_worldMatrix{1.0f};
    bool m_validLocalMatrix{false};
};
} // namespace zen::sg
