#include <map>
#include "SceneGraph/Scene.h"
#include "SceneGraph/Camera.h"

namespace zen::sg
{
void Scene::Clear()
{
    m_renderableNodes.clear();

    m_components.clear();

    m_nodes.clear();

    m_assetData       = SceneAssetData();

    m_aabb            = AABB();

    m_localAABB       = AABB();

    m_pRootNode       = nullptr;

    m_defaultTextures = {};
}

void Scene::UpdateAABB()
{
    m_localAABB = AABB();

    m_aabb      = AABB();

    for (Node* pNode : m_renderableNodes)
    {
        if (!pNode->IsVisible())
        {
            continue;
        }

        const AABB& meshAABB = pNode->GetComponent<Mesh>()->GetAABB();

        m_localAABB.SetMin(meshAABB.GetMin());

        m_localAABB.SetMax(meshAABB.GetMax());

        AABB worldAABB   = meshAABB;

        const Mat4 world = pNode->deformationInWorldSpace   ? pNode->GetData().modelMatrix
                         : pNode->HasComponent<Transform>() ? pNode->GetComponent<Transform>()->GetWorldMatrix()
                                                            : pNode->GetData().modelMatrix;

        worldAABB.Transform(world);

        m_aabb.SetMin(worldAABB.GetMin());

        m_aabb.SetMax(worldAABB.GetMax());
    }

    if (glm::any(glm::greaterThan(m_aabb.GetMin(), m_aabb.GetMax())))
    {
        m_aabb      = AABB(Vec3(0), Vec3(0));

        m_localAABB = m_aabb;
    }
}

bool Scene::SetMaterialVariant(int32_t variant)
{
    bool valid = variant >= -1 && (variant == -1 || static_cast<size_t>(variant) < m_assetData.materialVariants.size());

    const HeapVector<Material*> materials = GetComponents<Material>();

    const HeapVector<SubMesh*> primitives = GetComponents<SubMesh>();

    HeapVector<Material*> selected(primitives.size(), nullptr);

    for (size_t index = 0; valid && index < primitives.size(); ++index)
    {
        SubMesh* primitive = primitives[index];

        selected[index]    = primitive->GetDefaultMaterial();

        for (const MaterialVariantPrimitiveAsset& mapping : m_assetData.variantPrimitives)
        {
            if (variant >= 0 && mapping.mesh == primitive->assetMesh && mapping.primitive == primitive->assetPrimitive
                && static_cast<size_t>(variant) < mapping.materials.size())
            {
                const int32_t material = mapping.materials[variant];

                if (material >= 0)
                {
                    valid &= static_cast<size_t>(material) < materials.size();

                    if (valid)
                    {
                        selected[index] = materials[material];
                    }
                }
            }
        }

        valid &= selected[index] != nullptr;
    }

    if (valid)
    {
        for (size_t index = 0; index < primitives.size(); ++index)
        {
            primitives[index]->SetMaterial(selected[index]->index, selected[index]);
        }
    }

    return valid;
}

HeapVector<std::pair<Node*, SubMesh*>> Scene::GetSortedSubMeshes(const Vec3& eyePos, const Mat4& transform)
{
    HeapVector<std::pair<Node*, SubMesh*>> result;

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

    for (std::multimap<float, std::pair<Node*, SubMesh*>>::const_iterator nodeIt = tmp.begin(); nodeIt != tmp.end(); ++nodeIt)
    {
        result.push_back(nodeIt->second);
    }

    return result;
}

static Texture* CreateDefaultTexture(const char* name, uint32_t index, std::initializer_list<uint8_t> pixels)
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
    m_defaultTextures.pBaseColor = CreateDefaultTexture("DefaultBaseColor", startIndex, {255, 255, 255, 255});

    m_defaultTextures.pMetallicRoughness =
        CreateDefaultTexture("DefaultMetallicRoughness", startIndex + 1, {255, 255, 255, 255});

    m_defaultTextures.pNormal             = CreateDefaultTexture("DefaultNormal", startIndex + 2, {127, 127, 255, 255});

    m_defaultTextures.pNormal->flatNormal = true;

    m_defaultTextures.pEmissive           = CreateDefaultTexture("DefaultEmissive", startIndex + 3, {255, 255, 255, 255});

    m_defaultTextures.pOcclusion          = CreateDefaultTexture("DefaultOcclusion", startIndex + 4, {255, 0, 0, 255});
}

Scene::DefaultTextures Scene::GetDefaultTextures() const
{
    return m_defaultTextures;
}
} // namespace zen::sg
