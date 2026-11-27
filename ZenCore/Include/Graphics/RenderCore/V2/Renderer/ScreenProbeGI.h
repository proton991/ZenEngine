#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderCoreDefs.h"
#include "Math/Math.h"
#include "Graphics/Shared/ScreenProbeGI.h"
#include "Graphics/RenderCore/V2/Renderer/GlossyReflections.h"

namespace zen::rc
{
class RenderDevice;
class RenderScene;
class VoxelGIRenderer;
class WorldRadianceCache;
struct HybridGIUniformData;

// Per-view probe history. Persistent resources are committed only after successful submission.
class ScreenProbeGI
{
public:
    explicit ScreenProbeGI(RenderDevice* device);
    ~ScreenProbeGI();
    bool       Prepare(glm::uvec2 extent, bool useSpecular, bool hardwareTracing, bool movingLights, bool temporal);
    bool       InitializeHistories();
    void       Record(VoxelGIRenderer&           voxelGI,
                      RenderScene&               scene,
                      const WorldRadianceCache&  cache,
                      const HybridGIUniformData& receiver,
                      const Mat4&                projectionView,
                      RHISampler*                sampler,
                      bool                       reset,
                      bool                       pixelReset,
                      bool                       skyLight);
    void       OnRenderGraphExecuted(RenderFrameResult result);
    void       Destroy();
    RDGTexture GetSky() const
    {
        return m_output[0];
    }
    RDGTexture GetBounce() const
    {
        return m_output[1];
    }
    RDGTexture GetSpecular() const
    {
        return m_output[2];
    }
    RDGTexture GetRawSky() const
    {
        return m_gatheredDiffuse[0];
    }
    RDGTexture GetRawBounce() const
    {
        return m_gatheredDiffuse[1];
    }
    uint32_t GetProbeCount() const
    {
        return m_uniforms.grid.w;
    }
    uint64_t GetHistoryBytes() const
    {
        return uint64_t(GetProbeCount())
                 * (48u + m_uniforms.layoutInfo.x * sizeof(Vec4)
                    + (m_useSpecular ? (ZEN_SCREEN_PROBE_SPECULAR_TEXELS + 1u) * 8u : 0u)
                    + ZEN_SCREEN_PROBE_PROPOSAL_WORDS * sizeof(uint32_t))
                 * ZEN_SCREEN_PROBE_HISTORY_FRAMES
             + (m_pixelHistory[0].guide != nullptr ? uint64_t(m_extent.x) * m_extent.y * 2u * (16u + kPixelGuideBytes) : 0u)
             + m_glossy.GetHistoryBytes();
    }

private:
    static constexpr uint32_t kPixelGuideBytes = 3u * sizeof(uint32_t);
    static_assert(ZEN_SCREEN_PROBE_HISTORY_FRAMES % 2u == 0u);
    struct PixelHistorySide
    {
        RHITexture* diffuse[2]{};
        RHIBuffer*  guide{nullptr};
    };
    bool              AllocatePixelHistory(glm::uvec2 extent, uint32_t guideBytes);
    void              DestroyPixelHistory();
    void              RecordPixelHistory(VoxelGIRenderer&           voxelGI,
                                         RenderScene&               scene,
                                         const WorldRadianceCache&  cache,
                                         const HybridGIUniformData& receiver,
                                         RHISampler*                sampler,
                                         RDGBuffer                  filtered,
                                         bool                       reset);
    void              RecordClear(RenderGraph& graph) const;
    void              Bind(RDGPassDescBase& pass, const HybridGIUniformData& receiver, RHISampler* sampler) const;
    void              BindHistory(RDGPassDescBase& pass,
                                  const char*      currentName,
                                  const char*      previousName,
                                  RHIBuffer* const (&buffers)[ZEN_SCREEN_PROBE_HISTORY_FRAMES],
                                  bool             initialize = false) const;
    void              Dispatch(RenderGraph& graph, RDGComputePassDesc&& pass, uint32_t groups) const;
    void              BindTrace(RDGPassDescBase&           pass,
                                VoxelGIRenderer&           voxelGI,
                                RenderScene&               scene,
                                const WorldRadianceCache&  cache,
                                const HybridGIUniformData& receiver,
                                RHISampler*                sampler) const;
    void              AddTracePass(RDGRayTracingPassDesc&& pass, uint32_t width, uint32_t height) const;
    void              RecordSources(RDGComputePassDesc&        gather,
                                    VoxelGIRenderer&           voxelGI,
                                    RenderScene&               scene,
                                    const HybridGIUniformData& receiver,
                                    RHISampler*                sampler) const;
    RenderDevice*     m_device;
    GlossyReflections m_glossy;
    bool              m_useSpecular = false;
    RHIBuffer*        m_probes[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    RHIBuffer*        m_lighting[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    RHIBuffer*        m_specular[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    RHIBuffer*        m_cdf[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    Vec4 m_environmentHistory[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{}, m_orientationHistory[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    Mat4 m_projectionViews[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    PixelHistorySide m_pixelHistory[2];
    RDGTexture       m_gatheredDiffuse[2];
    RDGTexture       m_output[3];
    glm::uvec2       m_extent{0};
    uint32_t         m_current{0};
    uint32_t         m_validHistory{0};
    bool             m_needsClear{true};
    bool             m_recorded{false};
    // Whether the last allocation requested a pixel history. Its resources stay null if memory ran out.
    bool m_pixelHistoryRequested{false};
    struct Uniforms
    {
        Mat4       projectionView{1};
        Mat4       previousProjectionViews[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1]{};
        glm::uvec4 grid{0};
        Vec4       control{0};
        alignas(16) uint32_t spacing{0};
        uint32_t   adaptiveTiles{0};
        uint32_t   residualEnvironment{0};
        uint32_t   localResponse{0};
        glm::uvec4 layoutInfo{}; // lighting stride, split moving light signal
        Vec4       environment[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
        Vec4       orientation[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    } m_uniforms;
    static_assert(sizeof(Uniforms) == 320 + 32 * ZEN_SCREEN_PROBE_HISTORY_FRAMES);
};
} // namespace zen::rc
