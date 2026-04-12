#pragma once
#include "Graphics/RenderCore/V2/RenderGraph.h"
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

    void Destroy();

    void SetRenderScene(RenderScene* pRenderScene);

    void PrepareRenderWorkload();

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

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

    void BuildGraphicsPasses();

    void BuildRenderGraph();

    void UpdateGraphicsPassResources();

    void UpdateUniformData();

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass* pEvsm;
        GraphicsPass* pBlurShadowMap;
    } m_gfxPasses;

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
        RHITexture* pDepth{nullptr};
    } m_offscreenTextures;

    RHISampler* m_pColorSampler;

    UniquePtr<sg::Camera> m_lightView;
};
} // namespace zen::rc