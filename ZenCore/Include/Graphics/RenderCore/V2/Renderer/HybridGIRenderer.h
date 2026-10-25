#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/Renderer/GBuffer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"
#include "Graphics/RenderCore/V2/Renderer/ScreenProbeGather.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
struct SceneMeshDraw;

struct HybridReceiverDraw
{
    glm::uvec4 indices{0};
    Mat4       previousModel{1.0f};
};
static_assert(sizeof(HybridReceiverDraw) == 80);

struct HybridGIUniformData
{
    GBufferUniformData reconstruction;
    Mat4               previousProjectionView{1.0f};
    Vec4               previousViewPosition{0};
    Vec4               previousViewDirection{0, 0, 1, 0};
    glm::uvec4         sampling{0};   // frame, sample count, history limit, history valid
    glm::uvec4         provider{0};   // hardware trace variant, guide valid, hardware feature mask, adaptive sampling
    Vec4               rejection{0};  // plane threshold, specular enabled, temporal enabled, static reference
    glm::uvec4         bounce{0};     // ray bounce enabled, bounce history valid, responsive reconstruction, cache changed
    glm::uvec4         reflection{0}; // reflections enabled, history valid, responsive reconstruction, cache changed
};
static_assert(sizeof(HybridGIUniformData) == 256);

void BindHybridReceiverGuides(RDGPassDescBase& pass, RHISampler* sampler, const HybridGIUniformData& uniforms);

// One instance owns histories for one view. Imported textures use RenderDevice's
// submission history (including all outstanding readers), not CPU frame parity.
class HybridGIRenderer
{
public:
    explicit HybridGIRenderer(RenderDevice* device);
    void                   Prepare(const RenderView& view, RenderScene& scene);
    void                   BindReceiverInputs(RDGPassDescBase& pass) const;
    HybridReceiverDraw     ReceiverDraw(uint32_t node, uint32_t material) const;
    bool                   BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI);
    void                   BindLightingInputs(RDGPassDescBase& pass) const;
    DebugOutputDescription BuildDebugView(const RenderView& view, const DebugSelection& selection) const;
    void                   OnRenderGraphExecuted(RenderFrameResult result);
    void                   InvalidateHistory(const char* reason);
    // Set between recordings. Only the learned pixel-gather guide is affected.
    void SetDeterministicGuideCapture(bool enabled)
    {
        if (m_deterministicGuideCapture != enabled)
        {
            m_deterministicGuideCapture = enabled;
            InvalidateHistory("guide_accumulation");
        }
    }
    void Destroy();
    void SetCapture(RHIBuffer* output, RHIBuffer* readback)
    {
        m_captureOutput   = output;
        m_captureReadback = readback;
    }
    // Diagnostic variants only. The caller retains both buffers through GPU completion.
    static uint64_t GetRayCountCaptureBytes(uint32_t width, uint32_t height);

    // Whether environment light can reach the sky term. Without it the final filter queues no
    // receiver for the dense sky search, which could only confirm a zero sky.
    static bool HasSkyLight(const VoxelGISettings& settings, const SceneUniformData& scene);
    void        SetRayCountCapture(RHIBuffer* output, RHIBuffer* readback)
    {
        m_rayCountOutput    = output;
        m_rayCountReadback  = readback;
        m_rayCountsRecorded = false;
    }
    bool WereRayCountsRecorded() const
    {
        return m_rayCountsRecorded;
    }

    bool IsActive() const
    {
        return m_recorded;
    }
    bool IsHistoryValid() const
    {
        return m_valid;
    }
    // Whether the last built frame traced diffuse rays at half resolution.
    bool UsesHalfResolutionDiffuse() const
    {
        return m_halfDiffuse;
    }
    bool UsesScreenProbes() const
    {
        return m_screenProbes;
    }
    // Screen-probe counters of the last frame recorded with a lighting capture.
    ScreenProbeStatistics GetScreenProbeStatistics() const
    {
        return m_probeGather.ReadStatistics();
    }
    uint32_t GetFrame() const
    {
        return m_frame;
    }
    const char* GetResetReason() const
    {
        return m_resetReason;
    }

