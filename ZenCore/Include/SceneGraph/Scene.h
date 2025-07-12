#pragma once
#include "Common/UniquePtr.h"
#include "Node.h"
#include "Mesh.h"
#include "Sampler.h"
#include "Transform.h"
#include "Light.h"

namespace zen::sg
{
class Scene
{
public:
    struct DefaultTextures
    {
        Texture* baseColor;
        Texture* metallicRoughness;
        Texture* normal;
        Texture* emissive;
        Texture* occlusion;
    };

    Scene() = default;

    std::vector<Node*>& GetRenderableNodes()
    {
        return m_renderableNodes;
    }

    auto GetRenderableCount() const
    {
        return m_renderableNodes.size();
    }

    std::vector<std::pair<Node*, SubMesh*>> GetSortedSubMeshes(const Vec3& eyePos,
                                                               const Mat4& transform);

    void AddRenderableNode(Node* node)
    {
        m_renderableNodes.push_back(node);
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
    template <class T> void SetComponents(std::vector<UniquePtr<T>>&& components)
    {
        std::vector<UniquePtr<Component>> result(components.size());
        std::transform(components.begin(), components.end(), result.begin(),
                       [](UniquePtr<T>& component) -> UniquePtr<Component> {
                           return UniquePtr<Component>(std::move(component));
                       });
        m_components[typeid(T)] = std::move(result);
    }

    /**
	 * @return List of pointers to components casted to the given template type
	 */
    template <class T> std::vector<T*> GetComponents() const
    {
        std::vector<T*> result;
        if (HasComponent(typeid(T)))
        {
            auto& sceneComponents = m_components.at(typeid(T));
            result.resize(sceneComponents.size());
            std::transform(sceneComponents.begin(), sceneComponents.end(), result.begin(),
                           [](const UniquePtr<Component>& component) -> T* {
                               return dynamic_cast<T*>(component.Get());
                           });
        }
        return result;
    }

    bool HasComponent(const std::type_index& type_info) const
    {
        auto component = m_components.find(type_info);
        return (component != m_components.end() && !component->second.empty());
    }

    void SetNodes(std::vector<UniquePtr<Node>>&& nodes)
    {
        m_nodes = std::move(nodes);
    }

    void UpdateAABB();

    auto GetSize() const
    {
        return m_aabb.GetScale();
    }

    auto GetAABB() const
    {
        return m_aabb;
    }

    auto& GetAABB()
    {
        return m_aabb;
    }

    const auto& GetLocalAABB() const
    {
        return m_localAABB;
    }

    static void LoadDefaultTextures(uint32_t startIndex);

    static DefaultTextures GetDefaultTextures();

    void SetName(std::string name)
    {
        m_name = std::move(name);
    }

    const std::string GetName() const
    {
        return m_name;
    }

private:
    std::string m_name;

    // aabb without transformation
    AABB m_localAABB;

    AABB m_aabb;

    std::vector<UniquePtr<Node>> m_nodes;

    std::vector<Node*> m_renderableNodes;

    Node* m_rootNode{nullptr};

    HashMap<TypeId, std::vector<UniquePtr<Component>>> m_components;

    static DefaultTextures sDefaultTextures;
};
} // namespace zen::sg