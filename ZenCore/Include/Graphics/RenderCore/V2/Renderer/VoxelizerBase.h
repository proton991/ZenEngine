#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Utils/UniquePtr.h"

namespace zen::rc
{
class RenderScene;
class RenderDevice;

struct VoxelTextures
{
    RHITexture* pAlbedo{nullptr};
    RHITextureView* pAlbedoView{nullptr};
    RHITexture* pNormal{nullptr};
    RHITextureView* pNormalView{nullptr};
    RHITexture* pEmissive{nullptr};
    RHITextureView* pEmissiveView{nullptr};
};

class VoxelizerBase
{
public:
    VoxelizerBase(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    virtual ~VoxelizerBase() = default;

    virtual void Init() = 0;

    virtual void BuildRenderGraph() = 0;

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
        return false;
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
    bool BeginVoxelization(RenderGraph& graph);

    virtual void PrepareTextures();

    virtual void PrepareBuffers() {}

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    VoxelTextures m_voxelTextures;

    RHISampler* m_pVoxelSampler;
    RHISampler* m_pColorSampler;

    uint32_t m_voxelTexResolution;
    uint32_t m_voxelCount;
    DataFormat m_voxelTexFormat;

    bool m_needVoxelization{true};
};
} // namespace zen::rc
