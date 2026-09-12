#pragma once
#include "Utils/UniquePtr.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"

namespace zen::sys
{
class Camera;
}

namespace zen::asset
{
struct Vertex;
using Index = uint32_t;
} // namespace zen::asset

namespace zen::rc
{
class RenderScene;
class RenderDevice;
class SkyboxRenderer;

class DeferredLightingRenderer
{
public:
    enum class RenderFlags
    {
        eBindTextures = 1 << 0
    };

    DeferredLightingRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void BuildRenderGraph();

    void Destroy();

    void SetRenderScene(RenderScene* pRenderScene)
    {
        m_pScene = pRenderScene;
    }

private:
    void PrepareSamplers();

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    RHISampler* m_pColorSampler;
    RHISampler* m_pDepthSampler;
};
} // namespace zen::rc
