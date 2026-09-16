#include <map>
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

namespace zen::sg
{
Scene::DefaultTextures Scene::sDefaultTextures = {};

void Scene::UpdateAABB()
{
    m_localAABB = AABB();
    m_aabb      = AABB();
    for (Node* pNode : m_renderableNodes)
    {
        const AABB& meshAABB = pNode->GetComponent<Mesh>()->GetAABB();
        m_localAABB.SetMin(meshAABB.GetMin());
        m_localAABB.SetMax(meshAABB.GetMax());
        AABB worldAABB = meshAABB;
        worldAABB.Transform(pNode->GetComponent<Transform>()->GetWorldMatrix());
        m_aabb.SetMin(worldAABB.GetMin());
        m_aabb.SetMax(worldAABB.GetMax());
    }
}

std::vector<std::pair<Node*, SubMesh*>> Scene::GetSortedSubMeshes(const Vec3& eyePos,
                                                                  const Mat4& transform)
{
    std::vector<std::pair<Node*, SubMesh*>> result;

    std::multimap<float, std::pair<Node*, SubMesh*>> tmp;
    for (Mesh* mesh : GetComponents<Mesh>())
    {
        for (Node* node : mesh->GetNodes())
        {
            const Mat4 worldMat = node->GetComponent<Transform>()->GetWorldMatrix();
            for (SubMesh* subMesh : mesh->GetSubMeshes())
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

static Texture* CreateDefaultTexture(const char* name,
                                     uint32_t index,
                                     std::initializer_list<uint8_t> pixels)
{
    Texture* texture   = new Texture(name);
    texture->format    = asset::Format::R8G8B8A8_UNORM;
    texture->index     = index;
    texture->height    = 1;
    texture->width     = 1;
    texture->bytesData = pixels;
    return texture;
}

void Scene::LoadDefaultTextures(uint32_t startIndex)
{
    sDefaultTextures.pBaseColor =
        CreateDefaultTexture("DefaultBaseColor", startIndex, {129, 133, 137, 255});
    sDefaultTextures.pMetallicRoughness =
        CreateDefaultTexture("DefaultMetallicRoughness", startIndex + 1, {0, 0, 255, 255});
    sDefaultTextures.pNormal =
        CreateDefaultTexture("DefaultNormal", startIndex + 2, {127, 127, 255, 255});
    sDefaultTextures.pEmissive =
        CreateDefaultTexture("DefaultEmissive", startIndex + 3, {0, 0, 0, 255});
    sDefaultTextures.pOcclusion =
        CreateDefaultTexture("DefaultOcclusion", startIndex + 4, {255, 0, 0, 255});
}

Scene::DefaultTextures Scene::GetDefaultTextures()
{
    return sDefaultTextures;
}
} // namespace zen::sg
