#pragma once
#include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "Common/UniquePtr.h"

namespace zen::rc
{
class RenderScene;

struct SimpleVertex
{
    Vec4 position;
};

class VoxelRenderer
{
public:
    VoxelRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void SetRenderScene(RenderScene* scene)
    {
        m_scene = scene;
        UpdateUniformData();
        UpdateGraphicsPassResources();
    }

    void Destroy();

    void PrepareRenderWorkload();

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

    const rhi::TextureHandle& GetVoxelAlbedo() const
    {
        return m_voxelTextures.albedo;
    }

    void OnResize();

private:
    void PrepareTextures();

    void BuildRenderGraph();

    void BuildGraphicsPasses();

    void UpdateGraphicsPassResources();

    void UpdateUniformData();

    void VoxelizeStaticScene();

    void VoxelizeDynamicScene();

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
        rhi::TextureHandle radiance;
        rhi::TextureHandle mipmaps[6]; // per face
    } m_voxelTextures;

    rhi::SamplerHandle m_voxelSampler;
    rhi::SamplerHandle m_colorSampler;

    rhi::BufferHandle m_voxelVBO;

    struct
    {
        uint32_t volumeDimension;
        uint32_t voxelCount;
        bool injectFirstBounce;
        float volumeGridSize;
        float voxelSize;
        bool traceShadowCones;
        bool normalWeightedLambert;
        float traceShadowHit;
        uint32_t drawMipLevel;
        uint32_t drawDirection;
        glm::vec4 drawColorChannels;
        rhi::DataFormat voxelTexFormat;
    } m_config;

    struct
    {
        GraphicsPass voxelization;
        GraphicsPass voxelDraw;
    } m_gfxPasses;

    bool m_rebuildRDG{true};
    UniquePtr<RenderGraph> m_rdg;
    bool m_needVoxelization{true};
};
} // namespace zen::rc