#pragma once
#include <vector>
#include <string>
#include "Common/HashMap.h"
#include "Common/Math.h"
#include "Component.h"

namespace zen::sg
{
struct NodeData
{
    Mat4 modelMatrix{1.0f};
    Mat4 normalMatrix{1.0f};
};

class Node
{
public:
    explicit Node(uint32_t index, std::string name) : m_index(index), m_name(std::move(name)) {}

    void AddComponent(Component* component)
    {
        auto it = m_components.find(component->GetTypeId());
        if (it != m_components.end())
        {
            it->second = component;
        }
        else
        {
            m_components.insert({component->GetTypeId(), component});
        }
    }
    template <class T> bool HasComponent() const
    {
        return m_components.count(typeid(T)) > 0;
    }

    template <class T> inline T* GetComponent()
    {
        return dynamic_cast<T*>(m_components.at(typeid(T)));
    }

    void AddChild(Node* child)
    {
        m_children.push_back(child);
    }

    void SetParent(Node* node)
    {
        m_parent = node;
    }

    auto GetParent() const
    {
        return m_parent;
    }

    auto& GetName() const
    {
        return m_name;
    }

    auto GetIndex() const
    {
        return m_index;
    }

    auto GetRenderableIndex() const
    {
        return m_renderableIndex;
    }

    uint64_t GetHash() const
    {
        return *(reinterpret_cast<const uint64_t*>(this));
    }

    void SetData(uint32_t renderableIndex, const Mat4& modelMatrix)
    {
        m_data.modelMatrix = modelMatrix;
        // pre-calculate normal transform matrix
        m_data.normalMatrix = glm::transpose(glm::inverse(m_data.modelMatrix));
        m_renderableIndex   = renderableIndex;
    }

    void SetData(const NodeData& data)
    {
        m_data = data;
    }

    const NodeData& GetData() const
    {
        return m_data;
    }

private:
    uint32_t m_index{0};

    uint32_t m_renderableIndex{0};

    std::string m_name;

    Node* m_parent{nullptr};

    std::vector<Node*> m_children;

    // One unique instance for each Component
    HashMap<TypeId, Component*> m_components;

    // Used for renderer's uniform buffer
    NodeData m_data{};
};
} // namespace zen::sg