#pragma once
#include "AABB.h"
#include "Material.h"

namespace zen::sg
{
class SubMesh : public Component
{
public:
    SubMesh(std::string name) : Component(std::move(name)) {}

    SubMesh(std::string name, uint32_t firstIndex, uint32_t indexCount, uint32_t vertexCount) :
        Component(std::move(name)),
        m_firstIndex(firstIndex),
        m_indexCount(indexCount),
        m_vertexCount(vertexCount)
    {
        if (m_indexCount != 0)
        {
            m_hasIndices = true;
        }
    }

    TypeId GetTypeId() const override
    {
        return typeid(SubMesh);
    }

    void SetMaterial(uint32_t materialIndex, Material* pMaterial)
    {
        m_pMaterial     = pMaterial;
        m_materialIndex = materialIndex;
    }

    void SetFirstIndex(uint32_t firstIndex)
    {
        m_firstIndex = firstIndex;
    }
    void SetIndexCount(uint32_t indexCount)
    {
        m_indexCount = indexCount;
        m_hasIndices = indexCount != 0;
    }
    void SetVertexCount(uint32_t vertexCount)
    {
        m_vertexCount = vertexCount;
    }

    uint32_t GetVertexCount() const
    {
        return m_vertexCount;
    }

    uint32_t GetIndexCount() const
    {
        return m_indexCount;
    }
    uint32_t GetFirstIndex() const
    {
        return m_firstIndex;
    }
    Material* GetMaterial() const
    {
        return m_pMaterial;
    }

    void SetAABB(const Vec3& min, const Vec3& max)
    {
        m_aabb.SetMin(min);
        m_aabb.SetMax(max);
    }

    const AABB& GetAABB() const
    {
        return m_aabb;
    }

    AABB& GetAABB()
    {
        return m_aabb;
    }

    bool HasIndices() const
    {
        return m_hasIndices;
    }

    uint32_t GetMaterialIndex() const
    {
        return m_materialIndex;
    }

private:
    uint32_t m_firstIndex{0};
    uint32_t m_indexCount{0};
    uint32_t m_vertexCount{0};

    AABB m_aabb;

    uint32_t m_materialIndex{0};
    Material* m_pMaterial{nullptr};

    bool m_hasIndices{false};
};

inline bool operator==(const SubMesh& lhs, const SubMesh& rhs)
{
    return lhs.GetAABB() == rhs.GetAABB() && lhs.GetFirstIndex() == rhs.GetFirstIndex() &&
        lhs.GetIndexCount() == rhs.GetIndexCount() &&
        lhs.GetVertexCount() == rhs.GetVertexCount() && lhs.GetMaterial() == rhs.GetMaterial() &&
        lhs.GetMaterialIndex() == rhs.GetMaterialIndex() && lhs.HasIndices() == rhs.HasIndices();
}

inline bool operator!=(const SubMesh& lhs, const SubMesh& rhs)
{
    return !(lhs == rhs);
}
} // namespace zen::sg