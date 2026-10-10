#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <cmath>
#include <cstring>

namespace zen::rc
{
namespace
{
bool FiniteMaterialVector(const Vec4& value)
{
    bool finite = true;

    for (uint32_t component = 0; component < 4; ++component)
    {
        finite &= std::isfinite(value[component]);
    }

    return finite;
}

bool UnitMaterialFactor(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool ValidateMaterialTextureTransform(const sg::TextureTransformData& transform, size_t samplerCount)
{
    return FiniteMaterialVector(transform.row0) && FiniteMaterialVector(transform.row1) && transform.row0.w >= 0
        && transform.row0.w <= samplerCount && transform.row0.w == std::floor(transform.row0.w);
}
} // namespace

uint32_t RenderScene::GetInstanceMask(uint32_t instance) const
{
    return instance < m_instanceClasses.size() && instance < m_authoredVisibility.size() && m_instanceEnabled[instance] != 0
                && m_authoredVisibility[instance] != 0
             ? m_instanceClasses[instance]
             : 0;
}

bool RenderScene::SetInstanceClass(uint32_t instance, uint32_t objectClass)
{
    const bool valid = instance < m_instanceClasses.size() && (objectClass == GI_STATIC || objectClass == GI_DYNAMIC);

    if (valid && m_instanceClasses[instance] != objectClass)
    {
        m_dirtyClasses              |= m_instanceClasses[instance] | objectClass;

        m_instanceClasses[instance]  = objectClass;
    }

    return valid;
}

bool RenderScene::SetInstanceEnabled(uint32_t instance, bool enabled)
{
    const bool valid = instance < m_instanceClasses.size();

    if (valid && (m_instanceEnabled[instance] != 0) != enabled)
    {
        m_dirtyClasses              |= m_instanceClasses[instance];

        m_instanceEnabled[instance]  = enabled ? 1 : 0;
    }

    return valid;
}

bool RenderScene::SetInstanceTransform(uint32_t instance, const Mat4& worldTransform)
{
    const float determinant = glm::determinant(worldTransform);

    bool valid              = instance < m_nodesData.size() && instance < m_instanceClasses.size() && worldTransform[0][3] == 0
              && worldTransform[1][3] == 0 && worldTransform[2][3] == 0 && worldTransform[3][3] == 1
              && std::isfinite(determinant) && determinant != 0;

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

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double scale = glm::length(glm::dvec3(worldTransform[axis]));

            m_nodesData[instance].surfaceScale[axis] =
                static_cast<float>(std::min(scale, static_cast<double>(std::numeric_limits<float>::max())));
        }

        m_nodesData[instance].surfaceScale.w  = glm::determinant(glm::dmat3(worldTransform)) < 0.0 ? -1.0f : 1.0f;

        m_dirtyClasses                       |= m_instanceClasses[instance];
    }

    return valid;
}

bool RenderScene::UpdateVertices(uint32_t first, VectorView<const asset::Vertex> vertices)
{
    bool valid = first <= m_vertices.size() && vertices.size() <= m_vertices.size() - first
              && (vertices.empty() || vertices.data() != nullptr);

    for (size_t index = 0; valid && index < vertices.size(); ++index)
    {
        const asset::Vertex& vertex = vertices[index];

        for (uint32_t i = 0; i < 4; ++i)
        {
            valid = valid && std::isfinite(vertex.pos[i]) && std::isfinite(vertex.normal[i]) && std::isfinite(vertex.tangent[i])
                 && std::isfinite(vertex.color[i]) && std::isfinite(vertex.joint0[i]) && std::isfinite(vertex.weight0[i]);
        }

        valid = valid && std::isfinite(vertex.uv0.x) && std::isfinite(vertex.uv0.y) && std::isfinite(vertex.uv1.x)
             && std::isfinite(vertex.uv1.y);
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

                    affected              = affected || (vertex >= first && vertex - first < vertices.size());
                }
            }

            if (affected)
            {
                m_dirtyClasses        |= m_instanceClasses[node->GetRenderableIndex()];

                m_dirtySurfaceClasses |= m_instanceClasses[node->GetRenderableIndex()];
            }
        }
    }

    return valid;
}

