#pragma once
#include "Component.h"
#include "SubMesh.h"

namespace zen::sg
{
class Node;
class Mesh : public Component
{
public:
    Mesh(std::string name) : Component(std::move(name)) {}

    TypeId GetTypeId() const override { return typeid(Mesh); }

    const auto& GetSubMeshes() const { return m_subMeshes; }

    const auto& GetAABB() const { return m_aabb; }

    const auto& GetNodes() const { return m_nodes; }

    void AddNode(Node* node) { m_nodes.push_back(node); }

    void AddSubMesh(SubMesh* subMesh) { m_subMeshes.push_back(subMesh); }

    void SetAABB(const Vec3& min, const Vec3& max)
    {
        m_aabb.SetMin(min);
        m_aabb.SetMax(max);
    }

private:
    AABB m_aabb;

    std::vector<SubMesh*> m_subMeshes;

    std::vector<Node*> m_nodes;
};
} // namespace zen::sg