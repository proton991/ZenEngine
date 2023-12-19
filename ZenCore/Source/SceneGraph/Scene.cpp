#include <map>
#include "SceneGraph/Scene.h"
#include "Common/Errors.h"
#include "Systems/Camera.h"

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

std::vector<std::pair<Node*, SubMesh*>> Scene::GetSortedSubMeshes(const Vec3& eyePos,
                                                                  const Mat4& transform)
{
    std::vector<std::pair<Node*, SubMesh*>> result;

    std::multimap<float, std::pair<Node*, SubMesh*>> tmp;
    for (auto& mesh : GetComponents<Mesh>())
    {
        for (auto& node : mesh->GetNodes())
        {
            auto worldMat = node->GetComponent<Transform>()->GetWorldMatrix();
            for (auto& subMesh : mesh->GetSubMeshes())
            {
                const sg::AABB& meshBounds = subMesh->GetAABB();

                sg::AABB worldBounds{meshBounds.GetMin(), meshBounds.GetMax()};
                worldBounds.Transform(transform * worldMat);

                float distance = glm::length(eyePos - worldBounds.GetCenter());
                tmp.emplace(distance, std::make_pair(node, subMesh));
            }
        }
    }
    for (auto nodeIt = tmp.begin(); nodeIt != tmp.end(); nodeIt++)
    {
        result.push_back(nodeIt->second);
    }
    return result;
}
} // namespace zen::sg