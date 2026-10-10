#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/Renderer/GBuffer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"

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
    glm::uvec4         provider{0};   // hardware provider, guide valid, reserved, reserved
    Vec4               rejection{0};  // plane threshold, specular enabled, temporal enabled, static reference
    glm::uvec4         bounce{0};     // ray bounce enabled, bounce history valid, responsive reconstruction, cache changed
    glm::uvec4         reflection{0}; // reflections enabled, history valid, responsive reconstruction, cache changed
};
static_assert(sizeof(HybridGIUniformData) == 256);

// One instance owns histories for one view. Imported textures use RenderDevice's
// submission history (including all outstanding readers), not CPU frame parity.
class HybridGIRenderer
{
public:
    explicit HybridGIRenderer(RenderDevice* device);
    void               Prepare(const RenderView& view, RenderScene& scene);
    void               BindReceiverInputs(RDGPassDescBase& pass) const;
    HybridReceiverDraw ReceiverDraw(uint32_t node, uint32_t material) const;
    bool               BuildRenderGraph(const RenderView& view, VoxelGIRenderer& voxelGI);
    void               BindLightingInputs(RDGPassDescBase& pass) const;
    void               OnRenderGraphExecuted(bool succeeded);
    void               InvalidateHistory(const char* reason);
    void               Destroy();
    void               SetCapture(RHIBuffer* output, RHIBuffer* readback)
    {
        m_captureOutput   = output;
        m_captureReadback = readback;
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

private:
    bool PrepareHistory(uint32_t width, uint32_t height, bool bounce, bool reflections);
    void BindGuides(RDGPassDescBase& pass) const;
    void Dispatch(RDGComputePassDesc&& pass);

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
        Reflection, // Hit distance, luminance moments, history length; radiance is in Specular.rgb.
        Count
    };
    RenderDevice* m_device;
    RenderScene*  m_scene{nullptr};
    RHISampler*   m_sampler{nullptr};
    RHIBuffer*    m_captureOutput{nullptr};
    RHIBuffer*    m_captureReadback{nullptr};
    RHITexture*   m_history[2][Count]{};
    // Visibility guide per kGuideTile x kGuideTile screen tile, previous and next (hybrid_trace.glsl).
    RHIBuffer*          m_guide[2]{};
    RDGTexture          m_outputSky, m_outputSpecular, m_outputBounce;
    HeapVector<Mat4>    m_previousModels;
    HeapVector<Mat4>    m_recordedModels;
    Mat4                m_previousProjection{1};
    Mat4                m_recordedProjection{1};
    Mat4                m_recordedProjectionView{1};
    Vec4                m_recordedViewPosition{0};
    Vec4                m_recordedViewDirection{0, 0, 1, 0};
    HybridGIUniformData m_uniforms;
    VoxelGISettings     m_previousSettings;
    bool                m_previousHardware{false};
    bool                m_previousRayBounce{false};
    bool                m_previousReflections{false};
    glm::uvec2          m_extent{0};
    uint64_t            m_geometry{0}, m_environment{0};
    uint64_t            m_radianceGeneration{0}, m_recordedRadianceGeneration{0};
    uint32_t            m_bounceResponsiveFrames{0}; // Shared responsive window for both hit-radiance signals.
    uint32_t            m_frame{0}, m_current{0};
    bool                m_valid{false}, m_prepared{false}, m_recorded{false};
    const char*         m_resetReason{"initial"};
};
} // namespace zen::rc
