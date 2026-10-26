#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <algorithm>
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

bool ValidateMaterialAtlasBinding(const sg::TextureTransformData& transform, int index, const sg::Scene& scene)
{
    const HeapVector<sg::Texture*>& textures = scene.GetComponents<sg::Texture>();
    asset::ImageRegion              region;
    int                             sampler      = 0;
    float                           pageSizeCode = 0.0f;
    if (index >= 0 && static_cast<size_t>(index) < textures.size())
    {
        region       = textures[index]->GetAtlasRegion();
        sampler      = textures[index]->samplerIndex + 1;
        pageSizeCode = sg::GetAtlasPageSizeCode(textures[index]);
    }
    return transform.atlasRegion == glm::uvec4(region.x, region.y, region.width, region.height) && pageSizeCode >= 0.0f
        && transform.row1.w == pageSizeCode
        && (region == asset::ImageRegion{} || transform.row0.w == static_cast<float>(sampler));
}
} // namespace

uint32_t RenderScene::GetInstanceMask(uint32_t instance) const
{
    return instance < m_instanceClasses.size() && instance < m_authoredVisibility.size() && m_instanceEnabled[instance] != 0
                && m_authoredVisibility[instance] != 0
             ? m_instanceClasses[instance]
             : 0;
}

const HeapVector<RenderScene::ShadowCaster>& RenderScene::GetShadowCasters() const
{
    return m_shadowCasters;
}

bool RenderScene::SetInstanceClass(uint32_t instance, uint32_t objectClass)
{
    const bool valid = instance < m_instanceClasses.size() && (objectClass == GI_STATIC || objectClass == GI_DYNAMIC);

    if (valid && m_instanceClasses[instance] != objectClass)
    {
        m_dirtyClasses                   |= m_instanceClasses[instance] | objectClass;

        m_instanceClasses[instance]       = objectClass;

        m_dirtyShadowInstances[instance]  = 1;

        m_triangleRecordsDirty            = true;
    }

    return valid;
}

