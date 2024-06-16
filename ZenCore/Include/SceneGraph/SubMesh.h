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
        if (m_indexCount != 0) { m_hasIndices = true; }
    }

    TypeId GetTypeId() const override { return typeid(SubMesh); }

    void SetMaterial(uint32_t materialIndex, Material* material)
    {
        m_material      = material;
        m_materialIndex = materialIndex;
    }

    void SetFirstIndex(uint32_t firstIndex) { m_firstIndex = firstIndex; }
    void SetIndexCount(uint32_t indexCount) { m_indexCount = indexCount; }
    void SetVertexCount(uint32_t vertexCount) { m_vertexCount = vertexCount; }

    auto GetIndexCount() const { return m_indexCount; }
    auto GetFirstIndex() const { return m_firstIndex; }
    auto GetMaterial() const { return m_material; }

    void SetAABB(const Vec3& min, const Vec3& max)
    {
        m_aabb.SetMin(min);
        m_aabb.SetMax(max);
    }

    const auto& GetAABB() const { return m_aabb; }

    bool HasIndices() const { return m_hasIndices; }

private:
    uint32_t m_firstIndex{0};
    uint32_t m_indexCount{0};
    uint32_t m_vertexCount{0};

    AABB m_aabb;

    uint32_t m_materialIndex{0};
    Material* m_material{nullptr};

    bool m_hasIndices{false};
};

} // namespace zen::sg