#include "SceneGraph/Transform.h"
#include "SceneGraph/Node.h"

namespace zen::sg
{
Mat4 Transform::GetWorldMatrix()
{
    UpdateWorldMatrix();
    return m_worldMatrix;
}

void Transform::UpdateWorldMatrix()
{
    if (!m_updateWorldMatrix)
    {
        return;
    }
    m_worldMatrix = GetLocalMatrix();
    auto parent   = m_node.GetParent();
    while (parent)
    {
        if (parent->HasComponent<Transform>())
        {
            auto* transform = parent->GetComponent<Transform>();
            m_worldMatrix   = transform->GetWorldMatrix() * m_worldMatrix;
        }
        // Get parent node
        parent = parent->GetParent();
    }
    m_updateWorldMatrix = false;
}
} // namespace zen::sg