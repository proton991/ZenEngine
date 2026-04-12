#include <map>
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

namespace zen::sg
{
Scene::DefaultTextures Scene::sDefaultTextures = {};

void Scene::UpdateAABB()
{
    for (auto* pNode : m_renderableNodes)
    {
        auto& meshAABB = pNode->GetComponent<Mesh>()->GetAABB();
        m_localAABB    = meshAABB;
        meshAABB.Transform(pNode->GetComponent<Transform>()->GetWorldMatrix());
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

void Scene::LoadDefaultTextures(uint32_t startIndex)
{
    using namespace zen::asset;

    sDefaultTextures.pBaseColor            = new sg::Texture("DefaultBaseColor");
    sDefaultTextures.pBaseColor->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.pBaseColor->index     = startIndex;
    sDefaultTextures.pBaseColor->height    = 1;
    sDefaultTextures.pBaseColor->width     = 1;
    sDefaultTextures.pBaseColor->bytesData = {129, 133, 137, 255};

    sDefaultTextures.pMetallicRoughness         = new sg::Texture("DefaultMetallicRoughness");
    sDefaultTextures.pMetallicRoughness->format = Format::R8G8B8A8_UNORM;
    sDefaultTextures.pMetallicRoughness->index  = startIndex + 1;
    sDefaultTextures.pMetallicRoughness->height = 1;
    sDefaultTextures.pMetallicRoughness->width  = 1;
    // g 0 for metallic, b 255 for roughness
    sDefaultTextures.pMetallicRoughness->bytesData = {0, 0, 255, 255};

    sDefaultTextures.pNormal            = new sg::Texture("DefaultNormal");
    sDefaultTextures.pNormal->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.pNormal->index     = startIndex + 2;
    sDefaultTextures.pNormal->height    = 1;
    sDefaultTextures.pNormal->width     = 1;
    sDefaultTextures.pNormal->bytesData = {127, 127, 255, 255};

    sDefaultTextures.pEmissive            = new sg::Texture("DefaultEmissive");
    sDefaultTextures.pEmissive->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.pEmissive->index     = startIndex + 3;
    sDefaultTextures.pEmissive->height    = 1;
    sDefaultTextures.pEmissive->width     = 1;
    sDefaultTextures.pEmissive->bytesData = {0, 0, 0, 255};

    sDefaultTextures.pOcclusion            = new sg::Texture("DefaultOcclusion");
    sDefaultTextures.pOcclusion->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.pOcclusion->index     = startIndex + 4;
    sDefaultTextures.pOcclusion->height    = 1;
    sDefaultTextures.pOcclusion->width     = 1;
    sDefaultTextures.pOcclusion->bytesData = {255, 0, 0, 255};
}

Scene::DefaultTextures Scene::GetDefaultTextures()
{
    return sDefaultTextures;
}
} // namespace zen::sg