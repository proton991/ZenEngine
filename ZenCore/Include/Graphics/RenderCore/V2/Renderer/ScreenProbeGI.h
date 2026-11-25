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
    bool       Prepare(glm::uvec2 extent, bool useSpecular, bool hardwareTracing);
    bool       InitializeHistories();
    void       Record(VoxelGIRenderer&           voxelGI,
                      RenderScene&               scene,
                      const WorldRadianceCache&  cache,
                      const HybridGIUniformData& receiver,
                      const Mat4&                projectionView,
                      RHISampler*                sampler,
                      bool                       reset,
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
    uint32_t GetProbeCount() const
    {
        return m_uniforms.grid.w;
    }
    uint64_t GetHistoryBytes() const
    {
        return uint64_t(GetProbeCount())
                 * (48u + ZEN_SCREEN_PROBE_LIGHTING_VECTORS * sizeof(Vec4)
                    + (m_useSpecular ? ZEN_SCREEN_PROBE_SPECULAR_TEXELS * 8u : 0u)
                    + ZEN_SCREEN_PROBE_PROPOSAL_WORDS * sizeof(uint32_t))
                 * ZEN_SCREEN_PROBE_HISTORY_FRAMES
             + m_glossy.GetHistoryBytes();
    }

private:
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
    Mat4              m_projectionViews[ZEN_SCREEN_PROBE_HISTORY_FRAMES]{};
    RDGTexture        m_output[3];
    glm::uvec2        m_extent{0};
    uint32_t          m_current{0};
    uint32_t          m_validHistory{0};
    bool              m_needsClear{true};
    bool              m_recorded{false};
    struct Uniforms
    {
        Mat4       projectionView{1};
        Mat4       previousProjectionViews[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1]{};
        glm::uvec4 grid{0};
        Vec4       control{0};
        alignas(16) uint32_t spacing{0};
        uint32_t adaptiveTiles{0};
        uint32_t residualEnvironment{0};
    } m_uniforms;
    static_assert(sizeof(Uniforms) == 304);
};
} // namespace zen::rc
