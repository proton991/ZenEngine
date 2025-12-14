#pragma once
#include "../RenderDevice.h"
#include "../RenderGraph.h"
#include "Utils/UniquePtr.h"

namespace zen::rc
{
class VoxelizerBase;
class RenderScene;

class VoxelGIRenderer
{
public:
    VoxelGIRenderer(RenderDevice* renderDevice, RHIViewport* viewport);

    void Init();

    void SetRenderScene(RenderScene* scene);

    void Destroy();

    void PrepareRenderWorkload();

    void OnResize();

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

private:
    void PrepareTextures();

    void PrepareBuffers();

    void BuildRenderGraph();

    void BuildGraphicsPasses();

    void BuildComputePasses();

    void UpdatePassResources();

    void UpdateUniformData();

    RenderDevice* m_renderDevice{nullptr};

    RenderScene* m_scene{nullptr};

    // DynamicRHI* m_RHI{nullptr};

    RHIViewport* m_viewport{nullptr};

    VoxelizerBase* m_voxelizer{nullptr};

    bool m_rebuildRDG{true};
    UniquePtr<RenderGraph> m_rdg;

    struct
    {
        bool injectFirstBounce;
        bool traceShadowCones;
        bool normalWeightedLambert;
        float traceShadowHit;
        // uint32_t drawMipLevel;
        // uint32_t drawDirection;
        // glm::vec4 drawColorChannels;
    } m_config{};

    struct
    {
    } m_gfxPasses;

    struct
    {
        ComputePass* resetVoxelTexture;
        ComputePass* injectRadiance;
        ComputePass* injectPropagation;
        ComputePass* genMipMapBase;
        ComputePass* genMipMapVolume;
    } m_computePasses;

    struct
    {
        RHITexture* voxelRadiance;
        RHITexture* voxelMipmaps[6];
        // from ShadowMapRenderer
        RHITexture* shadowMap;
    } m_textures;
};
} // namespace zen::rc