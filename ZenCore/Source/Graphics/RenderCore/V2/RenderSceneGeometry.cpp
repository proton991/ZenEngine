#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/DynamicVoxelGIPlanning.h"
#include <cmath>
#include <cstring>

namespace zen::rc
{
uint32_t RenderScene::GetInstanceMask(uint32_t instance) const
{
    return instance < m_instanceClasses.size() && m_instanceEnabled[instance] != 0 ?
        m_instanceClasses[instance] :
        0;
}

bool RenderScene::SetInstanceClass(uint32_t instance, uint32_t objectClass)
{
    const bool valid = instance < m_instanceClasses.size() &&
        (objectClass == GI_STATIC || objectClass == GI_DYNAMIC);
    if (valid && m_instanceClasses[instance] != objectClass)
    {
        m_dirtyClasses |= m_instanceClasses[instance] | objectClass;
        m_instanceClasses[instance] = objectClass;
        InvalidateGIHistory();
    }
    return valid;
}

bool RenderScene::SetInstanceEnabled(uint32_t instance, bool enabled)
{
    const bool valid = instance < m_instanceClasses.size();
    if (valid && (m_instanceEnabled[instance] != 0) != enabled)
    {
        m_dirtyClasses |= m_instanceClasses[instance];
        m_instanceEnabled[instance] = enabled ? 1 : 0;
        InvalidateGIHistory();
    }
    return valid;
}

bool RenderScene::SetInstanceTransform(uint32_t instance, const Mat4& worldTransform)
{
    const float determinant = glm::determinant(worldTransform);
    bool valid = instance < m_nodesData.size() && instance < m_instanceClasses.size() &&
        worldTransform[0][3] == 0 && worldTransform[1][3] == 0 && worldTransform[2][3] == 0 &&
        worldTransform[3][3] == 1 && std::isfinite(determinant) && determinant != 0;
    for (uint32_t column = 0; column < 4; ++column)
    {
        for (uint32_t row = 0; row < 4; ++row)
        {
            valid = valid && std::isfinite(worldTransform[column][row]);
        }
    }
    Mat4 normalMatrix(1);
    if (valid)
    {
        normalMatrix = glm::transpose(glm::inverse(worldTransform));
        for (uint32_t column = 0; column < 4; ++column)
        {
            for (uint32_t row = 0; row < 4; ++row)
            {
                valid = valid && std::isfinite(normalMatrix[column][row]);
            }
        }
    }
    if (valid)
    {
        m_nodesData[instance] = {worldTransform, normalMatrix};
        m_dirtyClasses |= m_instanceClasses[instance];
    }
    return valid;
}

bool RenderScene::UpdateVertices(uint32_t first, VectorView<const asset::Vertex> vertices)
{
    bool valid = first <= m_vertices.size() && vertices.size() <= m_vertices.size() - first &&
        (vertices.empty() || vertices.data() != nullptr);
    for (size_t index = 0; valid && index < vertices.size(); ++index)
    {
        const asset::Vertex& vertex = vertices[index];
        for (uint32_t i = 0; i < 4; ++i)
        {
            valid = valid && std::isfinite(vertex.pos[i]) && std::isfinite(vertex.normal[i]) &&
                std::isfinite(vertex.tangent[i]) && std::isfinite(vertex.color[i]) &&
                std::isfinite(vertex.joint0[i]) && std::isfinite(vertex.weight0[i]);
        }
        valid = valid && std::isfinite(vertex.uv0.x) && std::isfinite(vertex.uv0.y) &&
            std::isfinite(vertex.uv1.x) && std::isfinite(vertex.uv1.y);
    }
    if (valid && !vertices.empty())
    {
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            m_vertices[first + i] = vertices[i];
        }
        m_verticesDirty = true;
        // Shared mesh vertices can affect instances in both classes.
        for (const sg::Node* node : m_pScene->GetRenderableNodes())
        {
            bool affected = false;
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                for (uint32_t i = 0; i < mesh->GetIndexCount(); ++i)
                {
                    const uint32_t vertex = m_indices[mesh->GetFirstIndex() + i];
                    affected = affected || (vertex >= first && vertex - first < vertices.size());
                }
            }
            if (affected)
            {
                m_dirtyClasses |= m_instanceClasses[node->GetRenderableIndex()];
            }
        }
    }
    return valid;
}

