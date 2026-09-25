#pragma once
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "SceneGraph/Scene.h"
#include "SceneLighting.h"
#include "AssetLib/Types.h"
#include "Graphics/Shared/GIVisibility.h"

namespace zen::sg
{
class Scene;
class Camera;
} // namespace zen::sg

namespace zen::asset
{
struct Vertex;
} // namespace zen::asset

namespace zen::rc
{
struct SceneData
{
    sg::Scene* pScene;
    const asset::Vertex* pVertices;
    const uint32_t* pIndices;
    uint32_t numVertices;
    uint32_t numIndices;
    sg::Camera* pCamera;
    std::string envTextureName;
    // other scene data
};

class RenderScene
{
public:
    RenderScene(RenderDevice* pRenderDevice, const SceneData& sceneData);

    void Init();

    void Destroy();

    void LoadSceneMaterials();

    void LoadSceneTextures();

    void PrepareBuffers();

    bool Update();

    // Call from the render/main thread between frame recordings. Setters stage one
    // generation; Update commits it before any raster, shadow, or voxel passes.
    // IDs are renderable slots assigned at load; disabling/promoting never compacts them.
    bool SetInstanceClass(uint32_t instance, uint32_t objectClass);
    bool SetInstanceEnabled(uint32_t instance, bool enabled);
    bool SetInstanceTransform(uint32_t instance, const Mat4& worldTransform);
    bool UpdateVertices(uint32_t first, VectorView<const asset::Vertex> vertices);
    // Update factors/UV selection or refer to an already loaded scene texture.
    bool UpdateMaterial(uint32_t material, const sg::MaterialData& data);
    // Explicit renderer-world bounds; voxelizers make this box cubic and add one cell of padding.
    bool SetVoxelBounds(const sg::AABB& bounds);
    const sg::AABB& GetVoxelSceneBounds() const
    {
        return m_voxelBounds;
    }
    uint32_t GetVoxelCoverageMask(const sg::AABB& gridBounds) const;
    // Use for discontinuous transforms/deformation; ordinary animation keeps GI history.
    void InvalidateGIHistory()
    {
        ++m_giHistoryRevision;
    }
    uint64_t GetGIHistoryRevision() const
    {
        return m_giHistoryRevision + m_lights.GetStructureRevision() + m_environmentRevision;
    }
    bool CommitGeometryUpdates();
    uint32_t GetInstanceMask(uint32_t instance) const;
    uint64_t GetGeometryRevision(uint32_t mask = GI_ALL) const
    {
        return mask == GI_STATIC ? m_staticRevision :
            mask == GI_DYNAMIC   ? m_dynamicRevision :
                                   m_geometryRevision;
    }
    uint64_t GetSurfaceRevision(uint32_t mask = GI_ALL) const
    {
        return mask == GI_STATIC ? m_staticSurfaceRevision :
            mask == GI_DYNAMIC   ? m_dynamicSurfaceRevision :
                                   m_surfaceRevision;
    }
    const HeapVector<asset::Vertex>& GetVertices() const
    {
        return m_vertices;
    }

    uint64_t GetEnvironmentRevision() const
    {
        return m_environmentRevision;
    }

    bool SetEnvironmentLighting(float intensity, float rotationDegrees, bool enabled, bool visible);

    SceneLights& GetLights()
    {
        return m_lights;
    }

    const SceneLights& GetLights() const
    {
        return m_lights;
    }

    RHIBuffer* GetVertexBuffer() const
    {
        return m_pVertexBuffer;
    }

    RHIBuffer* GetIndexBuffer() const
    {
        return m_pIndexBuffer;
    }

    RHIBuffer* GetVoxelTriangleBuffer() const
    {
        return m_pVoxelTriangleBuffer;
    }

    uint32_t GetVoxelTriangleCount() const
    {
        return m_voxelTriangleCount;
    }

    uint32_t GetNumIndices() const
    {
        return m_numIndices;
    }

    RHIBuffer* GetNodesDataSSBO() const
    {
        return m_pNodeSSBO;
    }

    RHIBuffer* GetMaterialsDataSSBO() const
    {
        return m_pMaterialSSBO;
    }

    const EnvTexture& GetEnvTexture() const
    {
        return m_envTexture;
    }

    const HeapVector<RHITexture*>& GetSceneTextures() const
    {
        return m_sceneTextures;
    }

    const std::vector<sg::Node*>& GetRenderableNodes() const
    {
        return m_pScene->GetRenderableNodes();
    }

    const sg::AABB& GetAABB() const
    {
        return m_pScene->GetAABB();
    }

    const sg::AABB& GetLocalAABB() const
    {
        return m_pScene->GetLocalAABB();
    }

    const sg::Camera* GetCamera() const;

    const uint8_t* GetCameraUniformData() const;

    const uint8_t* GetSceneUniformData() const;

    const HeapVector<sg::MaterialData>& GetMaterialsData() const
    {
        return m_materialsData;
    }

private:
    HeapVector<glm::uvec4> BuildTriangleRecords() const;
    bool ComputeGeometryBounds(sg::AABB& bounds, sg::AABB (&classBounds)[2]) const;
    HeapVector<asset::Vertex> m_vertices;
    HeapVector<uint32_t> m_indices;
    HeapVector<uint32_t> m_instanceClasses;
    HeapVector<uint32_t> m_instanceEnabled;
    uint32_t m_dirtyClasses{0};
    uint32_t m_dirtySurfaceClasses{0};
    bool m_materialsDirty{false};
    bool m_verticesDirty{false};
    bool m_geometryReady{true};
    uint64_t m_geometryRevision{1};
    uint64_t m_giHistoryRevision{1};
    uint64_t m_staticRevision{1};
    uint64_t m_dynamicRevision{1};
    uint64_t m_surfaceRevision{1};
    uint64_t m_staticSurfaceRevision{1};
    uint64_t m_dynamicSurfaceRevision{1};
    sg::AABB m_voxelBounds;
    sg::AABB m_classBounds[2];
    RenderDevice* m_pRenderDevice{nullptr};
    sg::Scene* m_pScene{nullptr};
    sg::Camera* m_pCamera{nullptr};

    HeapVector<sg::NodeData> m_nodesData;
    RHIBuffer* m_pNodeSSBO{nullptr};

    HeapVector<sg::MaterialData> m_materialsData;
    RHIBuffer* m_pMaterialSSBO{nullptr};

    SceneUniformData m_sceneUniformData{};
    SceneLights m_lights;
    uint64_t m_environmentRevision{1};

    RHIBuffer* m_pVertexBuffer{nullptr};
    RHIBuffer* m_pIndexBuffer{nullptr};


    uint32_t m_numIndices{0};
    RHIBuffer* m_pVoxelTriangleBuffer{nullptr};
    uint32_t m_voxelTriangleCount{0};

    // std::vector<TextureHandle> m_sceneTextures;
    HeapVector<RHITexture*> m_sceneTextures;
    std::string m_envTextureName;
    EnvTexture m_envTexture;
    RHITexture* m_pDefaultBaseColorTexture{nullptr};
    // TextureHandle m_defaultBaseColorTexture;
};
} // namespace zen::rc
