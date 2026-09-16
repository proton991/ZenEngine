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
    m_worldMatrix = GetLocalMatrix();
    Node* parent  = m_node.GetParent();
    while (parent)
    {
        if (parent->HasComponent<Transform>())
        {
            Transform* pTransform = parent->GetComponent<Transform>();
            m_worldMatrix         = pTransform->GetWorldMatrix() * m_worldMatrix;
            // The parent's world matrix already includes every ancestor.
            break;
        }
        // Get parent node
        parent = parent->GetParent();
    }
}
} // namespace zen::sg
