#pragma once
#include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "Common/UniquePtr.h"

namespace zen::rc
{
class RenderScene;

class VoxelizerBase
{
public:
    virtual ~VoxelizerBase() = default;

    VoxelizerBase(RenderDevice* renderDevice, rhi::RHIViewport* viewport) :
        m_renderDevice(renderDevice), m_viewport(viewport), m_config()
    {
        m_RHI = m_renderDevice->GetRHI();
    }

    virtual void Init() = 0;

    virtual void SetRenderScene(RenderScene* scene);

    virtual void Destroy() = 0;

    virtual void PrepareRenderWorkload() = 0;

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

    const rhi::TextureHandle& GetVoxelAlbedo() const
    {
        return m_voxelTextures.albedo;
    }

    virtual void OnResize() = 0;

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

    rhi::DynamicRHI* m_RHI{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    RenderScene* m_scene{nullptr};

    // 3D textures (written by voxelizer)
    struct
    {
        rhi::TextureHandle offscreen1;
        rhi::TextureHandle offscreen2;
        rhi::TextureHandle staticFlag;
        rhi::TextureHandle albedo;
        rhi::TextureHandle albedoProxy;
        rhi::TextureHandle normal;
        rhi::TextureHandle normalProxy;
        rhi::TextureHandle emissive;
        rhi::TextureHandle emissiveProxy;
        rhi::TextureHandle radiance;
        rhi::TextureHandle mipmaps[6]; // per face
    } m_voxelTextures;

    rhi::SamplerHandle m_voxelSampler;
    rhi::SamplerHandle m_colorSampler;
    struct
    {
        bool injectFirstBounce;
        bool traceShadowCones;
        bool normalWeightedLambert;
        float traceShadowHit;
        uint32_t drawMipLevel;
        uint32_t drawDirection;
        glm::vec4 drawColorChannels;
    } m_config;

    uint32_t m_voxelTexResolution;
    float m_voxelSize;
    uint32_t m_voxelCount;
    rhi::DataFormat m_voxelTexFormat;

    float m_sceneExtent;

    bool m_rebuildRDG{true};
    UniquePtr<RenderGraph> m_rdg;
    bool m_needVoxelization{true};
};
} // namespace zen::rc