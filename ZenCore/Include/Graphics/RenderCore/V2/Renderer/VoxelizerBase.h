#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Utils/UniquePtr.h"
#include "SceneGraph/AABB.h"
#include "Graphics/Shared/GIVisibility.h"

namespace zen::rc
{
class RenderScene;
class RenderDevice;

struct VoxelTextures
{
    RHITexture* pOwner{nullptr};
    RHITexture* pAlbedo{nullptr};
    RHITextureView* pAlbedoView{nullptr};
    RHITexture* pNormal{nullptr};
    RHITextureView* pNormalView{nullptr};
    RHITexture* pEmissive{nullptr};
    RHITextureView* pEmissiveView{nullptr};
    RHITexture* pReflectance{nullptr};
};

class VoxelizerBase
{
public:
    VoxelizerBase(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    virtual ~VoxelizerBase() = default;

    virtual void Init() = 0;
    bool ConfigureClass(uint32_t mask);
    uint32_t GetClassMask() const
    {
        return m_classMask;
    }
    bool EnableCompaction();
    RHIBuffer* GetOccupiedList() const
    {
        return m_occupiedList;
    }
    RHIBuffer* GetGridToList() const
    {
        return m_gridToList;
    }
    RHIBuffer* GetOccupiedCount() const
    {
        return m_occupiedCount;
    }

    virtual void BuildRenderGraph();

    virtual void BuildVoxelizationGraph() {}
    virtual void BuildVisualizationGraph() {}

    uint64_t GetGeometryRevision() const
    {
        return m_geometryRevision;
    }

    // Cache keys used while recording this frame include its pending voxel update.
    uint64_t GetRecordedGeometryRevision() const
    {
        return m_geometryRevision + (m_voxelizationPending ? 1 : 0);
    }
    // Surface-only rebuilds keep compaction and the geometric visibility cache valid.
    uint64_t GetRecordedVisibilityRevision() const
    {
        return m_visibilityRevision + (m_voxelizationPending && m_visibilityChanged ? 1 : 0);
    }

    virtual void OnRenderGraphExecuted(bool succeeded);

    sg::AABB GetVoxelBounds() const;

    virtual void SetRenderScene(RenderScene* pScene);

    virtual void Destroy();

    void RequestVoxelization()
    {
        m_needVoxelization = true;
    }

    // Only producers opting into radiance inputs allocate/reset normal and emissive volumes.
    // The current visualization paths produce albedo only.
    virtual bool ProducesRadianceInputs() const
    {
        return m_voxelTextures.pNormal != nullptr && m_voxelTextures.pEmissive != nullptr;
    }

    bool EnableRadianceInputs();
    bool EnsureReady();
    bool IsReady() const;

    bool UsesAveragedReflectance() const
    {
        return m_useAveragedReflectance;
    }

    RHIBuffer* GetReflectanceSums() const
    {
        return m_pReflectanceSums;
    }

    const VoxelTextures& GetVoxelTextures() const
    {
        return m_voxelTextures;
    }

    RHISampler* GetVoxelSampler() const
    {
        return m_pVoxelSampler;
    }

    DataFormat GetVoxelTexFormat() const
    {
        return m_voxelTexFormat;
    }

    uint32_t GetVoxelTexResolution() const
    {
        return m_voxelTexResolution;
    }

    float GetVoxelSize() const;

    float GetVoxelScale() const;

    Vec3 GetSceneMinPoint() const;

protected:
    // Start every initial/repeated voxelization from empty accumulation volumes.
    bool BeginVoxelization(RenderGraph& graph,
                           RDGQueuePreference queuePreference = RDGQueuePreference::eDefault);

    void BindVoxelScene(RDGPassDescBase& pass) const;
    void BindReflectanceSums(RDGPassDescBase& pass) const;
    void PrepareReflectance();
    void ResolveSurface(RDGQueuePreference queuePreference);
    void BuildCompaction(RDGQueuePreference queuePreference);
    RHITexture* CreateVolume(DataFormat format, NameID name, uint32_t mipCount = 1);

    virtual void PrepareTextures();

    virtual void PrepareBuffers() {}

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    VoxelTextures m_voxelTextures;

    RHISampler* m_pVoxelSampler{nullptr};
    RHISampler* m_pColorSampler{nullptr};

    uint32_t m_voxelTexResolution;
    uint32_t m_voxelCount;
    DataFormat m_voxelTexFormat;

    bool m_needVoxelization{true};
    bool m_textureInitializationAttempted{false};
    bool m_voxelizationPending{false};
    uint64_t m_geometryRevision{0};
    uint64_t m_visibilityRevision{0};
    bool m_visibilityChanged{false};
    RHIBuffer* m_pReflectanceSums{nullptr};
    bool m_requestAveragedReflectance{false};
    bool m_useAveragedReflectance{false};
    uint64_t m_reflectanceBudgetBytes{0};
    uint32_t m_classMask{GI_ALL};
    uint64_t m_sceneRevision{0};
    uint64_t m_pendingSceneRevision{0};
    uint64_t m_surfaceRevision{0};
    uint64_t m_pendingSurfaceRevision{0};
    RHIBuffer* m_occupiedList{nullptr};
    RHIBuffer* m_gridToList{nullptr};
    RHIBuffer* m_occupiedCount{nullptr};
};
} // namespace zen::rc
