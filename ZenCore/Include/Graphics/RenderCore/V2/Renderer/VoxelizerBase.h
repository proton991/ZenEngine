#pragma once
// #include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "Utils/UniquePtr.h"

namespace zen::rc
{
class RenderScene;
class RenderDevice;

// 3D textures (written by voxelizer)
// struct VoxelTextures
// {
//     rhi::TextureHandle staticFlag;
//     rhi::TextureHandle albedo;
//     rhi::TextureHandle albedoProxy;
//     rhi::TextureHandle normal;
//     rhi::TextureHandle normalProxy;
//     rhi::TextureHandle emissive;
//     rhi::TextureHandle emissiveProxy;
// };

struct VoxelTextures
{
    rhi::RHITexture* staticFlag{nullptr};
    rhi::RHITexture* albedo{nullptr};
    rhi::RHITexture* albedoProxy{nullptr};
    rhi::RHITexture* normal{nullptr};
    rhi::RHITexture* normalProxy{nullptr};
    rhi::RHITexture* emissive{nullptr};
    rhi::RHITexture* emissiveProxy{nullptr};
};

class VoxelizerBase
{
public:
    VoxelizerBase(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    virtual ~VoxelizerBase() = default;

    virtual void Init() = 0;

    virtual void SetRenderScene(RenderScene* scene);

    virtual void Destroy();

    virtual void PrepareRenderWorkload() = 0;

    virtual void OnResize() = 0;

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

    const VoxelTextures& GetVoxelTextures() const
    {
        return m_voxelTextures;
    }

    rhi::RHISampler* GetVoxelSampler() const
    {
        return m_voxelSampler;
    }
    DataFormat GetVoxelTexFormat() const
    {
        return m_voxelTexFormat;
    }

    uint32_t GetVoxelTexResolution() const
    {
        return m_voxelTexResolution;
    }

    float GetVoxelSize() const
    {
        return m_voxelSize;
    }

    float GetVoxelScale() const
    {
        return m_voxelScale;
    }

    Vec3 GetSceneMinPoint() const;

protected:
    virtual void PrepareTextures();

    virtual void PrepareBuffers() {}

    virtual void BuildRenderGraph() = 0;

    virtual void BuildGraphicsPasses() {}

    virtual void BuildComputePasses() {}

    virtual void UpdatePassResources() = 0;

    virtual void UpdateUniformData() {};
    //
    // void VoxelizeStaticScene();
    //
    // void VoxelizeDynamicScene();

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    RenderScene* m_scene{nullptr};

    VoxelTextures m_voxelTextures;

    rhi::RHISampler* m_voxelSampler;
    rhi::RHISampler* m_colorSampler;

    uint32_t m_voxelTexResolution;
    float m_voxelSize;
    float m_voxelScale;
    uint32_t m_voxelCount;
    DataFormat m_voxelTexFormat;

    float m_sceneExtent;

    bool m_rebuildRDG{true};
    UniquePtr<RenderGraph> m_rdg;
    bool m_needVoxelization{true};
};
} // namespace zen::rc