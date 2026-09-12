#pragma once
#include "../RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Utils/UniquePtr.h"

namespace zen::rc
{
class VoxelizerBase;
class RenderScene;
class ShadowMapRenderer;

class VoxelGIRenderer
{
public:
    // Kept for a future radiance consumer; the albedo renderer server does not create GI.
    VoxelGIRenderer(RenderDevice* pRenderDevice,
                    RHIViewport* pViewport,
                    VoxelizerBase* pVoxelizer,
                    ShadowMapRenderer* pShadowMapRenderer);

    void Init();

    void BuildRenderGraph();

    void SetRenderScene(RenderScene* pScene);

    void Destroy();

private:
    void PrepareTextures();

    RenderDevice* m_pRenderDevice{nullptr};

    RenderScene* m_pScene{nullptr};

    RHIViewport* m_pViewport{nullptr};

    VoxelizerBase* m_pVoxelizer{nullptr};
    ShadowMapRenderer* m_pShadowMapRenderer{nullptr};
    bool m_initialized{false};

    struct
    {
        bool injectFirstBounce;
        bool traceShadowCones;
        bool normalWeightedLambert;
        float traceShadowHit;
    } m_config{};

    struct
    {
        RHITexture* pVoxelRadiance;
    } m_textures{};
};
} // namespace zen::rc
