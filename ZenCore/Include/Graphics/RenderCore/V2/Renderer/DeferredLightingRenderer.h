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
    void BuildGBufferGraph();
    void BuildCompositionGraph(VoxelGIRenderer* voxelGI     = nullptr,
                               SceneShadowRenderer* shadows = nullptr);

    void Destroy();

    bool SetLightMarkers(bool enabled, float size);

    bool GetLightMarkersEnabled() const
    {
        return m_lightMarkersEnabled;
    }

    float GetLightMarkerSize() const
    {
        return m_lightMarkerSize;
    }

    // Opt-in diagnostic buffers owned by the caller through GPU completion.
    void SetLightingCapture(RHIBuffer* output, RHIBuffer* readback)
    {
        m_captureOutput = output;

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

    float m_lightMarkerSize{0.02f};
    bool m_lightMarkersEnabled{false};

    RenderDevice* m_pRenderDevice{nullptr};

    RHIViewport* m_pViewport{nullptr};

    RenderScene* m_pScene{nullptr};

    RHISampler* m_pColorSampler;
    RHISampler* m_pDepthSampler;
    RHIBuffer* m_captureOutput{nullptr};
    RHIBuffer* m_captureReadback{nullptr};
    bool m_captureRecorded{false};
};
} // namespace zen::rc
