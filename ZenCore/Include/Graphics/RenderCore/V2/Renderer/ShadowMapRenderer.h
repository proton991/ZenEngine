#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderResource.h"
#include "SceneGraph/Camera.h"

namespace zen::sg
{
class Camera;
}

namespace zen::rc
{
class RenderScene;
class RenderDevice;

class ShadowMapRenderer
{
public:
    ShadowMapRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void BuildRenderGraph();

    void Destroy();

    void SetRenderScene(RenderScene* pRenderScene);

    RHITexture* GetShadowMapTexture() const
    {
        return m_offscreenTextures.pShadowMap;
    }

    RHISampler* GetColorSampler() const
    {
        return m_pColorSampler;
    }

private:
    void PrepareTextures();

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    struct
    {
        DataFormat shadowMapFormat;
        uint32_t shadowMapWidth;
        uint32_t shadowMapHeight;
        Vec2 exponents;
        float alphaCutoff;
    } m_config;

    struct
    {
        RHITexture* pShadowMap{nullptr};
        RHITexture* pShadowMapRenderTarget{nullptr};
        RHITexture* pDepth{nullptr};
    } m_offscreenTextures;

    RHISampler* m_pColorSampler;
};
} // namespace zen::rc