bool RenderScene::SetInstanceEnabled(uint32_t instance, bool enabled)
{
    const bool valid = instance < m_instanceClasses.size();

    if (valid && (m_instanceEnabled[instance] != 0) != enabled)
    {
        m_dirtyClasses                   |= m_instanceClasses[instance];

        m_instanceEnabled[instance]       = enabled ? 1 : 0;

        m_dirtyShadowInstances[instance]  = 1;

        m_triangleRecordsDirty            = true;
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

    if (valid && m_nodesData[instance].modelMatrix != worldTransform)
    {
        m_nodesData[instance]            = {worldTransform, normalMatrix};

        m_dirtyShadowInstances[instance] = 1;

        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double scale = glm::length(glm::dvec3(worldTransform[axis]));

            m_nodesData[instance].surfaceScale[axis] =
                static_cast<float>(std::min(scale, static_cast<double>(std::numeric_limits<float>::max())));
        }

        m_nodesData[instance].surfaceScale.w  = glm::determinant(glm::dmat3(worldTransform)) < 0.0 ? -1.0f : 1.0f;

        m_dirtyClasses                       |= m_instanceClasses[instance];

        m_nodeBuffers.MarkDirty(instance * sizeof(sg::NodeData), sizeof(sg::NodeData));
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
        MarkVertexChanges(vertices, first);

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
    data.textureSampling             = m_pRenderDevice->GetTextureSamplingParameters();

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

    const int textureIndices[] = {data.bcTexIndex, data.mrTexIndex, data.normalTexIndex, data.occlusionTexIndex,
                                  data.emissiveTexIndex};
    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        valid &= ValidateMaterialAtlasBinding(data.textureTransforms[slot], textureIndices[slot], *m_pScene);
    }

    for (int uv : {data.bcTexSet, data.mrTexSet, data.normalTexSet, data.aoTexSet, data.emissiveTexSet})
    {
        valid = valid && uv >= -1 && (uv <= 1 || (!m_uvCoordinates.empty() && uv < m_uvCoordinates[0].x));
    }

    for (const sg::MaterialTextureData& binding : data.featureTextures)
    {
        valid             &= FiniteMaterialVector(binding.properties)
                          && ValidateMaterialTextureTransform(binding.transform, m_sceneSamplers.size());

        const float index  = binding.properties.x;

        valid             &= index == std::floor(index) && index >= -1.0f && (index == -1.0f || index < m_sceneTextures.size());

        if (index >= 0.0f && index < m_sceneTextures.size())
        {
            valid &= m_sceneTextures[static_cast<size_t>(index)] != nullptr;
        }

        if (valid)
        {
            valid &= ValidateMaterialAtlasBinding(binding.transform, static_cast<int>(index), *m_pScene);
        }

        const float uv = binding.properties.y;

        valid &= uv >= 0.0f && uv == std::floor(uv) && (uv <= 1.0f || (!m_uvCoordinates.empty() && uv < m_uvCoordinates[0].x));
    }

    valid &=
        data.normalEncoding.x <= 1 && data.normalEncoding.y <= 1 && data.normalEncoding.z == 0 && data.normalEncoding.w == 0;
    for (uint32_t slot = 0; valid && slot < 2; ++slot)
    {
        const int  index    = slot == 0 ? data.normalTexIndex : static_cast<int>(data.featureTextures[6].properties.x);
        const bool xyNormal = index >= 0 && m_sceneTextures[index]->GetFormat() == DataFormat::eBC5UNORMBlock;
        valid               = (data.normalEncoding[slot] == 1) == xyNormal;
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
            || previous.textureTransforms[0].row1 != data.textureTransforms[0].row1
            || previous.textureTransforms[0].atlasRegion != data.textureTransforms[0].atlasRegion;

        for (const sg::Node* node : m_pScene->GetRenderableNodes())
        {
            for (const sg::SubMesh* mesh : node->GetComponent<sg::Mesh>()->GetSubMeshes())
            {
                if (mesh->GetMaterial()->index == material)
                {
                    const uint32_t objectClass = m_instanceClasses[node->GetRenderableIndex()];

                    // Raster shadow coverage also depends on transmission, tint, IOR,
                    // thickness and their textures, beyond the ray-query alpha contract.
                    m_dirtyShadowInstances[node->GetRenderableIndex()]  = 1;

                    m_dirtySurfaceClasses                              |= objectClass;

                    if (coverageChanged)
                    {
                        m_dirtyClasses |= objectClass;
                    }
                }
            }
        }

        m_materialsData[material] = data;

        m_materialsDirty          = true;

        m_materialBuffers.MarkDirty(material * sizeof(sg::MaterialData), sizeof(sg::MaterialData));
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

void RenderScene::BuildMeshVertexIndices()
{
    HeapVector<uint32_t> visited(m_vertices.size(), UINT32_MAX);
    for (const sg::Node* node : m_pScene->GetRenderableNodes())
    {
        const sg::Mesh* mesh = node->GetComponent<sg::Mesh>();
        if (m_meshVertexIndices.find(mesh) == m_meshVertexIndices.end())
        {
            const uint32_t        meshIndex = static_cast<uint32_t>(m_meshVertexIndices.size());
            HeapVector<uint32_t>& vertices  = m_meshVertexIndices[mesh];
            for (const sg::SubMesh* primitive : mesh->GetSubMeshes())
            {
                for (uint32_t i = 0; i < primitive->GetIndexCount(); ++i)
                {
                    const uint32_t vertex = m_indices[primitive->GetFirstIndex() + i];
                    if (visited[vertex] != meshIndex)
                    {
                        visited[vertex] = meshIndex;
                        vertices.push_back(vertex);
                    }
                }
            }
        }
    }
}

bool RenderScene::ComputeGeometryBounds(sg::AABB&                 bounds,
                                        sg::AABB                  (&classBounds)[2],
                                        HeapVector<ShadowCaster>& shadowCasters) const
{
    bool valid     = true;

    bounds         = m_voxelBounds;

    classBounds[0] = classBounds[1] = sg::AABB();

    shadowCasters                   = m_shadowCasters;
    shadowCasters.resize(m_nodesData.size());

    for (const sg::Node* node : m_pScene->GetRenderableNodes())
    {
        const uint32_t instance = node->GetRenderableIndex();

        const uint32_t mask     = GetInstanceMask(instance);

        ShadowCaster& caster    = shadowCasters[instance];
        caster.bounds           = sg::AABB();
        bool shadowChanged      = m_dirtyShadowInstances[instance] != 0;

        if (mask != 0)
        {
            const HashMap<const sg::Mesh*, HeapVector<uint32_t>>::const_iterator vertices =
                m_meshVertexIndices.find(node->GetComponent<sg::Mesh>());

            valid &= vertices != m_meshVertexIndices.end();

            if (vertices != m_meshVertexIndices.end())
            {
                sg::AABB& objectBounds = classBounds[mask == GI_STATIC ? 0 : 1];

                for (uint32_t vertex : vertices->second)
                {
                    const Vec3 world(m_nodesData[instance].modelMatrix * Vec4(Vec3(m_vertices[vertex].pos), 1));

                    valid = valid && std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);

                    bounds.SetMin(world);

                    bounds.SetMax(world);

                    objectBounds.SetMin(world);

                    objectBounds.SetMax(world);

                    caster.bounds.SetMin(world);

                    caster.bounds.SetMax(world);

                    shadowChanged |= m_dirtyShadowVertices[vertex] != 0;
                }
            }
        }

        if (instance < m_shadowCasters.size() && (shadowChanged || caster.bounds != m_shadowCasters[instance].bounds))
        {
            ++caster.revision;
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

RHIBuffer* RenderScene::GetVertexBuffer() const
{
    return m_vertexBuffers.GetBuffer();
}

RHIBuffer* RenderScene::GetUVBuffer() const
{
    return m_uvBuffers.GetBuffer();
}

RHIBuffer* RenderScene::GetNodesDataSSBO() const
{
    return m_nodeBuffers.GetBuffer();
}

RHIBuffer* RenderScene::GetMaterialsDataSSBO() const
{
    return m_materialBuffers.GetBuffer();
}

uint64_t RenderScene::GetVertexGeneration() const
{
    return m_vertexBuffers.GetGeneration();
}

bool RenderScene::HasVertexGeneration(const RHIBuffer* buffer, uint64_t generation) const
{
    return m_vertexBuffers.ContainsGeneration(buffer, generation);
}

void RenderScene::MarkVertexChanges(VectorView<const asset::Vertex> vertices, uint32_t first)
{
    size_t begin = 0;
    while (begin < vertices.size())
    {
        if (std::memcmp(&vertices[begin], &m_vertices[first + begin], sizeof(asset::Vertex)) == 0)
        {
            ++begin;
        }
        else
        {
            size_t end = begin + 1;
            while (end < vertices.size() && std::memcmp(&vertices[end], &m_vertices[first + end], sizeof(asset::Vertex)) != 0)
            {
                ++end;
            }
            m_vertexBuffers.MarkDirty(static_cast<uint32_t>((first + begin) * sizeof(asset::Vertex)),
                                      static_cast<uint32_t>((end - begin) * sizeof(asset::Vertex)));
            std::fill(m_dirtyShadowVertices.begin() + first + begin, m_dirtyShadowVertices.begin() + first + end, uint8_t(1));
            begin = end;
        }
    }
}

bool RenderScene::RefreshUVCoordinates()
{
    bool         valid  = true;
    const size_t stride = static_cast<size_t>(m_uvCoordinates[0].x);
    for (size_t vertex = 0; vertex < m_vertices.size(); ++vertex)
    {
        const HeapVector<sg::VertexTextureCoordinates>& coordinates = m_pScene->GetAssetData().vertexTexCoords;
        const size_t sets = vertex < coordinates.size() ? std::min(stride, coordinates[vertex].size()) : 0;
        for (size_t set = 0; set < std::max(sets, size_t(2)); ++set)
        {
            const Vec2 uv = set == 0 ? m_vertices[vertex].uv0 : set == 1 ? m_vertices[vertex].uv1 : coordinates[vertex][set];
            valid         = valid && std::isfinite(uv.x) && std::isfinite(uv.y);
            const size_t offset = 1 + vertex * stride + set;
            if (Vec2(m_uvCoordinates[offset]) != uv)
            {
                m_uvCoordinates[offset]       = Vec4(uv, 0, 0);
                m_dirtyShadowVertices[vertex] = 1;
                m_uvBuffers.MarkDirty(static_cast<uint32_t>(offset * sizeof(Vec4)), sizeof(Vec4));
            }
        }
    }
    return valid;
}

bool RenderScene::CommitGeometryUpdates()
{
    bool valid = m_geometryReady && !m_pRenderDevice->AreSubmissionsBlocked();
    if (valid && (m_dirtyClasses != 0 || m_verticesDirty || m_materialsDirty || m_triangleRecordsDirty))
    {
        const bool                   geometryChanged = m_dirtyClasses != 0 || m_verticesDirty || m_triangleRecordsDirty;
        const HeapVector<glm::uvec4> triangles = m_triangleRecordsDirty ? BuildTriangleRecords() : HeapVector<glm::uvec4>();
        const uint32_t triangleCount = m_triangleRecordsDirty ? static_cast<uint32_t>(triangles.size()) : m_voxelTriangleCount;
        const RHIGPUInfo& gpu           = m_pRenderDevice->GetGPUInfo();
        uint64_t          nodeBytes = 0, triangleBytes = 0, vertexBytes = 0, materialBytes = 0, uvBytes = 0;
        sg::AABB          bounds;
        sg::AABB          classBounds[2];
        HeapVector<ShadowCaster> shadowCasters;
        valid = (!m_verticesDirty || RefreshUVCoordinates()) && ComputeGeometryBounds(bounds, classBounds, shadowCasters)
             && ValidateGIStorageBuffer(std::max(m_nodesData.size(), size_t(1)), sizeof(sg::NodeData), gpu, nodeBytes)
                    == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(triangleCount, sizeof(glm::uvec4), gpu, triangleBytes) == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(std::max(m_vertices.size(), size_t(1)), sizeof(asset::Vertex), gpu, vertexBytes)
                    == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(std::max(m_materialsData.size(), size_t(1)), sizeof(sg::MaterialData), gpu,
                                        materialBytes)
                    == GIResourceStatus::eSuccess
             && ValidateGIStorageBuffer(m_uvCoordinates.size(), sizeof(Vec4), gpu, uvBytes) == GIResourceStatus::eSuccess;
        if (valid)
        {
            RHIBuffer* records = !m_triangleRecordsDirty ? m_pVoxelTriangleBuffer
                               : triangles.empty()
                                   ? nullptr
                                   : m_pRenderDevice->CreateStorageBuffer(static_cast<uint32_t>(triangleBytes),
                                                                          reinterpret_cast<const uint8_t*>(triangles.data()),
                                                                          "scene_triangles_generation");
            const sg::NodeData     emptyNode{};
            const sg::MaterialData emptyMaterial{};
            const asset::Vertex    emptyVertex{};
            valid =
                (triangleCount == 0 || records != nullptr)
                && m_nodeBuffers.Update(*m_pRenderDevice, m_nodesData.empty() ? &emptyNode : m_nodesData.data(),
                                        static_cast<uint32_t>(nodeBytes), "scene_nodes")
                && m_vertexBuffers.Update(*m_pRenderDevice, m_vertices.empty() ? &emptyVertex : m_vertices.data(),
                                          static_cast<uint32_t>(vertexBytes), "scene_vertices", true)
                && m_materialBuffers.Update(*m_pRenderDevice, m_materialsData.empty() ? &emptyMaterial : m_materialsData.data(),
                                            static_cast<uint32_t>(materialBytes), "scene_materials")
                && m_uvBuffers.Update(*m_pRenderDevice, m_uvCoordinates.data(), static_cast<uint32_t>(uvBytes), "scene_uv");
            if (valid)
            {
                if (m_triangleRecordsDirty)
                {
                    m_pRenderDevice->DestroyBuffer(m_pVoxelTriangleBuffer);
                }
                m_pVoxelTriangleBuffer = records;
                m_voxelTriangleCount   = triangleCount;
                m_pScene->GetAABB()    = bounds;
                m_classBounds[0]       = classBounds[0];
                m_classBounds[1]       = classBounds[1];
                m_shadowCasters        = std::move(shadowCasters);
                std::fill(m_dirtyShadowInstances.begin(), m_dirtyShadowInstances.end(), uint8_t(0));
                if (m_verticesDirty)
                {
                    std::fill(m_dirtyShadowVertices.begin(), m_dirtyShadowVertices.end(), uint8_t(0));
                }
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
                m_dirtyClasses         = 0;
                m_dirtySurfaceClasses  = 0;
                m_materialsDirty       = false;
                m_verticesDirty        = false;
                m_triangleRecordsDirty = false;
            }
            else if (m_triangleRecordsDirty)
            {
                m_pRenderDevice->DestroyBuffer(records);
            }
        }
        if (!valid)
        {
            LOGE("Scene geometry update rejected; rendering stopped");
        }
        m_geometryReady = valid;
    }
    return valid;
}
} // namespace zen::rc
