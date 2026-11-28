#pragma once
#include "Math/Math.h"
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/Renderer/GBuffer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/WorldRadianceCache.h"
#include "Graphics/RenderCore/V2/Renderer/ScreenProbeGI.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"

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
    glm::uvec4         provider{0};   // hardware trace variant, guide valid, hardware feature mask, reference tile stride
    Vec4               rejection{0};  // plane threshold, specular enabled, temporal enabled, static reference
    glm::uvec4         bounce{0};     // ray bounce enabled, bounce history valid, responsive reconstruction, cache changed
    glm::uvec4         reflection{0}; // reflections enabled, history valid, responsive reconstruction, cache changed
    glm::uvec4         resolution{0}; // output width, output height, pixel divisor, unchanged receivers
    glm::uvec4         reservoir{
        0}; // reuse bits, diffuse candidate-frame cap, specular candidate-frame cap, denoiser (0: reference mean)
};
static_assert(sizeof(HybridGIUniformData) == 288);

// uReSTIRData (restir_target.glsl).
struct ReSTIRUniformData
{
    glm::uvec4 options{0};          // spatial reuse, reflection reservoirs, diffuse reservoirs, spatial parity stage
    glm::uvec4 spatial{4, 1, 0, 0}; // neighbors per signal (1..4), visibility rays for the pairwise MIS weights
};
static_assert(sizeof(ReSTIRUniformData) == 32);

void SetHybridRayTracingGroups(RDGRayTracingPassDesc& pass, uint32_t maxDepth);
void BindHybridReceiverGuides(RDGPassDescBase& pass, RHISampler* sampler, const HybridGIUniformData& uniforms);

struct GICacheUniformData
{
    glm::uvec4 limits{0};
    Vec4       camera{0};
    Vec4       previousCamera{0};
    Vec4       grid{0.01f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(GICacheUniformData) == 64);

// One instance owns histories for one view. Imported textures use RenderDevice's
// submission history (including all outstanding readers), not CPU frame parity.
class HybridGIRenderer
{
public:
    explicit HybridGIRenderer(RenderDevice* device);
    void                   Prepare(const RenderView& view, RenderScene& scene);
    void                   BindReceiverInputs(RDGPassDescBase& pass) const;
    HybridReceiverDraw     ReceiverDraw(uint32_t node, uint32_t material) const;
    Mat4                   PreviousModel(uint32_t node) const;
    bool                   BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI);
    void                   BindLightingInputs(RDGPassDescBase& pass) const;
    void                   BindForwardLightingInputs(RDGPassDescBase& pass) const;
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
        m_captureRecorded = false;
    }
    // Whether the last submitted frame wrote and read back the hybrid capture.
    bool WasCaptureRecorded() const
    {
        return m_captureRecorded;
    }
    // Diagnostic variants only. The caller retains both buffers through GPU completion.
    static uint64_t GetRayCountCaptureBytes(uint32_t width, uint32_t height);

    // Whether environment light can reach the sky term. Without it the sky signal and its
    // histories shrink to placeholders and the denoiser uses its no-sky variants.
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

    glm::uvec2 GetGIExtent() const
    {
        return m_extent;
    }
    uint32_t GetRadianceCacheCapacity() const
    {
        return m_cacheCapacity;
    }
    uint32_t GetNativeProbeRaySlots() const
    {
        return m_nativeProbeRaySlots;
    }
    // The next frame copies the new-probe seed counts, which ReadProbeSeedCounts returns once it is submitted.
    void RequestProbeSeedCounts()
    {
        m_worldCache.RequestSeedCounts();
    }
    glm::uvec4 ReadProbeSeedCounts() const
    {
        return m_worldCache.ReadSeedCounts();
    }
    RDGTexture GetBounceOutput() const
    {
        return m_outputBounce;
    }
    RDGTexture GetRawBounceOutput() const
    {
        return m_rawBounce;
    }
    bool HasMovingLightHistory() const
    {
        return m_worldCache.HasMovingLights();
    }
    uint32_t GetWorldCacheCapacity() const
    {
        return m_worldCache.GetCapacity();
    }
    bool InitializeScreenProbeHistories()
    {
        return m_screenProbes.InitializeHistories();
    }
    uint32_t GetScreenProbeCount() const
    {
        return m_probePath ? m_screenProbes.GetProbeCount() : 0;
    }
    bool IsActive() const
    {
        return m_recorded;
    }
    bool IsHistoryValid() const
    {
        return m_valid;
    }
    uint32_t GetFrame() const
    {
        return m_frame;
    }
    const char* GetResetReason() const
    {
        return m_resetReason;
    }
    // Why the last BuildRenderGraph recorded no GI, or nullptr when it recorded.
    const char* GetFailureReason() const
    {
        return m_failureReason;
    }

