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
class VoxelRenderer;
class ShadowMapRenderer;
class RenderScene;

class RendererServer
{
public:
    RendererServer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* scene);

    SceneRenderer* RequestSceneRenderer() const
    {
        return m_sceneRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_skyboxRenderer;
    }

    VoxelRenderer* RequestVoxelRenderer() const
    {
        return m_voxelRenderer;
    }

    ShadowMapRenderer* RequestShadowMapRenderer() const
    {
        return m_shadowMapRenderer;
    }


private:
    rhi::RHIViewport* m_viewport{nullptr};
    RenderDevice* m_renderDevice{nullptr};

    SceneRenderer* m_sceneRenderer{nullptr};
    SkyboxRenderer* m_skyboxRenderer{nullptr};
    VoxelRenderer* m_voxelRenderer{nullptr};
    ShadowMapRenderer* m_shadowMapRenderer{nullptr};
};
} // namespace zen::rc