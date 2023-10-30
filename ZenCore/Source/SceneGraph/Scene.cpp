#include "SceneGraph/Scene.h"
#include "Common/Errors.h"

namespace zen::sg
{
void Scene::UpdateAABB()
{
    for (auto* node : m_renderableNodes)
    {
        auto meshAABB = node->GetComponent<Mesh>()->GetAABB();
        meshAABB.Transform(node->GetComponent<Transform>()->GetWorldMatrix());
        m_aabb.SetMin(meshAABB.GetMin());
        m_aabb.SetMax(meshAABB.GetMax());
    }
}
} // namespace zen::sg