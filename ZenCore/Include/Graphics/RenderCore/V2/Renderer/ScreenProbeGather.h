#pragma once
#include "Graphics/RenderCore/V2/RenderGraph/RenderGraph.h"
#include "Graphics/RenderCore/V2/RenderView.h"
#include "Graphics/RenderCore/V2/Renderer/DebugVisualization.h"
#include "Graphics/RenderCore/V2/ResourceRetirement.h"

namespace zen::rc
{
class RenderDevice;
class VoxelGIRenderer;
struct HybridGIUniformData;
struct SceneUniformData;

// probe_common.glsl uProbeData.
struct ScreenProbeUniformData
{
    glm::uvec4 grid{0};       // uniform probe counts, pixel spacing, valid history
    Vec4       parameters{0}; // plane threshold, history limit, responsive radiance, frames since history restarted
    glm::uvec4 adaptive{0};   // subtile counts, adaptive probe rows after the uniform grid, history slots
};
static_assert(sizeof(ScreenProbeUniformData) == 48);

// Probe counts from probe_allocate.comp's counters: of the last lighting capture once it completed,
// otherwise of the newest frame whose requested count was read back.
struct ScreenProbeStatistics
{
    uint32_t uniformProbes{0};
    uint32_t adaptiveCapacity{0};
    uint32_t adaptiveAllocated{0};
    uint32_t adaptiveRequested{0}; // Above the capacity when the pool ran short.
};

// The probe SH and compact pixel histories belong to one HybridGIRenderer view.
// Only submitted frames advance either history; RDG tracks outstanding GPU readers.
// One uniform probe per spacing tile, plus adaptive probes in subtiles whose receivers no
// uniform probe represents (Graphics/Shared/ScreenProbe.h). Every frame copies the requested
// adaptive count to a host buffer; once those frames complete, the adaptive pool grows to the
// demand plus headroom, or shrinks after demand stayed low, keeping the probe history.
class ScreenProbeGather
{
public:
    explicit ScreenProbeGather(RenderDevice* device);
    bool                   Prepare(uint32_t width, uint32_t height, uint32_t spacing);
    void                   BuildRenderGraph(VoxelGIRenderer&           voxelGI,
                                            RHISampler*                sampler,
                                            const HybridGIUniformData& uniforms,
                                            const SceneUniformData&    scene,
                                            RHIBuffer*                 captureOutput,
                                            RHIBuffer*                 captureReadback);
    DebugOutputDescription BuildDebugView(const RenderView& view, const DebugSelection& selection, RHISampler* sampler) const;
    void                   OnRenderGraphExecuted(RenderFrameResult result);
    void                   Destroy();
    bool                   HasHistory() const
    {
        return m_probes[0] != nullptr;
    }
    // Counters copied by the last frame recorded with a capture, valid once that frame completed;
    // without a capture, those of the newest completed frame.
    ScreenProbeStatistics ReadStatistics() const;
    RDGTexture            GetSky() const
    {
        return m_output[Sky];
    }
    RDGTexture GetBounce() const
    {
        return m_output[Bounce];
    }
    RDGTexture GetSpecular() const
    {
        return m_output[Specular];
    }

private:
    enum HistoryTexture : uint32_t
    {
        Sky,
        Bounce,
        Specular,
        Position,
        Surface,
        NormalRoughness,
        Count
    };
    void BindInputs(RDGPassDescBase&           pass,
                    RHISampler*                sampler,
                    const HybridGIUniformData& uniforms,
                    const SceneUniformData&    scene) const;
    void BindTraceInputs(RDGPassDescBase& pass, VoxelGIRenderer& voxelGI, bool hardware) const;
    void Dispatch(RDGComputePassDesc&& pass, const glm::uvec2& groups);
    // Reads the requested counts of completed frames and returns the adaptive rows they call for.
    uint32_t PollDemand();
    // Replaces both probe histories with ones of the given adaptive rows; the previous side is
    // copied by the next recorded frame. Returns false, keeping the current pool, if allocation fails.
    bool ResizePool(uint32_t rows);
    void RecordDemandReadback(RDGBuffer counters);
    // Uniform probes followed by the adaptive rows.
    uint32_t SlotCount() const
    {
        return SlotCount(m_uniforms.adaptive.z);
    }
    uint32_t SlotCount(uint32_t rows) const
    {
        return m_uniforms.grid.x * (m_uniforms.grid.y + rows);
    }
    uint32_t SubtileCount() const
    {
        return m_uniforms.adaptive.x * m_uniforms.adaptive.y;
    }
    uint32_t AdaptiveCapacity() const
    {
        return m_uniforms.grid.x * m_uniforms.adaptive.z;
    }

    struct PendingDemand
    {
        RHIBuffer*         readback;
        ResourceRetirement retirement;
    };

    RenderDevice* m_device;
    RHIBuffer*    m_probes[2]{};
    RHIBuffer*    m_subtiles[2]{};          // Adaptive probe slot per subtile, for the next frame's reprojection.
    RHIBuffer*    m_statistics{nullptr};    // Host copy of the counters, written only by capture frames.
    RHIBuffer*    m_retraceHeader{nullptr}; // Host-written start of the brick trace's re-trace queue header.
    bool          m_statisticsRecorded{false};
    // Host copies of the requested count, in submission order, and the copies ready for reuse.
    HeapVector<PendingDemand> m_pendingDemand;
    HeapVector<RHIBuffer*>    m_freeDemand;
    RHIBuffer*                m_recordedDemand{nullptr};
    glm::uvec2                m_lastDemand{0}; // Allocated and requested of the newest read-back frame.
    // Histories replaced by a pool resize, kept until a submitted frame copied the previous side.
    RHIBuffer*             m_resizedFrom[2]{};
    uint32_t               m_resizedSlots{0};
    uint32_t               m_baseRows{0};        // The smallest pool, ZEN_PROBE_ADAPTIVE_ROW_RATIO.
    uint32_t               m_maxRows{0};         // Every subtile's every candidate.
    uint32_t               m_lowDemandFrames{0}; // Completed frames in a row that needed half the pool or less.
    uint32_t               m_lowDemandRows{0};   // The most rows those frames needed.
    RHITexture*            m_history[2][Count]{};
    RDGTexture             m_output[Count];
    RDGTexture             m_rawSky, m_rawBounce, m_rawSpecular;
    ScreenProbeUniformData m_uniforms;
    glm::uvec2             m_extent{0};
    uint32_t               m_current{0};
    uint32_t               m_historyFrames{0}; // Submitted frames of probe history since it last restarted.
    bool                   m_needsClear{true};
    bool                   m_recorded{false};
};
} // namespace zen::rc
