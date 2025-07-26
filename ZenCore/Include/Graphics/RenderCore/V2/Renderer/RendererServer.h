#pragma once
#include <cstdint>

namespace zen::rhi
{
class RHIViewport;
}

namespace zen::rc
{
class RenderDevice;
class SkyboxRenderer;
class DeferredLightingRenderer;
class VoxelRenderer;
class VoxelizerBase;
class ShadowMapRenderer;
class RenderScene;

enum class RenderOption : uint32_t
{
    eVoxelize = 0,
    ePBR      = 1,
    eMax      = 2
};

class RendererServer
{
public:
    RendererServer(RenderDevice* renderDevice, rhi::RHIViewport* viewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* scene);

    void DispatchRenderWorkloads();

    void ViewportResizeCallback();

    DeferredLightingRenderer* RequestDeferredLightingRenderer() const
    {
        return m_deferredLightingRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_skyboxRenderer;
    }

    // VoxelRenderer* RequestVoxelRenderer() const
    // {
    //     return m_voxelRenderer;
    // }

    ShadowMapRenderer* RequestShadowMapRenderer() const
    {
        return m_shadowMapRenderer;
    }

    void SetRenderOption(RenderOption option)
    {
        m_renderOption = option;
    }

private:
    rhi::RHIViewport* m_viewport{nullptr};
    RenderDevice* m_renderDevice{nullptr};
    RenderScene* m_scene{nullptr};

    DeferredLightingRenderer* m_deferredLightingRenderer{nullptr};
    SkyboxRenderer* m_skyboxRenderer{nullptr};
    VoxelizerBase* m_voxelizer{nullptr};
    // VoxelRenderer* m_voxelRenderer{nullptr};
    ShadowMapRenderer* m_shadowMapRenderer{nullptr};

    RenderOption m_renderOption{RenderOption::eVoxelize};
};
} // namespace zen::rc