bool RenderScene::UpdateMaterial(uint32_t material, const sg::MaterialData& data)
{
    bool valid = material < m_materialsData.size() && std::isfinite(data.metallicFactor) &&
        data.metallicFactor >= 0 && data.metallicFactor <= 1 &&
        std::isfinite(data.roughnessFactor) && data.roughnessFactor >= 0 &&
        data.roughnessFactor <= 1 && data.surfaceProperties.x >= 0 &&
        data.surfaceProperties.x <= 1 && data.surfaceProperties.y >= 0 &&
        data.surfaceProperties.y <= 2 &&
        data.surfaceProperties.y == std::floor(data.surfaceProperties.y) &&
        data.surfaceProperties.z >= 0;
    for (const Vec4& value : {data.baseColorFactor, data.emissiveFactor, data.surfaceProperties})
    {
        for (uint32_t component = 0; component < 4; ++component)
        {
            valid = valid && std::isfinite(value[component]) && value[component] >= 0;
        }
    }
    for (int index : {data.bcTexIndex, data.mrTexIndex, data.normalTexIndex, data.occlusionTexIndex,
                      data.emissiveTexIndex})
    {
        valid = valid &&
            (index == -1 ||
             (index >= 0 && static_cast<size_t>(index) < m_sceneTextures.size() &&
              m_sceneTextures[index] != nullptr));
    }
    for (int uv :
         {data.bcTexSet, data.mrTexSet, data.normalTexSet, data.aoTexSet, data.emissiveTexSet})
    {
        valid = valid && uv >= -1 && uv <= 1;
    }
    if (valid && std::memcmp(&m_materialsData[material], &data, sizeof(data)) != 0)
    {
        const sg::MaterialData& previous = m_materialsData[material];
        const bool coverageChanged       = previous.baseColorFactor.a != data.baseColorFactor.a ||
            previous.bcTexIndex != data.bcTexIndex || previous.bcTexSet != data.bcTexSet ||
            glm::vec2(previous.surfaceProperties) != glm::vec2(data.surfaceProperties);
        for (const sg::Node* node : m_pScene->GetRenderableNodes())
        {
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                if (mesh->GetMaterial()->index == material)
                {
                    const uint32_t objectClass = m_instanceClasses[node->GetRenderableIndex()];
                    m_dirtySurfaceClasses |= objectClass;
                    if (coverageChanged)
                    {
                        m_dirtyClasses |= objectClass;
                    }
                }
            }
        }
        m_materialsData[material] = data;
        m_materialsDirty          = true;
    }
    return valid;
}

bool RenderScene::SetVoxelBounds(const sg::AABB& bounds)
{
    const Vec3 center = bounds.GetCenter();
    const float side  = bounds.GetMaxExtent() * (64.0f / 62.0f);
    bool valid        = side > 0 && std::isfinite(side);
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        valid = valid && bounds.GetMin()[axis] <= bounds.GetMax()[axis] &&
            std::isfinite(center[axis] - side) && std::isfinite(center[axis] + side);
    }
    if (valid && bounds != m_voxelBounds)
    {
        m_voxelBounds = bounds;
        m_dirtyClasses |= GI_ALL;
        InvalidateGIHistory();
    }
    return valid;
}

uint32_t RenderScene::GetVoxelCoverageMask(const sg::AABB& gridBounds) const
{
    uint32_t mask = 0;
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        const sg::AABB& bounds = m_classBounds[kind];
        const bool empty       = glm::any(glm::greaterThan(bounds.GetMin(), bounds.GetMax()));
        if (empty ||
            (glm::all(glm::greaterThanEqual(bounds.GetMin(), gridBounds.GetMin())) &&
             glm::all(glm::lessThanEqual(bounds.GetMax(), gridBounds.GetMax()))))
        {
            mask |= kind == 0 ? GI_STATIC : GI_DYNAMIC;
        }
    }
    return mask;
}

bool RenderScene::ComputeGeometryBounds(sg::AABB& bounds, sg::AABB (&classBounds)[2]) const
{
    bool valid     = true;
    bounds         = m_voxelBounds;
    classBounds[0] = classBounds[1] = sg::AABB();
    for (const sg::Node* node : m_pScene->GetRenderableNodes())
    {
        const uint32_t instance = node->GetRenderableIndex();
        if (GetInstanceMask(instance) != 0)
        {
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                for (uint32_t i = 0; i < mesh->GetIndexCount(); ++i)
                {
                    const uint32_t vertex = m_indices[mesh->GetFirstIndex() + i];
                    const Vec3 world(m_nodesData[instance].modelMatrix *
                                     Vec4(Vec3(m_vertices[vertex].pos), 1));
                    valid = valid && std::isfinite(world.x) && std::isfinite(world.y) &&
                        std::isfinite(world.z);
                    bounds.SetMin(world);
                    bounds.SetMax(world);
                    sg::AABB& objectBounds =
                        classBounds[GetInstanceMask(instance) == GI_STATIC ? 0 : 1];
                    objectBounds.SetMin(world);
                    objectBounds.SetMax(world);
                }
            }
        }
    }
    // The cubic grid adds one cell of padding. Reject nonfinite bounds before
    // publishing any buffers or advancing the generation.
    const Vec3 center = bounds.GetCenter();
    const float side  = bounds.GetMaxExtent() * (64.0f / 62.0f);
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        valid = valid && std::isfinite(center[axis] - side) && std::isfinite(center[axis] + side);
    }
    return valid;
}