private:
    bool PrepareHistory(uint32_t width,
                        uint32_t height,
                        bool     bounce,
                        bool     reflections,
                        bool     search,
                        bool     reference,
                        bool     budget);
    void BindGuides(RDGPassDescBase& pass) const;
    void BindTraceInputs(RDGComputePassDesc& pass, VoxelGIRenderer& voxelGI, const glm::uvec4& reuseData) const;
    void Dispatch(RDGComputePassDesc&& pass);
    void DestroyPixelHistory();
    // A brick trace pass's diffuse rays to re-trace against triangles (hybrid_retrace.glsl).
    struct RetraceQueue
    {
        RDGBuffer header;
        RDGBuffer rays;
        RDGBuffer receivers;
    };
    // Resets a queue of up to the given rays and binds it to the trace pass that fills it.
    RetraceQueue AddRetraceQueue(RDGComputePassDesc& trace, const char* name, uint64_t capacity) const;
    // Re-traces a queue after its trace pass and adds the results to the images that pass wrote;
    // with counts, its rays are added to the ray-count capture.
    void AddRetracePass(const RetraceQueue& queue,
                        VoxelGIRenderer&    voxelGI,
                        const char*         tag,
                        bool                counts,
                        RDGTexture          sky,
                        RDGTexture          bounce,
                        RDGTexture          previous) const;

    enum HistoryTexture : uint32_t
    {
        Sky,
        Specular,
        Moments,
        Length,
        Position,
        Normal,
        Receiver,
        Bounce,
        BounceMoments,
        Reflection,   // Hit distance, luminance moments, history length; radiance is in Specular.rgb.
        SearchWeight, // Geometry-only dense sky-filter denominator; zero means not cached.
        Budget,       // Nonzero where the trace used the reduced settled ray budget.
        Count
    };
    RenderDevice*     m_device;
    ScreenProbeGather m_probeGather;
    RenderScene*      m_scene{nullptr};
    RHISampler*       m_sampler{nullptr};
    RHIBuffer*        m_captureOutput{nullptr};
    RHIBuffer*        m_captureReadback{nullptr};
    RHIBuffer*        m_rayCountOutput{nullptr};
    RHIBuffer*        m_rayCountReadback{nullptr};
    bool              m_rayCountsRecorded{false};
    RHITexture*       m_history[2][Count]{};
    bool              m_historyNeedsClear[2][Count]{};
    // Visibility guide per kGuideTile x kGuideTile screen tile, previous and next (hybrid_trace.glsl).
    RHIBuffer*       m_guide[2]{};
    RHIBuffer*       m_retraceHeader{nullptr}; // Host-written start of every re-trace queue's header.
    RDGTexture       m_outputSky, m_outputSpecular, m_outputBounce;
    RDGTexture       m_rawSky, m_rawSpecular;
    HeapVector<Mat4> m_previousModels;
    HeapVector<Mat4> m_recordedModels;
    // A retained buffer does not freeze its contents. Check the slot generation
    // before using a view's previous vertices after skipped/declined frames.
    RHIResourcePtr<RHIBuffer> m_previousVertices;
    RHIResourcePtr<RHIBuffer> m_recordedVertices;
    uint64_t                  m_previousVertexGeneration{0};
    uint64_t                  m_recordedVertexGeneration{0};
    Mat4                      m_previousProjection{1};
    Mat4                      m_recordedProjection{1};
    Mat4                      m_recordedProjectionView{1};
    Vec4                      m_recordedViewPosition{0};
    Vec4                      m_recordedViewDirection{0, 0, 1, 0};
    HybridGIUniformData       m_uniforms;
    VoxelGISettings           m_previousSettings;
    VoxelGISettings           m_recordedSettings;
    uint32_t                  m_previousHardwareMask{0};
    bool                      m_previousBrickTraversal{false};
    bool                      m_recordedBrickTraversal{false};
    bool                      m_previousRayBounce{false};
    bool                      m_halfDiffuse{false};
    bool                      m_screenProbes{false};
    bool                      m_previousScreenProbes{false};
    bool                      m_previousReflections{false};
    bool                      m_deterministicGuideCapture{false};
    glm::uvec2                m_extent{0};
    uint64_t                  m_geometry{0}, m_environment{0}, m_surface{0};
    uint64_t                  m_radianceGeneration{0}, m_recordedRadianceGeneration{0};
    uint32_t                  m_bounceResponsiveFrames{0}; // Shared responsive window for both hit-radiance signals.
    uint32_t                  m_frame{0}, m_current{0};
    bool                      m_valid{false}, m_prepared{false}, m_recorded{false};
    const char*               m_resetReason{"initial"};
    // Recording may invalidate history for changed settings without consuming it.
    bool        m_preparedHistoryValid{false};
    const char* m_preparedResetReason{"initial"};
    uint32_t    m_preparedResponsiveFrames{0};
};
} // namespace zen::rc
