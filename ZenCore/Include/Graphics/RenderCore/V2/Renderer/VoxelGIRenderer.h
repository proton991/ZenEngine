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
    VoxelGIRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void SetRenderScene(RenderScene* pScene);

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

    RenderDevice* m_pRenderDevice{nullptr};

    RenderScene* m_pScene{nullptr};

    // DynamicRHI* m_RHI{nullptr};

    RHIViewport* m_pViewport{nullptr};

    VoxelizerBase* m_pVoxelizer{nullptr};

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
        ComputePass* pResetVoxelTexture;
        ComputePass* pInjectRadiance;
        ComputePass* pInjectPropagation;
        ComputePass* pGenMipMapBase;
        ComputePass* pGenMipMapVolume;
    } m_computePasses;

    struct
    {
        RHITexture* pVoxelRadiance;
        RHITexture* pVoxelMipmaps[6];
        // from ShadowMapRenderer
        RHITexture* pShadowMap;
    } m_textures;
};
} // namespace zen::rc