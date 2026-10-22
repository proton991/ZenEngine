#pragma once
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Utils/UniquePtr.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"

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
class HybridGIRenderer;

class DeferredLightingRenderer
{
public:
    enum class RenderFlags
    {
        eBindTextures = 1 << 0
    };

    explicit DeferredLightingRenderer(RenderDevice* pRenderDevice);

    void Init();

    void BuildRenderGraph(const RenderView& view, VoxelGIRenderer* voxelGI = nullptr, SceneShadowRenderer* shadows = nullptr);

    void              BuildGBufferGraph(const RenderView& view, bool hybrid = false);
    void              OnRenderGraphExecuted(RenderFrameResult result);
    HybridGIRenderer* GetHybridGI() const
    {
        return m_hybrid;
    }
    void SetHybridCapture(RHIBuffer* output, RHIBuffer* readback)
    {
        m_hybridCaptureOutput   = output;
        m_hybridCaptureReadback = readback;
    }

    DebugOutputDescription BuildDebugView(const RenderView& view, const DebugSelection& selection);

    void BuildCompositionGraph(const RenderView&    view,
                               VoxelGIRenderer*     voxelGI = nullptr,
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
        m_captureOutput   = output;

        m_captureReadback = readback;

        m_captureRecorded = false;
    }

    bool WasLightingCaptureRecorded() const
    {
        return m_captureRecorded;
    }

    // The G-buffer is sized to the supplied view. Zero when the last build declared none: forward
    // materials or a suspended (zero-sized) view.
    glm::uvec2 GetGBufferExtent() const
    {
        return m_gbufferExtent;
    }

    void SetRenderScene(RenderScene* pRenderScene);

private:
    void PrepareSamplers();

    bool UsesForwardMaterials() const;

    // Whether the G-buffer can shade every opaque material, leaving only translucent layers and
    // non-triangle draws to the forward renderer.
    bool ShadesOpaqueDeferred() const;

    void BuildForwardGraph(const RenderView& view, VoxelGIRenderer* voxelGI, SceneShadowRenderer* shadows);

    // Fullscreen lighting of the G-buffer receivers. Without a linear target it tone-maps into the
    // view and owns the view's depth. With one it writes linear HDR over the forward background's
    // prefilled depth, so translucent layers compose over it before the forward tone map.
    void AddLightingPass(const RenderView&    view,
                         VoxelGIRenderer*     voxelGI,
                         SceneShadowRenderer* shadows,
                         bool                 capture,
                         bool                 captureValid,
                         const RDGTexture*    linearTarget);

    void BuildLightMarkers(const RenderView& view);

    bool BuildLightingCaptureClear(const RenderView& view);

    float m_lightMarkerSize{0.02f};
    bool  m_lightMarkersEnabled{false};

    struct HybridView
    {
        uint64_t          id;
        HybridGIRenderer* renderer;
    };
    HeapVector<HybridView> m_hybridViews;
    HybridGIRenderer*      m_hybrid{nullptr};
    RHIBuffer*             m_hybridCaptureOutput{nullptr};
    RHIBuffer*             m_hybridCaptureReadback{nullptr};
    RenderDevice*          m_pRenderDevice{nullptr};

    RenderScene* m_pScene{nullptr};

    RHISampler* m_pColorSampler;
    RHISampler* m_pDepthSampler;
    RHISampler* m_pTransmissionSampler{nullptr};
    RHIBuffer*  m_captureOutput{nullptr};
    RHIBuffer*  m_captureReadback{nullptr};
    bool        m_captureRecorded{false};
    glm::uvec2  m_gbufferExtent{0, 0};
};
} // namespace zen::rc
