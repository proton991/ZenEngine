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
    ShadowMapRenderer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* renderScene);

    void PrepareRenderWorkload();

    RenderGraph* GetRenderGraph() const
    {
        return m_rdg.Get();
    };

    rhi::RHITexture* GetShadowMapTexture() const
    {
        return m_offscreenTextures.shadowMap;
    }

    rhi::RHISampler* GetColorSampler() const
    {
        return m_colorSampler;
    }

private:
    void PrepareTextures();

    void BuildGraphicsPasses();

    void BuildRenderGraph();

    void UpdateGraphicsPassResources();

    void UpdateUniformData();

    RenderDevice* m_renderDevice{nullptr};

    rhi::RHIViewport* m_viewport{nullptr};

    RenderScene* m_scene{nullptr};

    UniquePtr<RenderGraph> m_rdg;
    bool m_rebuildRDG{true};

    struct GraphicsPasses
    {
        GraphicsPass evsm;
        GraphicsPass blurShadowMap;
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
        rhi::RHITexture* shadowMap{nullptr};
        rhi::RHITexture* depth{nullptr};
    } m_offscreenTextures;

    rhi::RHISampler* m_colorSampler;

    UniquePtr<sg::Camera> m_lightView;
};
} // namespace zen::rc