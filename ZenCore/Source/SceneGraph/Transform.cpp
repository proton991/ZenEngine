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
    m_worldMatrix = m_prefixMatrix * GetLocalMatrix();

    Node* parent  = m_node.GetParent();

    while (parent)
    {
        if (parent->HasComponent<Transform>())
        {
            Transform* pTransform = parent->GetComponent<Transform>();

            m_worldMatrix         = pTransform->m_prefixMatrix * pTransform->GetLocalMatrix() * m_worldMatrix;
        }

        // Get parent node
        parent = parent->GetParent();
    }
}
} // namespace zen::sg
