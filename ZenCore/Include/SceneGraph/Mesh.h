#pragma once
#include "Component.h"
#include "SubMesh.h"
#include "Templates/HeapVector.h"
#include <algorithm>

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

    const HeapVector<SubMesh*>& GetSubMeshes() const
    {
        return m_subMeshes;
    }

    const AABB& GetAABB() const
    {
        return m_aabb;
    }

    AABB& GetAABB()
    {
        return m_aabb;
    }

    const HeapVector<Node*>& GetNodes() const
    {
        return m_nodes;
    }

    void AddNode(Node* pNode)
    {
        m_nodes.push_back(pNode);
    }

    void AddSubMesh(SubMesh* pSubMesh)
    {
        m_subMeshes.push_back(pSubMesh);
        m_numIndices += pSubMesh->GetIndexCount();
    }

    void SetAABB(const Vec3& min, const Vec3& max)
    {
        m_aabb.SetMin(min);
        m_aabb.SetMax(max);
    }

    uint32_t GetNumIndices() const
    {
        return m_numIndices;
    }

private:
    uint32_t m_numIndices{0};

    AABB m_aabb;

    HeapVector<SubMesh*> m_subMeshes;

    HeapVector<Node*> m_nodes;
};

inline bool operator==(const Mesh& lhs, const Mesh& rhs)
{
    return lhs.GetName() == rhs.GetName() && lhs.GetAABB() == rhs.GetAABB() &&
        std::equal(lhs.GetNodes().begin(), lhs.GetNodes().end(), rhs.GetNodes().begin(),
                   rhs.GetNodes().end()) &&
        std::equal(lhs.GetSubMeshes().begin(), lhs.GetSubMeshes().end(), rhs.GetSubMeshes().begin(),
                   rhs.GetSubMeshes().end(),
                   [](const SubMesh* left, const SubMesh* right) { return *left == *right; });
}

inline bool operator!=(const Mesh& lhs, const Mesh& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg