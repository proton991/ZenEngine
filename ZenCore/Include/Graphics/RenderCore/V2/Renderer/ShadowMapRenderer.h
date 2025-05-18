#pragma once
#include "Common/UniquePtr.h"
#include "Graphics/RenderCore/V2/RenderGraph.h"
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
        rhi::DataFormat shadowMapFormat;
        uint32_t shadowMapWidth;
        uint32_t shadowMapHeight;
        Vec2 exponents;
        float alphaCutoff;
    } m_config;

    struct
    {
        rhi::TextureHandle shadowMap;
        rhi::TextureHandle depth;
    } m_offscreenTextures;

    rhi::SamplerHandle m_colorSampler;

    UniquePtr<sg::Camera> m_lightView;
};
} // namespace zen::rc