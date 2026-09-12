#pragma once
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"

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

    RenderOption m_renderOption{RenderOption::eVoxelize};
};
} // namespace zen::rc
