#pragma once

namespace zen::rhi
{
class RHIViewport;
}

namespace zen::rc
{
class RenderDevice;
class SkyboxRenderer;
class SceneRenderer;

class RendererServer
{
public:
    RendererServer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    SceneRenderer* RequestSceneRenderer() const
    {
        return m_sceneRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_skyboxRenderer;
    }

private:
    rhi::RHIViewport* m_viewport{nullptr};
    RenderDevice* m_renderDevice{nullptr};

    SceneRenderer* m_sceneRenderer{nullptr};
    SkyboxRenderer* m_skyboxRenderer{nullptr};
};
} // namespace zen::rc