private:
    WorldRadianceCache m_worldCache;
    ScreenProbeGI      m_screenProbes;
    bool               m_probePath{false};
    void               RecordNativeProbeBenchmark(VoxelGIRenderer& voxelGI);
    uint32_t           m_nativeProbeRaySlots{0};
    void               RecordRadianceCache(VoxelGIRenderer& voxelGI, bool enabled, bool noGuide);
    void               BindRadianceCache(RDGPassDescBase& pass) const;
    void               DestroyRadianceCache();
    void               DestroyPixelHistory();
    GICacheUniformData m_cacheUniforms;
    RHIBuffer*         m_cacheBuffers[3]{};
    uint32_t           m_cacheCapacity{0};
    bool               m_cacheNeedsClear{true};
    bool               m_cacheRequested{false};
    bool               PrepareHistory(uint32_t width,
                                      uint32_t height,
                                      bool     reflections,
                                      bool     reference,
                                      uint32_t restirMask,
                                      bool     relax,
                                      bool     skyLight,
                                      bool     guideEnabled,
                                      bool     antiLag);
    void               BindGuides(RDGPassDescBase& pass) const;
    void               BindTraceInputs(RDGDispatchPassDesc& pass,
                                       VoxelGIRenderer&     voxelGI,
                                       const glm::uvec4&    reuseData,
                                       bool                 worldField) const;
    void               PrepareGuides();
    // One ReSTIR resampling dispatch: signal 2 is temporal reuse, 0 and 1 are diffuse and reflection spatial reuse
    // at checkerboard stage `stage` (1 and 2; one parity-free stage without spatial reuse).
    void       RecordReSTIRResample(VoxelGIRenderer&         voxelGI,
                                    const ReSTIRUniformData& restirData,
                                    uint32_t                 stage,
                                    uint32_t                 signal,
                                    RDGBuffer                receiverGuides,
                                    RDGTexture               rawBounce,
                                    RDGTexture               rawSpecular,
                                    RDGTexture               rawDiffuseDistance,
                                    RDGTexture               rawReflection);
    void       RecordDenoiser(RDGTexture rawSky,
                              RDGTexture rawBounce,
                              RDGTexture rawSpecular,
                              RDGTexture rawReflection,
                              RDGTexture rawDiffuseDistance);
    void       ReconstructOutput(VoxelGIRenderer& voxelGI);
    bool       PrepareOutputHistory(bool enabled, bool skyLight);
    void       StabilizeOutput();
    void       Dispatch(RDGComputePassDesc&& pass, bool fullResolution = false);
    RDGTexture m_receiverGuides[6];

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
        BounceMoments, // Reference diagnostic only.
        Reflection,    // Hit distance, luminance moments, history length; radiance is in Specular.rgb.
        FastSky,
        FastBounce,
        FastSpecular,
        ResponseAge,
        Count
    };
    glm::uvec2    m_outputExtent{0};
    RenderDevice* m_device;
    RenderScene*  m_scene{nullptr};
    RHISampler*   m_sampler{nullptr};
    RDGTexture    m_denoiserValidation[2];
    RHIBuffer*    m_captureOutput{nullptr};
    RHIBuffer*    m_captureReadback{nullptr};
    bool          m_captureRecorded{false};
    RHIBuffer*    m_rayCountOutput{nullptr};
    RHIBuffer*    m_rayCountReadback{nullptr};
    bool          m_rayCountsRecorded{false};
    RHITexture*   m_history[2][Count]{};
    bool          m_historyNeedsClear[2][Count]{};
    enum OutputHistoryTexture : uint32_t
    {
        OutputSky,
        OutputSpecular,
        OutputBounce,
        OutputPosition,
        OutputNormal,
        OutputSurface,
        OutputAge,
        OutputCount
    };
    RHITexture* m_outputHistory[2][OutputCount]{};
    bool        m_outputHistoryValid{false};
    bool        m_stabilizeOutput{false};
    // Visibility guide per kGuideTile x kGuideTile screen tile, previous and next (hybrid_trace.glsl).
    RHIBuffer*  m_guide[2]{};
    RHIBuffer*  m_reservoir[2][2]{}; // Frame side, then diffuse / rough specular; 24 bytes per pixel.
    RHITexture* m_restirLobeIntegral{nullptr};
    bool        m_restirLobeInitialized{false};
    RDGTexture  m_outputSky, m_outputSpecular, m_outputBounce;
    RDGTexture  m_rawSky, m_rawSpecular, m_rawBounce;
    RDGTexture  m_historyFixed[3];
    // Full-resolution receivers the reduced-resolution reconstruction could not interpolate.
    RDGTexture       m_missingReceiver;
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
    uint64_t                  m_previousVisibility{0}; // VoxelGIRenderer::GetVisibilityGeneration of the history.
    uint64_t                  m_recordedVisibility{0};
    bool                      m_previousReflections{false};
    bool                      m_deterministicGuideCapture{false};
    bool                      m_previousGuideWritten{false};
    bool                      m_recordedGuideWritten{false};
    glm::uvec2                m_extent{0};
    uint64_t                  m_geometry{0}, m_environment{0}, m_environmentTexture{0}, m_surface{0}, m_lighting{0};
    uint64_t                  m_radianceGeneration{0}, m_recordedRadianceGeneration{0};
    uint32_t                  m_bounceResponsiveFrames{0}; // Shared responsive window for both hit-radiance signals.
    uint32_t                  m_frame{0}, m_current{0};
    bool                      m_valid{false}, m_prepared{false}, m_recorded{false};
    const char*               m_resetReason{"initial"};
    const char*               m_failureReason{nullptr};
    // Recording may invalidate history for changed settings without consuming it.
    bool        m_preparedHistoryValid{false};
    const char* m_preparedResetReason{"initial"};
    uint32_t    m_preparedResponsiveFrames{0};
};
} // namespace zen::rc
