#pragma once
#include "Templates/HeapVector.h"
#include "Utils/UniquePtr.h"
#include "Node.h"
#include "Mesh.h"
#include "Sampler.h"
#include "Transform.h"
#include "Light.h"
#include "SceneAsset.h"

namespace zen::sg
{
class Scene
{
public:
    struct DefaultTextures
    {
        Texture* pBaseColor;
        Texture* pMetallicRoughness;
        Texture* pNormal;
        Texture* pEmissive;
        Texture* pOcclusion;
    };

    Scene() = default;

    void Clear();

    const HeapVector<UniquePtr<Node>>& GetNodes() const
    {
        return m_nodes;
    }

    SceneAssetData& GetAssetData()
    {
        return m_assetData;
    }

    const SceneAssetData& GetAssetData() const
    {
        return m_assetData;
    }

    HeapVector<Node*>& GetRenderableNodes()
    {
        return m_renderableNodes;
    }

    size_t GetRenderableCount() const
    {
        return m_renderableNodes.size();
    }

    HeapVector<std::pair<Node*, SubMesh*>> GetSortedSubMeshes(const Vec3& eyePos, const Mat4& transform);

    void AddRenderableNode(Node* pNode)
    {
        m_renderableNodes.push_back(pNode);
    }

    void AddComponent(UniquePtr<Component>&& component)
    {
        if (component)
        {
            m_components[component->GetTypeId()].emplace_back(std::move(component));
        }
    }

    /**
	 * @brief Set list of components casted from the given template type
	 */
    template <class T> void SetComponents(HeapVector<UniquePtr<T>>&& components)
    {
        HeapVector<UniquePtr<Component>> result(components.size());
        for (size_t i = 0; i < components.size(); ++i)
        {
            result[i] = UniquePtr<Component>(std::move(components[i]));
        }
        m_components[typeid(T)] = std::move(result);
    }

    /**
	 * @return List of pointers to components casted to the given template type
	 */
    template <class T> HeapVector<T*> GetComponents() const
    {
        HeapVector<T*> result;
        if (HasComponent(typeid(T)))
        {
            const HeapVector<UniquePtr<Component>>& sceneComponents = m_components.at(typeid(T));
            result.resize(sceneComponents.size());
            for (size_t i = 0; i < sceneComponents.size(); ++i)
            {
                result[i] = dynamic_cast<T*>(sceneComponents[i].Get());
            }
        }
        return result;
    }

    bool HasComponent(const std::type_index& type_info) const
    {
        const HashMap<TypeId, HeapVector<UniquePtr<Component>>>::const_iterator component = m_components.find(type_info);
        return (component != m_components.end() && !component->second.empty());
    }

    void SetNodes(HeapVector<UniquePtr<Node>>&& nodes)
    {
        m_nodes = std::move(nodes);
    }

    void UpdateAABB();

    // -1 restores the authored default material on every primitive.
    bool SetMaterialVariant(int32_t variant);

    float GetSize() const
    {
        return m_aabb.GetScale();
    }

    const AABB& GetAABB() const
    {
        return m_aabb;
    }

    AABB& GetAABB()
    {
        return m_aabb;
    }

    const AABB& GetLocalAABB() const
    {
        return m_localAABB;
    }

    void LoadDefaultTextures(uint32_t startIndex);

    DefaultTextures GetDefaultTextures() const;

    void SetName(std::string name)
    {
        m_name = std::move(name);
    }

    const std::string& GetName() const
    {
        return m_name;
    }

private:
    SceneAssetData m_assetData;

    std::string m_name;

    // aabb without transformation
    AABB m_localAABB;

    AABB m_aabb;

    HeapVector<UniquePtr<Node>> m_nodes;

    HeapVector<Node*> m_renderableNodes;

    Node* m_pRootNode{nullptr};

    HashMap<TypeId, HeapVector<UniquePtr<Component>>> m_components;

    DefaultTextures m_defaultTextures{};
};
} // namespace zen::sg
