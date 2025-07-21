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

    TypeId GetTypeId() const override
    {
        return typeid(Mesh);
    }

    const auto& GetSubMeshes() const
    {
        return m_subMeshes;
    }

    const auto& GetAABB() const
    {
        return m_aabb;
    }

    auto& GetAABB()
    {
        return m_aabb;
    }

    const auto& GetNodes() const
    {
        return m_nodes;
    }

    void AddNode(Node* node)
    {
        m_nodes.push_back(node);
    }

    void AddSubMesh(SubMesh* subMesh)
    {
        m_subMeshes.push_back(subMesh);
        m_numIndices += subMesh->GetIndexCount();
    }

    void SetAABB(const Vec3& min, const Vec3& max)
    {
        m_aabb.SetMin(min);
        m_aabb.SetMax(max);
    }

    const auto GetNumIndices() const
    {
        return m_numIndices;
    }

private:
    uint32_t m_numIndices{0};

    AABB m_aabb;

    std::vector<SubMesh*> m_subMeshes;

    std::vector<Node*> m_nodes;
};

inline bool operator==(const Mesh& lhs, const Mesh& rhs)
{
    // Compare the name
    if (lhs.GetName() != rhs.GetName())
        return false;

    // Compare the AABB (bounding box)
    if (lhs.GetAABB() != rhs.GetAABB())
        return false;

    // Compare the number of nodes
    if (lhs.GetNodes().size() != rhs.GetNodes().size())
        return false;

    // Compare the nodes themselves
    for (size_t i = 0; i < lhs.GetNodes().size(); ++i)
    {
        if (lhs.GetNodes()[i] != rhs.GetNodes()[i])
            return false;
    }

    // Compare the number of submeshes
    if (lhs.GetSubMeshes().size() != rhs.GetSubMeshes().size())
        return false;

    // Compare the submeshes themselves
    for (size_t i = 0; i < lhs.GetSubMeshes().size(); ++i)
    {
        auto& subMesh1 = *lhs.GetSubMeshes()[i];
        auto& subMesh2 = *lhs.GetSubMeshes()[i];
        bool equal     = (subMesh1 == subMesh2);
        if (!equal)
            return false;
    }

    return true;
}

inline bool operator!=(const Mesh& lhs, const Mesh& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg