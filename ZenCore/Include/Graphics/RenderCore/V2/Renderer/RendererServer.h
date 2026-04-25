#pragma once
#include "Templates/HeapVector.h"

namespace zen
{
class RHIViewport;
}

namespace zen::rc
{
class RenderDevice;
class SkyboxRenderer;
class DeferredLightingRenderer;
class VoxelizerBase;
class ShadowMapRenderer;
class VoxelGIRenderer;
class RenderScene;
class RenderGraph;

enum class RenderOption : uint32_t
{
    eVoxelize = 0,
    ePBR      = 1,
    eMax      = 2
};

class RendererServer
{
public:
    RendererServer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void Destroy();

    void SetRenderScene(RenderScene* pScene);

    void DispatchRenderWorkloads();

    void ViewportResizeCallback();

    DeferredLightingRenderer* RequestDeferredLightingRenderer() const
    {
        return m_pDeferredLightingRenderer;
    }

    SkyboxRenderer* RequestSkyboxRenderer() const
    {
        return m_pSkyboxRenderer;
    }

    VoxelizerBase* RequestVoxelizer() const
    {
        return m_pVoxelizer;
    }

    ShadowMapRenderer* RequestShadowMapRenderer() const
    {
        return m_pShadowMapRenderer;
    }

    void SetRenderOption(RenderOption option)
    {
        m_renderOption = option;
    }

private:
    RHIViewport* m_pViewport{nullptr};
    RenderDevice* m_pRenderDevice{nullptr};
    RenderScene* m_pScene{nullptr};

    DeferredLightingRenderer* m_pDeferredLightingRenderer{nullptr};
    SkyboxRenderer* m_pSkyboxRenderer{nullptr};
    VoxelizerBase* m_pVoxelizer{nullptr};
    // VoxelRenderer* m_voxelRenderer{nullptr};
    ShadowMapRenderer* m_pShadowMapRenderer{nullptr};
    VoxelGIRenderer* m_pVoxelGIRenderer{nullptr};

    RenderOption m_renderOption{RenderOption::eVoxelize};
    HeapVector<RenderGraph*> m_frameRDGs;
};
} // namespace zen::rc
