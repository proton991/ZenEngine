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
class VoxelGIRenderer;
class DynamicVoxelGIRenderer;
class SceneShadowRenderer;

class DeferredLightingRenderer
{
public:
    enum class RenderFlags
    {
        eBindTextures = 1 << 0
    };

    DeferredLightingRenderer(RenderDevice* pRenderDevice, RHIViewport* pViewport);

    void Init();

    void BuildRenderGraph(VoxelGIRenderer* voxelGI     = nullptr,
                          SceneShadowRenderer* shadows = nullptr);
    void BuildGBufferGraph(bool dynamicGI = false);
    void BuildCompositionGraph(VoxelGIRenderer* voxelGI          = nullptr,
                               SceneShadowRenderer* shadows      = nullptr,
                               DynamicVoxelGIRenderer* dynamicGI = nullptr);

    void Destroy();

    // Opt-in diagnostic buffers owned by the caller through GPU completion.
    void SetLightingCapture(RHIBuffer* output,
                            RHIBuffer* readback,
                            RHIBuffer* surfaceOutput   = nullptr,
                            RHIBuffer* surfaceReadback = nullptr)
    {
        m_surfaceOutput   = surfaceOutput;
        m_surfaceReadback = surfaceReadback;
        m_captureOutput   = output;
        m_captureReadback = readback;
        m_captureRecorded = false;
    }

    bool WasLightingCaptureRecorded() const
    {
        return m_captureRecorded;
    }

    void SetRenderScene(RenderScene* pRenderScene)
    {
        m_pScene = pRenderScene;
    }

private:
    void PrepareSamplers();
    void BuildLightMarkers();
    bool BuildLightingCaptureClear();

    float m_lightMarkerSize{0.0f};

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    RHISampler* m_pColorSampler;
    RHISampler* m_pDepthSampler;
    RHIBuffer* m_captureOutput{nullptr};
    RHIBuffer* m_captureReadback{nullptr};
    bool m_captureRecorded{false};
    RHIBuffer* m_surfaceOutput{nullptr};
    RHIBuffer* m_surfaceReadback{nullptr};
};
} // namespace zen::rc
