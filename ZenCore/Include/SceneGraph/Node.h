#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include "Component.h"

namespace zen::sg
{
class Node
{
public:
    explicit Node(uint32_t index, std::string name) : m_index(index), m_name(std::move(name)) {}

    void AddComponent(Component* component)
    {
        auto it = m_components.find(component->GetTypeId());
        if (it != m_components.end()) { it->second = component; }
        else { m_components.insert({component->GetTypeId(), component}); }
    }
    template <class T> bool HasComponent() const { return m_components.count(typeid(T)) > 0; }

    template <class T> inline T* GetComponent()
    {
        return dynamic_cast<T*>(m_components.at(typeid(T)));
    }

    void AddChild(Node* child) { m_children.push_back(child); }

    void SetParent(Node* node) { m_parent = node; }

    auto GetParent() const { return m_parent; }

    auto& GetName() const { return m_name; }

    auto GetIndex() const { return m_index; }

    uint64_t GetHash() const { return *(reinterpret_cast<const uint64_t*>(this)); }

private:
    uint32_t m_index{0};

    std::string m_name;

    Node* m_parent{nullptr};

    std::vector<Node*> m_children;

    // One unique instance for each Component
    std::unordered_map<TypeId, Component*> m_components;
};
} // namespace zen::sg