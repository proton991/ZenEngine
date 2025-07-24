#include <map>
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

namespace zen::sg
{
Scene::DefaultTextures Scene::sDefaultTextures = {};

void Scene::UpdateAABB()
{
    for (auto* node : m_renderableNodes)
    {
        auto& meshAABB = node->GetComponent<Mesh>()->GetAABB();
        m_localAABB    = meshAABB;
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

void Scene::LoadDefaultTextures(uint32_t startIndex)
{
    using namespace zen::asset;

    sDefaultTextures.baseColor            = new sg::Texture("DefaultBaseColor");
    sDefaultTextures.baseColor->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.baseColor->index     = startIndex;
    sDefaultTextures.baseColor->height    = 1;
    sDefaultTextures.baseColor->width     = 1;
    sDefaultTextures.baseColor->bytesData = {129, 133, 137, 255};

    sDefaultTextures.metallicRoughness         = new sg::Texture("DefaultMetallicRoughness");
    sDefaultTextures.metallicRoughness->format = Format::R8G8B8A8_UNORM;
    sDefaultTextures.metallicRoughness->index  = startIndex + 1;
    sDefaultTextures.metallicRoughness->height = 1;
    sDefaultTextures.metallicRoughness->width  = 1;
    // g 0 for metallic, b 255 for roughness
    sDefaultTextures.metallicRoughness->bytesData = {0, 0, 255, 255};

    sDefaultTextures.normal            = new sg::Texture("DefaultNormal");
    sDefaultTextures.normal->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.normal->index     = startIndex + 2;
    sDefaultTextures.normal->height    = 1;
    sDefaultTextures.normal->width     = 1;
    sDefaultTextures.normal->bytesData = {127, 127, 255, 255};

    sDefaultTextures.emissive            = new sg::Texture("DefaultEmissive");
    sDefaultTextures.emissive->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.emissive->index     = startIndex + 3;
    sDefaultTextures.emissive->height    = 1;
    sDefaultTextures.emissive->width     = 1;
    sDefaultTextures.emissive->bytesData = {0, 0, 0, 255};

    sDefaultTextures.occlusion            = new sg::Texture("DefaultOcclusion");
    sDefaultTextures.occlusion->format    = Format::R8G8B8A8_UNORM;
    sDefaultTextures.occlusion->index     = startIndex + 4;
    sDefaultTextures.occlusion->height    = 1;
    sDefaultTextures.occlusion->width     = 1;
    sDefaultTextures.occlusion->bytesData = {255, 0, 0, 255};
}

Scene::DefaultTextures Scene::GetDefaultTextures()
{
    return sDefaultTextures;
}
} // namespace zen::sg