bool RenderScene::UpdateMaterial(uint32_t material, const sg::MaterialData& sourceData)
{
    sg::MaterialData data            = sourceData;

    const double attenuationDistance = static_cast<double>(data.volumeIridescence.y) * m_sceneUnitScale;

    data.volumeIridescence.y =
        attenuationDistance > std::numeric_limits<float>::max() ? 0.0f : static_cast<float>(attenuationDistance);

    bool valid = material < m_materialsData.size() && UnitMaterialFactor(data.metallicFactor)
              && UnitMaterialFactor(data.roughnessFactor) && data.surfaceProperties.x >= 0 && data.surfaceProperties.y >= 0
              && data.surfaceProperties.y <= 2 && data.surfaceProperties.y == std::floor(data.surfaceProperties.y)
              && UnitMaterialFactor(data.surfaceProperties.w) && UnitMaterialFactor(data.materialProperties.x)
              && UnitMaterialFactor(data.materialProperties.y) && UnitMaterialFactor(data.materialProperties.z)
              && UnitMaterialFactor(data.materialProperties.w);

    for (const Vec4& value :
         {data.baseColorFactor, data.emissiveFactor, data.surfaceProperties, data.materialProperties, data.specularColorIor,
          data.specularGlossiness, data.diffuseFactor, data.clearcoatSheenSpecular, data.sheenColorTransmission,
          data.volumeIridescence, data.attenuationColorDispersion, data.iridescenceAnisotropy,
          data.diffuseTransmissionColorFactor, data.volumeScatterColorRetroreflection})
    {
        valid &= FiniteMaterialVector(value);
    }

    for (uint32_t component = 0; component < 4; ++component)
    {
        valid &= UnitMaterialFactor(data.baseColorFactor[component]) && data.emissiveFactor[component] >= 0.0f
              && UnitMaterialFactor(data.diffuseFactor[component]) && UnitMaterialFactor(data.clearcoatSheenSpecular[component])
              && UnitMaterialFactor(data.volumeScatterColorRetroreflection[component]);
    }

    for (uint32_t component = 0; component < 3; ++component)
    {
        valid &= data.specularColorIor[component] >= 0.0f && data.specularGlossiness[component] >= 0.0f
              && UnitMaterialFactor(data.sheenColorTransmission[component])
              && UnitMaterialFactor(data.attenuationColorDispersion[component])
              && UnitMaterialFactor(data.diffuseTransmissionColorFactor[component]);
    }

    valid &= (data.specularColorIor.w == 0.0f || data.specularColorIor.w >= 1.0f)
          && UnitMaterialFactor(data.specularGlossiness.w) && UnitMaterialFactor(data.sheenColorTransmission.w)
          && data.volumeIridescence.x >= 0.0f && data.volumeIridescence.y >= 0.0f
          && UnitMaterialFactor(data.volumeIridescence.z) && data.volumeIridescence.w >= 1.0f
          && data.attenuationColorDispersion.w >= 0.0f && data.iridescenceAnisotropy.x >= 0.0f
          && data.iridescenceAnisotropy.y >= data.iridescenceAnisotropy.x && UnitMaterialFactor(data.iridescenceAnisotropy.z)
          && UnitMaterialFactor(data.diffuseTransmissionColorFactor.w);

    for (const sg::TextureTransformData& transform : data.textureTransforms)
    {
        valid &= ValidateMaterialTextureTransform(transform, m_sceneSamplers.size());
    }

    for (int index : {data.bcTexIndex, data.mrTexIndex, data.normalTexIndex, data.occlusionTexIndex, data.emissiveTexIndex})
    {
        valid = valid
             && (index == -1
                 || (index >= 0 && static_cast<size_t>(index) < m_sceneTextures.size() && m_sceneTextures[index] != nullptr));
    }

    for (int uv : {data.bcTexSet, data.mrTexSet, data.normalTexSet, data.aoTexSet, data.emissiveTexSet})
    {
        valid = valid && uv >= -1 && (uv <= 1 || (!m_uvCoordinates.empty() && uv < m_uvCoordinates[0].x));
    }

    for (const sg::MaterialTextureData& binding : data.featureTextures)
    {
        valid &= FiniteMaterialVector(binding.properties)
              && ValidateMaterialTextureTransform(binding.transform, m_sceneSamplers.size());

        const float index  = binding.properties.x;

        valid             &= index == std::floor(index) && index >= -1.0f && (index == -1.0f || index < m_sceneTextures.size());

        if (index >= 0.0f && index < m_sceneTextures.size())
        {
            valid &= m_sceneTextures[static_cast<size_t>(index)] != nullptr;
        }

        const float uv = binding.properties.y;

        valid &= uv >= 0.0f && uv == std::floor(uv) && (uv <= 1.0f || (!m_uvCoordinates.empty() && uv < m_uvCoordinates[0].x));
    }

    if (valid && std::memcmp(&m_materialsData[material], &data, sizeof(data)) != 0)
    {
        const sg::MaterialData& previous = m_materialsData[material];

        const bool coverageChanged =
            previous.baseColorFactor.a != data.baseColorFactor.a || previous.bcTexIndex != data.bcTexIndex
            || previous.bcTexSet != data.bcTexSet || previous.sheenColorTransmission.w != data.sheenColorTransmission.w
            || previous.materialProperties.z != data.materialProperties.z
            || previous.surfaceProperties.w != data.surfaceProperties.w || previous.diffuseFactor.a != data.diffuseFactor.a
            || std::memcmp(&previous.featureTextures[2], &data.featureTextures[2], sizeof(data.featureTextures[2])) != 0
            || glm::vec2(previous.surfaceProperties) != glm::vec2(data.surfaceProperties)
            || previous.textureTransforms[0].row0 != data.textureTransforms[0].row0
            || previous.textureTransforms[0].row1 != data.textureTransforms[0].row1;

        for (const sg::Node* node : m_pScene->GetRenderableNodes())
        {
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                if (mesh->GetMaterial()->index == material)
                {
                    const uint32_t objectClass  = m_instanceClasses[node->GetRenderableIndex()];

                    m_dirtySurfaceClasses      |= objectClass;

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
        valid = valid && bounds.GetMin()[axis] <= bounds.GetMax()[axis] && std::isfinite(center[axis] - side)
             && std::isfinite(center[axis] + side);
    }

    if (valid && bounds != m_voxelBounds)
    {
        m_voxelBounds   = bounds;

        m_dirtyClasses |= GI_ALL;
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

        if (empty
            || (glm::all(glm::greaterThanEqual(bounds.GetMin(), gridBounds.GetMin()))
                && glm::all(glm::lessThanEqual(bounds.GetMax(), gridBounds.GetMax()))))
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

                    const Vec3 world(m_nodesData[instance].modelMatrix * Vec4(Vec3(m_vertices[vertex].pos), 1));

                    valid = valid && std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);

                    bounds.SetMin(world);

                    bounds.SetMax(world);

                    sg::AABB& objectBounds = classBounds[GetInstanceMask(instance) == GI_STATIC ? 0 : 1];

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

        uint64_t nodeBytes = 0, triangleBytes = 0, vertexBytes = 0, materialBytes = 0, uvBytes = 0;

        bool validUVCoordinates = true;

        if (m_verticesDirty)
        {
            const size_t stride = static_cast<size_t>(m_uvCoordinates[0].x);

            for (size_t vertex = 0; vertex < m_vertices.size(); ++vertex)
            {
                if (vertex < m_pScene->GetAssetData().vertexTexCoords.size())
                {
                    const HeapVector<Vec2>& sets = m_pScene->GetAssetData().vertexTexCoords[vertex];

                    for (size_t set = 2; set < std::min(stride, sets.size()); ++set)
                    {
                        validUVCoordinates                         &= std::isfinite(sets[set].x) && std::isfinite(sets[set].y);

                        m_uvCoordinates[1 + vertex * stride + set]  = Vec4(sets[set], 0, 0);
                    }
                }

                m_uvCoordinates[1 + vertex * stride] = Vec4(m_vertices[vertex].uv0, 0, 0);

                m_uvCoordinates[2 + vertex * stride] = Vec4(m_vertices[vertex].uv1, 0, 0);
            }
        }

        sg::AABB bounds;

        sg::AABB classBounds[2];

        valid =
            validUVCoordinates && ComputeGeometryBounds(bounds, classBounds)
            && ValidateGIStorageBuffer(m_nodesData.size(), sizeof(sg::NodeData), gpu, nodeBytes) == GIResourceStatus::eSuccess
            && ValidateGIStorageBuffer(triangles.size(), sizeof(glm::uvec4), gpu, triangleBytes) == GIResourceStatus::eSuccess
            && ValidateGIStorageBuffer(m_vertices.size(), sizeof(asset::Vertex), gpu, vertexBytes) == GIResourceStatus::eSuccess
            && ValidateGIStorageBuffer(m_materialsData.size(), sizeof(sg::MaterialData), gpu, materialBytes)
                   == GIResourceStatus::eSuccess
            && ValidateGIStorageBuffer(m_uvCoordinates.size(), sizeof(Vec4), gpu, uvBytes) == GIResourceStatus::eSuccess;

        if (valid)
        {
            RHIBuffer* nodes    = geometryChanged
                                    ? m_pRenderDevice->CreateStorageBuffer(static_cast<uint32_t>(nodeBytes),
                                                                           reinterpret_cast<const uint8_t*>(m_nodesData.data()),
                                                                           "scene_nodes_generation")
                                    : m_pNodeSSBO;

            RHIBuffer* records  = !geometryChanged ? m_pVoxelTriangleBuffer
                                : triangles.empty()
                                    ? nullptr
                                    : m_pRenderDevice->CreateStorageBuffer(static_cast<uint32_t>(triangleBytes),
                                                                           reinterpret_cast<const uint8_t*>(triangles.data()),
                                                                           "scene_triangles_generation");

            RHIBuffer* vertices = m_verticesDirty
                                    ? m_pRenderDevice->CreateVertexBuffer(static_cast<uint32_t>(vertexBytes),
                                                                          reinterpret_cast<const uint8_t*>(m_vertices.data()))
                                    : m_pVertexBuffer;

            RHIBuffer* materials =
                m_materialsDirty
                    ? m_pRenderDevice->CreateStorageBuffer(static_cast<uint32_t>(materialBytes),
                                                           reinterpret_cast<const uint8_t*>(m_materialsData.data()),
                                                           "scene_materials_generation")
                    : m_pMaterialSSBO;

            RHIBuffer* uvCoordinates =
                m_verticesDirty ? m_pRenderDevice->CreateStorageBuffer(static_cast<uint32_t>(uvBytes),
                                                                       reinterpret_cast<const uint8_t*>(m_uvCoordinates.data()),
                                                                       "scene_uv_generation")
                                : m_pUVBuffer;

            valid = nodes != nullptr && (triangles.empty() || records != nullptr) && vertices != nullptr && materials != nullptr
                 && uvCoordinates != nullptr;

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
                }

                if (m_verticesDirty)
                {
                    m_pRenderDevice->DestroyBuffer(m_pVertexBuffer);

                    m_pRenderDevice->DestroyBuffer(m_pUVBuffer);
                }

                m_pNodeSSBO            = nodes;

                m_pVoxelTriangleBuffer = records;

                m_pVertexBuffer        = vertices;

                m_pMaterialSSBO        = materials;

                m_pUVBuffer            = uvCoordinates;

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

                if (m_materialsDirty || m_dirtySurfaceClasses != 0)
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

                    m_pRenderDevice->DestroyBuffer(uvCoordinates);
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