bool RenderScene::CommitGeometryUpdates()
{
    bool valid = m_geometryReady && !m_pRenderDevice->AreSubmissionsBlocked();
    if (valid && (m_dirtyClasses != 0 || m_verticesDirty || m_materialsDirty))
    {
        const bool geometryChanged             = m_dirtyClasses != 0 || m_verticesDirty;
        const HeapVector<glm::uvec4> triangles = BuildTriangleRecords();
        const RHIGPUInfo& gpu                  = m_pRenderDevice->GetGPUInfo();
        uint64_t nodeBytes = 0, triangleBytes = 0, vertexBytes = 0, materialBytes = 0;
        sg::AABB bounds;
        sg::AABB classBounds[2];
        valid = ComputeGeometryBounds(bounds, classBounds) &&
            ValidateGIStorageBuffer(m_nodesData.size(), sizeof(sg::NodeData), gpu, nodeBytes) ==
                GIResourceStatus::eSuccess &&
            ValidateGIStorageBuffer(triangles.size(), sizeof(glm::uvec4), gpu, triangleBytes) ==
                GIResourceStatus::eSuccess &&
            ValidateGIStorageBuffer(m_vertices.size(), sizeof(asset::Vertex), gpu, vertexBytes) ==
                GIResourceStatus::eSuccess &&
            ValidateGIStorageBuffer(m_materialsData.size(), sizeof(sg::MaterialData), gpu,
                                    materialBytes) == GIResourceStatus::eSuccess;
        if (valid)
        {
            RHIBuffer* nodes     = geometryChanged ?
                m_pRenderDevice->CreateStorageBuffer(
                    static_cast<uint32_t>(nodeBytes),
                    reinterpret_cast<const uint8_t*>(m_nodesData.data()),
                    "scene_nodes_generation") :
                m_pNodeSSBO;
            RHIBuffer* records   = !geometryChanged ? m_pVoxelTriangleBuffer :
                triangles.empty()                   ? nullptr :
                                                      m_pRenderDevice->CreateStorageBuffer(
                                                          static_cast<uint32_t>(triangleBytes),
                                                          reinterpret_cast<const uint8_t*>(triangles.data()),
                                                          "scene_triangles_generation");
            RHIBuffer* vertices  = m_verticesDirty ?
                m_pRenderDevice->CreateVertexBuffer(
                    static_cast<uint32_t>(vertexBytes),
                    reinterpret_cast<const uint8_t*>(m_vertices.data())) :
                m_pVertexBuffer;
            RHIBuffer* materials = m_materialsDirty ?
                m_pRenderDevice->CreateStorageBuffer(
                    static_cast<uint32_t>(materialBytes),
                    reinterpret_cast<const uint8_t*>(m_materialsData.data()),
                    "scene_materials_generation") :
                m_pMaterialSSBO;
            valid                = nodes != nullptr && (triangles.empty() || records != nullptr) &&
                vertices != nullptr && materials != nullptr;
            if (valid)
            {
                if (geometryChanged)
                {
                    m_pRenderDevice->DestroyBuffer(m_pNodeSSBO);
                    m_pRenderDevice->DestroyBuffer(m_pVoxelTriangleBuffer);
                }
                if (m_materialsDirty)
                {
                    m_pRenderDevice->DestroyBuffer(m_pMaterialSSBO);
                    InvalidateGIHistory();
                }
                if (m_verticesDirty)
                {
                    m_pRenderDevice->DestroyBuffer(m_pVertexBuffer);
                }
                m_pNodeSSBO            = nodes;
                m_pVoxelTriangleBuffer = records;
                m_pVertexBuffer        = vertices;
                m_pMaterialSSBO        = materials;
                m_voxelTriangleCount   = static_cast<uint32_t>(triangles.size());
                m_pScene->GetAABB()    = bounds;
                m_classBounds[0]       = classBounds[0];
                m_classBounds[1]       = classBounds[1];
                for (sg::Node* node : m_pScene->GetRenderableNodes())
                {
                    node->SetData(m_nodesData[node->GetRenderableIndex()]);
                }
                if (geometryChanged)
                {
                    ++m_geometryRevision;
                }
                if ((m_dirtyClasses & GI_STATIC) != 0)
                {
                    ++m_staticRevision;
                }
                if ((m_dirtyClasses & GI_DYNAMIC) != 0)
                {
                    ++m_dynamicRevision;
                }
                if (m_materialsDirty)
                {
                    ++m_surfaceRevision;
                    if ((m_dirtySurfaceClasses & GI_STATIC) != 0)
                    {
                        ++m_staticSurfaceRevision;
                    }
                    if ((m_dirtySurfaceClasses & GI_DYNAMIC) != 0)
                    {
                        ++m_dynamicSurfaceRevision;
                    }
                }
                m_dirtyClasses        = 0;
                m_dirtySurfaceClasses = 0;
                m_materialsDirty      = false;
                m_verticesDirty       = false;
            }
            else
            {
                if (geometryChanged)
                {
                    m_pRenderDevice->DestroyBuffer(nodes);
                    m_pRenderDevice->DestroyBuffer(records);
                }
                if (m_materialsDirty)
                {
                    m_pRenderDevice->DestroyBuffer(materials);
                }
                if (m_verticesDirty)
                {
                    m_pRenderDevice->DestroyBuffer(vertices);
                }
            }
        }
        if (!valid)
        {
            LOGE("Scene geometry generation rejected; rendering stopped");
        }
        m_geometryReady = valid;
    }
    return valid;
}
} // namespace zen::rc
