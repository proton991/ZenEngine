#include "Graphics/RenderCore/V2/Renderer/ScreenProbeGather.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/ScreenProbe.h"
#include <algorithm>
#include <cstring>

namespace zen::rc
{
namespace
{
// Three receiver vectors, eight L1 SH coefficients and two normal-aligned irradiance anchors,
// matching probe_common.glsl.
constexpr uint32_t kProbeBytes = 13u * sizeof(Vec4);
static_assert(kProbeBytes == 208);
// ProbePlacement: the receiver vectors alone.
constexpr uint32_t kPlacementBytes = 3u * sizeof(Vec4);
// The subtile maps and the allocation block counts hold one uvec2 per subtile and per block.
static_assert(ZEN_PROBE_ADAPTIVE_PER_SUBTILE == 2u);
// probe_allocate.comp's counters: allocated and requested adaptive probes, two reserved words, then
// the probe trace's indirect dispatch at kTraceArgumentsOffset.
constexpr uint32_t kCounterBytes         = 8u * sizeof(uint32_t);
constexpr uint32_t kTraceArgumentsOffset = 4u * sizeof(uint32_t);
// The allocated and requested counts, read back every frame to size the adaptive pool.
constexpr uint32_t kDemandBytes = 2u * sizeof(uint32_t);
// Host copies in flight before a frame goes without one.
constexpr uint32_t kDemandReadbacks = 4u;
// Completed frames in a row that need at most half the pool before it shrinks.
constexpr uint32_t kShrinkFrames = 240u;
// Frames of probe history counted since it restarted; far beyond any history limit, exact as float.
constexpr uint32_t kMaxHistoryFrames = 1u << 20;
// probe_specular_filter.comp passes at block steps 1, 2, 4 and 8, as registered in ShaderProgram.cpp.
constexpr uint32_t kSpecularFilterPasses = 4u;
const char* const  kHistoryNames[]       = {"Sky", "Bounce", "Specular", "Position", "Surface", "NormalRoughness"};
const DataFormat   kHistoryFormats[]     = {DataFormat::eR16G16B16A16SFloat, DataFormat::eR16G16B16A16SFloat,
                                            DataFormat::eR16G16B16A16SFloat, DataFormat::eR32G32B32A32SFloat,
                                            DataFormat::eR32G32UInt,         DataFormat::eR16G16B16A16SFloat};

NameID ProbeTraceProgram(const char* base, bool hardware, bool bricks)
{
    return NameID(fmt::format("{}{}{}SP", base, hardware ? "Hardware" : "", bricks ? "Bricks" : ""));
}

// Probe histories are also copy sources: a pool resize carries the previous probes across.
RHIBuffer* CreateProbeHistory(RenderDevice* device, uint64_t bytes, uint32_t side)
{
    RHIBufferCreateInfo info;
    info.size         = bytes;
    info.tag          = NameID(fmt::format("probe_history_{}", side));
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                             RHIBufferUsageFlagBits::eTransferDstBuffer);
    return device->CreateBuffer(info);
}
} // namespace

ScreenProbeGather::ScreenProbeGather(RenderDevice* device) : m_device(device) {}

bool ScreenProbeGather::Prepare(uint32_t width, uint32_t height, uint32_t spacing)
{
    if (m_extent != glm::uvec2(width, height) || m_uniforms.grid.z != spacing)
    {
        Destroy();
        m_extent        = glm::uvec2(width, height);
        m_uniforms.grid = glm::uvec4((width + spacing - 1) / spacing, (height + spacing - 1) / spacing, spacing, 0);
        const glm::uvec2 subtiles((width + ZEN_PROBE_SUBTILE - 1) / ZEN_PROBE_SUBTILE,
                                  (height + ZEN_PROBE_SUBTILE - 1) / ZEN_PROBE_SUBTILE);
        m_baseRows = (m_uniforms.grid.y + ZEN_PROBE_ADAPTIVE_ROW_RATIO - 1) / ZEN_PROBE_ADAPTIVE_ROW_RATIO;
        m_maxRows  = std::max(m_baseRows, (subtiles.x * subtiles.y * ZEN_PROBE_ADAPTIVE_PER_SUBTILE + m_uniforms.grid.x - 1)
                                              / m_uniforms.grid.x);
        m_uniforms.adaptive = glm::uvec4(subtiles, m_baseRows, 0);
    }
    const uint32_t rows = PollDemand();
    // One resize at a time: a pending one still holds the history to copy.
    if (rows != m_uniforms.adaptive.z && m_resizedFrom[0] == nullptr)
    {
        if (m_probes[0] == nullptr)
        {
            m_uniforms.adaptive.z = rows;
        }
        else
        {
            ResizePool(rows);
        }
    }
    uint64_t probeBytes = 0, placementBytes = 0, subtileBytes = 0;
    bool     ready =
        ValidateGIStorageBuffer(SlotCount(), kProbeBytes, m_device->GetGPUInfo(), probeBytes) == GIResourceStatus::eSuccess
        && ValidateGIStorageBuffer(uint64_t(SubtileCount()) * ZEN_PROBE_ADAPTIVE_PER_SUBTILE, kPlacementBytes,
                                   m_device->GetGPUInfo(), placementBytes)
               == GIResourceStatus::eSuccess
        && ValidateGIStorageBuffer(SubtileCount(), ZEN_PROBE_ADAPTIVE_PER_SUBTILE * sizeof(uint32_t), m_device->GetGPUInfo(),
                                   subtileBytes)
               == GIResourceStatus::eSuccess;
    for (uint32_t side = 0; side < 2 && ready; ++side)
    {
        if (m_probes[side] == nullptr)
        {
            m_probes[side] = CreateProbeHistory(m_device, probeBytes, side);
            ready          = m_probes[side] != nullptr;
        }
        if (ready && m_subtiles[side] == nullptr)
        {
            m_subtiles[side] = m_device->CreateStorageBuffer(static_cast<uint32_t>(subtileBytes), nullptr,
                                                             NameID(fmt::format("probe_subtiles_{}", side)));
            ready            = m_subtiles[side] != nullptr;
        }
        for (uint32_t texture = 0; texture < Count && ready; ++texture)
        {
            if (m_history[side][texture] == nullptr)
            {
                TextureFormat format;
                format.dimension         = TextureDimension::e2D;
                format.depth             = 1;
                format.width             = width;
                format.height            = height;
                format.format            = kHistoryFormats[texture];
                m_history[side][texture] = m_device->CreateTextureStorage(
                    format, {.copyUsage = true}, NameID(fmt::format("probe_pixel_history_{}_{}", side, texture)));
                ready = m_history[side][texture] != nullptr;
            }
        }
    }
    if (ready && m_retraceHeader == nullptr)
    {
        // The brick trace's re-trace queue header at the start of every frame: no dispatch groups
        // (x), one in y and z for the indirect dispatch, no rays (w).
        RHIBufferCreateInfo info;
        info.size         = sizeof(glm::uvec4);
        info.tag          = "probe_retrace_header_reset";
        info.allocateType = RHIBufferAllocateType::eCPUWrite;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer);
        m_retraceHeader = m_device->CreateBuffer(info);
        uint8_t* data   = m_retraceHeader != nullptr ? m_retraceHeader->Map() : nullptr;
        ready           = data != nullptr;
        if (ready)
        {
            const glm::uvec4 header(0, 1, 1, 0);
            std::memcpy(data, &header, sizeof(header));
            m_retraceHeader->Unmap();
        }
    }
    if (!ready)
    {
        LOGE("Screen probe history exceeds device limits or could not be allocated");
        Destroy();
    }
    return ready;
}

uint32_t ScreenProbeGather::PollDemand()
{
    uint32_t rows = m_uniforms.adaptive.z;
    while (!m_pendingDemand.empty() && m_device->IsResourceRetired(m_pendingDemand.front().retirement))
    {
        RHIBuffer*      readback = m_pendingDemand.front().readback;
        const uint32_t* counters = reinterpret_cast<const uint32_t*>(readback->Map());
        // probe_allocate.comp never requests more than every subtile's every candidate; a larger
        // count is not a demand and is ignored.
        if (counters != nullptr && counters[1] > SubtileCount() * ZEN_PROBE_ADAPTIVE_PER_SUBTILE)
        {
            readback->Unmap();
        }
        else if (counters != nullptr)
        {
            // A quarter of headroom: growing demand reaches the pool's end a few frames later.
            const uint64_t wanted = (uint64_t(counters[1]) * 5u / 4u + m_uniforms.grid.x - 1u) / m_uniforms.grid.x;
            const uint32_t needed = static_cast<uint32_t>(std::clamp<uint64_t>(wanted, m_baseRows, m_maxRows));
            m_lastDemand          = glm::uvec2(counters[0], counters[1]);
            readback->Unmap();
            if (needed > rows)
            {
                rows = needed;
            }
            if (needed * 2u <= rows)
            {
                ++m_lowDemandFrames;
                m_lowDemandRows = std::max(m_lowDemandRows, needed);
            }
            else
            {
                m_lowDemandFrames = 0;
                m_lowDemandRows   = 0;
            }
            if (m_lowDemandFrames >= kShrinkFrames)
            {
                rows              = m_lowDemandRows;
                m_lowDemandFrames = 0;
                m_lowDemandRows   = 0;
            }
        }
        m_freeDemand.push_back(readback);
        m_pendingDemand.erase(m_pendingDemand.begin());
    }
    return rows;
}

bool ScreenProbeGather::ResizePool(uint32_t rows)
{
    uint64_t   probeBytes = 0;
    RHIBuffer* resized[2]{};
    bool       ready =
        ValidateGIStorageBuffer(SlotCount(rows), kProbeBytes, m_device->GetGPUInfo(), probeBytes) == GIResourceStatus::eSuccess;
    for (uint32_t side = 0; side < 2 && ready; ++side)
    {
        resized[side] = CreateProbeHistory(m_device, probeBytes, side);
        ready         = resized[side] != nullptr;
    }
    if (ready)
    {
        // Slot indices stay valid across the resize: uniform probes keep theirs, and the previous
        // frame's adaptive slots lie below the old count. A shrink drops the slots past the new one.
        m_resizedSlots = m_needsClear ? 0u : std::min(SlotCount(), SlotCount(rows));
        for (uint32_t side = 0; side < 2; ++side)
        {
            if (m_needsClear)
            {
                m_device->DestroyBuffer(m_probes[side]);
            }
            else
            {
                m_resizedFrom[side] = m_probes[side];
            }
            m_probes[side] = resized[side];
        }
        m_uniforms.adaptive.z = rows;
    }
    else
    {
        m_device->DestroyBuffer(resized[0]);
        m_device->DestroyBuffer(resized[1]);
        // Stop growing at the current pool rather than retrying every frame.
        LOGW("Screen probe pool could not be resized to {} adaptive probes; keeping {}", m_uniforms.grid.x * rows,
             AdaptiveCapacity());
        m_maxRows = std::max(m_baseRows, std::min(m_maxRows, m_uniforms.adaptive.z));
    }
    return ready;
}

void ScreenProbeGather::RecordDemandReadback(RDGBuffer counters)
{
    if (m_recordedDemand == nullptr)
    {
        if (m_freeDemand.empty() && m_pendingDemand.size() < kDemandReadbacks)
        {
            RHIBufferCreateInfo info;
            info.size         = kDemandBytes;
            info.tag          = "probe_demand_readback";
            info.allocateType = RHIBufferAllocateType::eCPURead;
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
            RHIBuffer* readback = m_device->CreateBuffer(info);
            if (readback != nullptr)
            {
                m_freeDemand.push_back(readback);
            }
        }
        if (!m_freeDemand.empty())
        {
            RenderGraph* graph = m_device->GetCurrentFrameRDG();
            m_recordedDemand   = m_freeDemand.back();
            m_freeDemand.pop_back();
            graph->AddTransferPass("ReadProbeDemand")
                .CopyBuffer(counters,
                            graph->GetResourceManager()->ImportBuffer(m_recordedDemand, RDGImportContents::eUndefined),
                            {0, 0, kDemandBytes})
                .NeverCull();
        }
    }
}

void ScreenProbeGather::BindInputs(RDGPassDescBase&           pass,
                                   RHISampler*                sampler,
                                   const HybridGIUniformData& uniforms,
                                   const SceneUniformData&    scene) const
{
    BindHybridReceiverGuides(pass, sampler, uniforms);
    pass.BindValue("uProbeData", m_uniforms);
    pass.BindValue("uSceneData", scene);
}

void ScreenProbeGather::BindTraceInputs(RDGPassDescBase& pass, VoxelGIRenderer& voxelGI, bool hardware) const
{
    voxelGI.BindHitRadianceInputs(pass);
    if (hardware)
    {
        voxelGI.BindHardwareRayInputs(pass);
    }
    else
    {
        voxelGI.BindRayInputs(pass);
        voxelGI.BindSoftwareRayInputs(pass);
    }
    if (voxelGI.UsesBrickTraversal())
    {
        voxelGI.BindBrickInputs(pass);
    }
}

void ScreenProbeGather::Dispatch(RDGComputePassDesc&& pass, const glm::uvec2& groups)
{
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
}

void ScreenProbeGather::BuildRenderGraph(VoxelGIRenderer&           voxelGI,
                                         RHISampler*                sampler,
                                         const HybridGIUniformData& uniforms,
                                         const SceneUniformData&    scene,
                                         RHIBuffer*                 captureOutput,
                                         RHIBuffer*                 captureReadback)
{
    RenderGraph*        graph     = m_device->GetCurrentFrameRDG();
    RDGResourceManager* resources = graph->GetResourceManager();
    const glm::uvec2    grid(m_uniforms.grid);
    const glm::uvec2    slots(grid.x, grid.y + m_uniforms.adaptive.z);
    const glm::uvec2    probeGroups    = (grid + glm::uvec2(7)) / 8u;
    const glm::uvec2    slotGroups     = (slots + glm::uvec2(7)) / 8u;
    const glm::uvec2    pixelGroups    = (m_extent + glm::uvec2(7)) / 8u;
    const uint32_t      allocateGroups = (SubtileCount() + ZEN_PROBE_ALLOCATE_GROUP - 1) / ZEN_PROBE_ALLOCATE_GROUP;
    m_uniforms.grid.w                  = !m_needsClear && uniforms.sampling.w != 0 ? 1u : 0u;
    // How old the previous probes can be: only probes clearly younger than that trace extra rays.
    m_uniforms.parameters = Vec4(uniforms.rejection.x, float(uniforms.sampling.z), float(uniforms.bounce.z),
                                 float(m_uniforms.grid.w != 0 ? m_historyFrames : 0u));
    m_uniforms.adaptive.w = m_resizedFrom[0] != nullptr ? m_resizedSlots : SlotCount();
    if (m_resizedFrom[0] != nullptr)
    {
        // The pool was resized: bring the previous frame's probes into the new history.
        graph->AddTransferPass("ResizeProbeHistory")
            .CopyBuffer(m_resizedFrom[1 - m_current], m_probes[1 - m_current], {0, 0, uint64_t(m_resizedSlots) * kProbeBytes})
            .NeverCull();
    }
    if (m_needsClear)
    {
        for (uint32_t side = 0; side < 2; ++side)
        {
            RDGComputePassDesc clear;
            clear.SetShaderProgramName("ProbeClearSP");
            clear.SetPassTag("InitializeProbes");
            BindInputs(clear, sampler, uniforms, scene);
            clear.BindStorageBuffer("ProbeOutput", m_probes[side], RDGContentGuarantee::eFullWrite);
            clear.BindStorageBuffer("ProbeSubtiles", m_subtiles[side], RDGContentGuarantee::eFullWrite);
            Dispatch(std::move(clear), glm::uvec2((std::max(SlotCount(), SubtileCount()) + 63u) / 64u, 1u));
            for (uint32_t texture = 0; texture < Count; ++texture)
            {
                graph->AddTransferPass("InitializeProbePixelHistory").ClearTexture(m_history[side][texture], Color(0));
            }
        }
    }
    // Uniform placement, adaptive candidates where uniform probes leave receivers uncovered, then a
    // deterministic assignment of the adaptive pool that fills every subtile's first probe first.
    RDGBufferDesc placementDesc;
    placementDesc.name = "probe_placements";
    placementDesc.size = uint64_t(grid.x) * grid.y * kPlacementBytes;
    placementDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    const RDGBuffer placements         = resources->CreateBuffer(placementDesc);
    placementDesc.name                 = "probe_subtile_candidates";
    placementDesc.size                 = uint64_t(SubtileCount()) * ZEN_PROBE_ADAPTIVE_PER_SUBTILE * kPlacementBytes;
    const RDGBuffer candidates         = resources->CreateBuffer(placementDesc);
    placementDesc.name                 = "probe_adaptive_placements";
    placementDesc.size                 = uint64_t(AdaptiveCapacity()) * kPlacementBytes;
    const RDGBuffer adaptivePlacements = resources->CreateBuffer(placementDesc);
    placementDesc.name                 = "probe_allocation_blocks";
    placementDesc.size                 = uint64_t(allocateGroups) * sizeof(glm::uvec2);
    const RDGBuffer allocationBlocks   = resources->CreateBuffer(placementDesc);
    RDGBufferDesc   counterDesc;
    counterDesc.name = "probe_counters";
    counterDesc.size = kCounterBytes;
    counterDesc.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer,
                                    RHIBufferUsageFlagBits::eTransferSrcBuffer);
    const RDGBuffer counters = resources->CreateBuffer(counterDesc);
    placementDesc.name       = "probe_claims";
    placementDesc.size       = uint64_t(grid.x) * grid.y * sizeof(uint32_t);
    const RDGBuffer claims   = resources->CreateBuffer(placementDesc);
    // Previous probes that still project onto their surfaces keep their place and history. Each
    // tile first claims one; a probe that lost its tile can then go to a free neighbour.
    RDGComputePassDesc claim;
    claim.SetShaderProgramName("ProbeClaimSP");
    claim.SetPassTag("ProbeClaim");
    BindInputs(claim, sampler, uniforms, scene);
    claim.BindStorageBuffer("ProbeHistory", m_probes[1 - m_current]);
    claim.BindStorageBuffer("ProbeClaims", claims, RDGContentGuarantee::eFullWrite);
    Dispatch(std::move(claim), probeGroups);
    RDGComputePassDesc place;
    place.SetShaderProgramName("ProbePlaceSP");
    place.SetPassTag("ProbePlace");
    BindInputs(place, sampler, uniforms, scene);
    place.BindStorageBuffer("ProbePlacements", placements, RDGContentGuarantee::eFullWrite);
    place.BindStorageBuffer("ProbeHistory", m_probes[1 - m_current]);
    place.BindStorageBuffer("ProbeClaims", claims);
    Dispatch(std::move(place), probeGroups);
    RDGComputePassDesc adaptive;
    adaptive.SetShaderProgramName("ProbeAdaptiveSP");
    adaptive.SetPassTag("ProbeAdaptive");
    BindInputs(adaptive, sampler, uniforms, scene);
    adaptive.BindStorageBuffer("ProbePlacements", placements);
    adaptive.BindStorageBuffer("ProbeSubtileCandidates", candidates, RDGContentGuarantee::eFullWrite);
    adaptive.BindStorageBuffer("ProbeHistory", m_probes[1 - m_current]);
    adaptive.BindStorageBuffer("ProbeSubtilesPrevious", m_subtiles[1 - m_current]);
    Dispatch(std::move(adaptive), glm::uvec2(m_uniforms.adaptive));
    RDGComputePassDesc count;
    count.SetShaderProgramName("ProbeAllocateCountSP");
    count.SetPassTag("ProbeAllocateCount");
    BindInputs(count, sampler, uniforms, scene);
    count.BindStorageBuffer("ProbeSubtileCandidates", candidates);
    count.BindStorageBuffer("ProbeAllocationBlocks", allocationBlocks, RDGContentGuarantee::eFullWrite);
    Dispatch(std::move(count), glm::uvec2(allocateGroups, 1u));
    RDGComputePassDesc allocate;
    allocate.SetShaderProgramName("ProbeAllocateSP");
    allocate.SetPassTag("ProbeAllocate");
    BindInputs(allocate, sampler, uniforms, scene);
    allocate.BindStorageBuffer("ProbeSubtileCandidates", candidates);
    allocate.BindStorageBuffer("ProbeAllocationBlocks", allocationBlocks);
    allocate.BindStorageBuffer("ProbeAdaptivePlacements", adaptivePlacements, RDGContentGuarantee::eProducedElements);
    allocate.BindStorageBuffer("ProbeSubtiles", m_subtiles[m_current], RDGContentGuarantee::eFullWrite);
    allocate.BindStorageBuffer("ProbeCounters", counters, RDGContentGuarantee::eFullWrite);
    Dispatch(std::move(allocate), glm::uvec2(allocateGroups, 1u));
    RecordDemandReadback(counters);

    RDGBufferDesc probeDesc;
    probeDesc.name = "probe_raw_sh";
    probeDesc.size = uint64_t(SlotCount()) * kProbeBytes;
    probeDesc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    const RDGBuffer    rawProbes = resources->CreateBuffer(probeDesc);
    const bool         hardware  = uniforms.provider.x != 0;
    const bool         bricks    = voxelGI.UsesBrickTraversal();
    RDGComputePassDesc trace;
    trace.SetShaderProgramName(ProbeTraceProgram("ProbeTrace", hardware, bricks));
    trace.SetPassTag("ProbeTrace");
    BindInputs(trace, sampler, uniforms, scene);
    BindTraceInputs(trace, voxelGI, hardware);
    trace.BindStorageBuffer("ProbePlacements", placements);
    trace.BindStorageBuffer("ProbeAdaptivePlacements", adaptivePlacements, RDGContentGuarantee::eConsumeProducedElements);
    trace.BindStorageBuffer("ProbeCounters", counters);
    // Uniform probes and the allocated adaptive rows only; the temporal pass bounds its reads by
    // the same count.
    trace.BindStorageBuffer("ProbeOutput", rawProbes, RDGContentGuarantee::eProducedElements);
    // Brick traversal queues shading rays whose hit lies next to a lamp (probe_trace.glsl):
    // probe_retrace_bricks.comp re-traces them against triangles, one thread per ray, and the
    // temporal pass adds them to their probes, so the brick trace carries no triangle traversal.
    RDGBuffer retraceHeader, retraceHeads, retraceRays, retraceResults;
    if (bricks)
    {
        const uint64_t capacity = uint64_t(SlotCount()) * ZEN_PROBE_RETRACE_RAYS_PER_SLOT;
        RDGBufferDesc  headerDesc;
        headerDesc.name = "probe_retrace_header";
        headerDesc.size = sizeof(glm::uvec4);
        headerDesc.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eIndirectBuffer,
                                       RHIBufferUsageFlagBits::eTransferDstBuffer);
        retraceHeader = resources->CreateBuffer(headerDesc);
        graph->AddTransferPass("ResetProbeRetrace")
            .CopyBuffer(resources->ImportHostWrittenBuffer(m_retraceHeader), retraceHeader, {0, 0, sizeof(glm::uvec4)});
        probeDesc.name = "probe_retrace_heads";
        probeDesc.size = uint64_t(SlotCount()) * sizeof(uint32_t);
        retraceHeads   = resources->CreateBuffer(probeDesc);
        probeDesc.name = "probe_retrace_rays";
        probeDesc.size = capacity * sizeof(glm::uvec4);
        retraceRays    = resources->CreateBuffer(probeDesc);
        probeDesc.name = "probe_retrace_results";
        probeDesc.size = capacity * 2u * sizeof(Vec4);
        retraceResults = resources->CreateBuffer(probeDesc);
        trace.BindStorageBuffer("ProbeRetraceHeader", retraceHeader);
        trace.BindStorageBuffer("ProbeRetraceHeads", retraceHeads, RDGContentGuarantee::eProducedElements);
        trace.BindStorageBuffer("ProbeRetraceRays", retraceRays, RDGContentGuarantee::eProducedElements);
    }
    trace.UseIndirectBuffer(counters);
    graph->AddComputePass(std::move(trace)).RecordPassCommands([counters](RDGPassCmdEncoder& encoder) {
        encoder.DispatchIndirect(counters, kTraceArgumentsOffset);
    });
    if (bricks)
    {
        RDGComputePassDesc retrace;
        retrace.SetShaderProgramName("ProbeRetraceBricksSP");
        retrace.SetPassTag("ProbeRetrace");
        BindInputs(retrace, sampler, uniforms, scene);
        BindTraceInputs(retrace, voxelGI, false);
        retrace.BindStorageBuffer("ProbePlacements", placements);
        retrace.BindStorageBuffer("ProbeAdaptivePlacements", adaptivePlacements, RDGContentGuarantee::eConsumeProducedElements);
        retrace.BindStorageBuffer("ProbeCounters", counters);
        retrace.BindStorageBuffer("ProbeRetraceHeader", retraceHeader);
        retrace.BindStorageBuffer("ProbeRetraceRays", retraceRays, RDGContentGuarantee::eConsumeProducedElements);
        retrace.BindStorageBuffer("ProbeRetraceResults", retraceResults, RDGContentGuarantee::eProducedElements);
        retrace.UseIndirectBuffer(retraceHeader);
        graph->AddComputePass(std::move(retrace)).RecordPassCommands([retraceHeader](RDGPassCmdEncoder& encoder) {
            encoder.DispatchIndirect(retraceHeader, 0);
        });
    }

    RDGComputePassDesc temporal;
    temporal.SetShaderProgramName(bricks ? "ProbeTemporalBricksSP" : "ProbeTemporalSP");
    temporal.SetPassTag("ProbeTemporal");
    temporal.allowCulling = false;
    BindInputs(temporal, sampler, uniforms, scene);
    temporal.BindStorageBuffer("ProbeInput", rawProbes, RDGContentGuarantee::eConsumeProducedElements);
    if (bricks)
    {
        temporal.BindStorageBuffer("ProbeRetraceHeads", retraceHeads, RDGContentGuarantee::eConsumeProducedElements);
        temporal.BindStorageBuffer("ProbeRetraceRays", retraceRays, RDGContentGuarantee::eConsumeProducedElements);
        temporal.BindStorageBuffer("ProbeRetraceResults", retraceResults, RDGContentGuarantee::eConsumeProducedElements);
    }
    temporal.BindStorageBuffer("ProbeCounters", counters);
    temporal.BindStorageBuffer("ProbeHistory", m_probes[1 - m_current]);
    temporal.BindStorageBuffer("ProbeOutput", m_probes[m_current], RDGContentGuarantee::eFullWrite);
    temporal.BindStorageBuffer("ProbeSubtilesPrevious", m_subtiles[1 - m_current]);
    Dispatch(std::move(temporal), slotGroups);
    RDGBuffer integratedProbes = resources->ImportBuffer(m_probes[m_current]);
    if (voxelGI.GetSettings().filter)
    {
        probeDesc.name              = "probe_filtered_sh";
        const RDGBuffer    filtered = resources->CreateBuffer(probeDesc);
        RDGComputePassDesc filter;
        filter.SetShaderProgramName("ProbeFilterSP");
        filter.SetPassTag("ProbeFilter");
        BindInputs(filter, sampler, uniforms, scene);
        filter.BindStorageBuffer("ProbeInput", integratedProbes);
        filter.BindStorageBuffer("ProbeOutput", filtered, RDGContentGuarantee::eFullWrite);
        filter.BindStorageBuffer("ProbeSubtiles", m_subtiles[m_current]);
        Dispatch(std::move(filter), slotGroups);
        integratedProbes = filtered;
    }

    RDGTextureDesc desc;
    desc.texFormat.dimension = TextureDimension::e2D;
    desc.texFormat.depth     = 1;
    desc.texFormat.width     = m_extent.x;
    desc.texFormat.height    = m_extent.y;
    desc.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
    desc.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
    desc.name     = "probe_raw_sky";
    m_rawSky      = resources->CreateTexture(desc);
    desc.name     = "probe_raw_bounce";
    m_rawBounce   = resources->CreateTexture(desc);
    desc.name     = "probe_raw_specular";
    m_rawSpecular = resources->CreateTexture(desc);
    RDGComputePassDesc integrate;
    integrate.SetShaderProgramName("ProbeIntegrateSP");
    integrate.SetPassTag("ProbeIntegrate");
    BindInputs(integrate, sampler, uniforms, scene);
    integrate.BindStorageBuffer("ProbeInput", integratedProbes);
    integrate.BindStorageBuffer("ProbeSubtiles", m_subtiles[m_current]);
    integrate.BindStorageImage("probeRawSky", m_rawSky, RDGContentGuarantee::eFullWrite);
    integrate.BindStorageImage("probeRawBounce", m_rawBounce, RDGContentGuarantee::eFullWrite);
    Dispatch(std::move(integrate), pixelGroups);

    desc.texFormat.width               = (m_extent.x + 1) / 2;
    desc.texFormat.height              = (m_extent.y + 1) / 2;
    desc.name                          = "probe_specular_samples";
    const RDGTexture specular          = resources->CreateTexture(desc);
    desc.name                          = "probe_specular_guides";
    desc.texFormat.format              = DataFormat::eR32G32B32A32SFloat;
    const RDGTexture specularGuide     = resources->CreateTexture(desc);
    desc.name                          = "probe_specular_positions";
    const RDGTexture specularPlace     = resources->CreateTexture(desc);
    desc.name                          = "probe_specular_surfaces";
    desc.texFormat.format              = DataFormat::eR32G32B32A32UInt;
    const RDGTexture   specularSurface = resources->CreateTexture(desc);
    RDGComputePassDesc specularTrace;
    specularTrace.SetShaderProgramName(ProbeTraceProgram("ProbeSpecularTrace", hardware, bricks));
    specularTrace.SetPassTag("ProbeSpecularTrace");
    BindInputs(specularTrace, sampler, uniforms, scene);
    BindTraceInputs(specularTrace, voxelGI, hardware);
    specularTrace.BindStorageImage("rawProbeSpecular", specular, RDGContentGuarantee::eFullWrite);
    specularTrace.BindStorageImage("rawProbeSpecularGuide", specularGuide, RDGContentGuarantee::eFullWrite);
    specularTrace.BindStorageImage("rawProbeSpecularSurface", specularSurface, RDGContentGuarantee::eFullWrite);
    specularTrace.BindStorageImage("rawProbeSpecularPosition", specularPlace, RDGContentGuarantee::eFullWrite);
    const glm::uvec2 blockGroups = (glm::uvec2(desc.texFormat.width, desc.texFormat.height) + glm::uvec2(7)) / 8u;
    Dispatch(std::move(specularTrace), blockGroups);
    // One ray per block flickers without a spatial filter: the samples are averaged on their
    // surfaces before the resolve spreads them to pixels.
    RDGTexture specularSamples = specular;
    if (voxelGI.GetSettings().filter)
    {
        desc.texFormat.format = DataFormat::eR16G16B16A16SFloat;
        for (uint32_t iteration = 0; iteration < kSpecularFilterPasses; ++iteration)
        {
            desc.name                 = NameID(fmt::format("probe_specular_filtered_{}", iteration));
            const RDGTexture   output = resources->CreateTexture(desc);
            RDGComputePassDesc filter;
            filter.SetShaderProgramName(NameID(fmt::format("ProbeSpecularFilter{}SP", iteration)));
            filter.SetPassTag(NameID(fmt::format("ProbeSpecularFilter{}", iteration)));
            BindInputs(filter, sampler, uniforms, scene);
            filter.BindSampledTexture("rawProbeSpecular", sampler, specularSamples);
            filter.BindSampledTexture("rawProbeSpecularSurface", sampler, specularSurface);
            filter.BindSampledTexture("rawProbeSpecularPosition", sampler, specularPlace);
            filter.BindStorageImage("filteredProbeSpecular", output, RDGContentGuarantee::eFullWrite);
            Dispatch(std::move(filter), blockGroups);
            specularSamples = output;
        }
    }
    RDGComputePassDesc resolve;
    resolve.SetShaderProgramName("ProbeSpecularResolveSP");
    resolve.SetPassTag("ProbeSpecularResolve");
    BindInputs(resolve, sampler, uniforms, scene);
    resolve.BindSampledTexture("rawProbeSpecular", sampler, specularSamples);
    resolve.BindSampledTexture("rawProbeSpecularGuide", sampler, specularGuide);
    resolve.BindStorageImage("probeRawSpecular", m_rawSpecular, RDGContentGuarantee::eFullWrite);
    Dispatch(std::move(resolve), pixelGroups);

    RDGComputePassDesc pixelTemporal;
    pixelTemporal.SetShaderProgramName("ProbePixelTemporalSP");
    pixelTemporal.SetPassTag("ProbePixelTemporal");
    pixelTemporal.allowCulling = false;
    BindInputs(pixelTemporal, sampler, uniforms, scene);
    pixelTemporal.BindSampledTexture("probeRawSky", sampler, m_rawSky);
    pixelTemporal.BindSampledTexture("probeRawBounce", sampler, m_rawBounce);
    pixelTemporal.BindSampledTexture("probeRawSpecular", sampler, m_rawSpecular);
    for (uint32_t texture = 0; texture < Count; ++texture)
    {
        pixelTemporal.BindSampledTexture(NameID(fmt::format("previousProbe{}", kHistoryNames[texture])), sampler,
                                         m_history[1 - m_current][texture]->GetDefaultView());
        pixelTemporal.BindStorageImage(NameID(fmt::format("nextProbe{}", kHistoryNames[texture])),
                                       m_history[m_current][texture]->GetDefaultView(), RDGContentGuarantee::eFullWrite);
        m_output[texture] = resources->ImportTexture(m_history[m_current][texture]);
    }
    Dispatch(std::move(pixelTemporal), pixelGroups);
    if (captureOutput != nullptr && captureReadback != nullptr)
    {
        RDGComputePassDesc capture;
        capture.SetShaderProgramName("ProbeCaptureSP");
        capture.SetPassTag("HybridCapture");
        BindInputs(capture, sampler, uniforms, scene);
        voxelGI.BindLightingInputs(capture);
        capture.BindSampledTexture("probeRawSky", sampler, m_rawSky);
        capture.BindSampledTexture("probeRawBounce", sampler, m_rawBounce);
        capture.BindSampledTexture("probeRawSpecular", sampler, m_rawSpecular);
        for (uint32_t texture = 0; texture < Count; ++texture)
        {
            capture.BindSampledTexture(NameID(fmt::format("nextProbe{}", kHistoryNames[texture])), sampler, m_output[texture]);
        }
        capture.BindStorageBuffer("HybridCapture", captureOutput, RDGContentGuarantee::eFullWrite);
        Dispatch(std::move(capture), pixelGroups);
        graph->AddTransferPass("ReadHybridCapture")
            .CopyBuffer(captureOutput, captureReadback, {0, 0, captureOutput->GetRequiredSize()})
            .NeverCull();
        if (m_statistics == nullptr)
        {
            RHIBufferCreateInfo info;
            info.size         = kCounterBytes;
            info.tag          = "probe_counters_readback";
            info.allocateType = RHIBufferAllocateType::eCPURead;
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eTransferDstBuffer);
            m_statistics = m_device->CreateBuffer(info);
        }
        if (m_statistics != nullptr)
        {
            graph->AddTransferPass("ReadProbeCounters")
                .CopyBuffer(counters, resources->ImportBuffer(m_statistics, RDGImportContents::eUndefined),
                            {0, 0, kCounterBytes})
                .NeverCull();
            m_statisticsRecorded = true;
        }
    }
    m_recorded = true;
}

DebugOutputDescription ScreenProbeGather::BuildDebugView(const RenderView&     view,
                                                         const DebugSelection& selection,
                                                         RHISampler*           sampler) const
{
    DebugOutputDescription description;
    description.output = selection.output;
    description.available =
        m_recorded && selection.output >= DebugOutput::eGIRawSky && selection.output <= DebugOutput::eGIHitMiss;
    description.reason = "Per-pixel rejection reasons are unavailable for screen probes.";
    if (description.available)
    {
        description.reason.clear();
        description.width             = view.width;
        description.height            = view.height;
        description.format            = DataFormat::eR16G16B16A16SFloat;
        const char* interpretations[] = {
            "Integrated diffuse sky before pixel temporal accumulation, displayed as sRGB.",
            "Reconstructed diffuse sky, displayed as sRGB.",
            "Pixel diffuse history length, black at zero and white at four frames.",
            "Resolved specular escape fraction: black is blocked, white escapes, blue has no sample."};
        const uint32_t mode        = static_cast<uint32_t>(selection.output) - static_cast<uint32_t>(DebugOutput::eGIRawSky);
        description.interpretation = interpretations[mode];
        DebugVisualizationData data;
        data.selection.x           = mode;
        data.range.y               = 4.0f;
        RDGGraphicsPassDesc pass   = MakeDebugVisualizationPass(view, "ProbeDebugSP", data);
        const RDGTexture    source = selection.output == DebugOutput::eGIRawSky  ? m_rawSky
                                   : selection.output == DebugOutput::eGIHitMiss ? m_rawSpecular
                                                                                 : m_output[Sky];
        pass.BindSampledTexture("sourceImage", sampler, source);
        pass.BindSampledTexture("pixelHistory", sampler, m_output[Position]);
        AddDebugVisualizationPass(*m_device->GetCurrentFrameRDG(), std::move(pass));
    }
    return description;
}

ScreenProbeStatistics ScreenProbeGather::ReadStatistics() const
{
    ScreenProbeStatistics statistics;
    statistics.uniformProbes     = m_uniforms.grid.x * m_uniforms.grid.y;
    statistics.adaptiveCapacity  = AdaptiveCapacity();
    statistics.adaptiveAllocated = m_lastDemand.x;
    statistics.adaptiveRequested = m_lastDemand.y;
    if (m_statistics != nullptr && m_statisticsRecorded)
    {
        const uint32_t* counters = reinterpret_cast<const uint32_t*>(m_statistics->Map());
        if (counters != nullptr)
        {
            statistics.adaptiveAllocated = counters[0];
            statistics.adaptiveRequested = counters[1];
            m_statistics->Unmap();
        }
    }
    return statistics;
}

void ScreenProbeGather::OnRenderGraphExecuted(RenderFrameResult result)
{
    const bool submitted = m_recorded && result == RenderFrameResult::eSubmitted;
    if (submitted)
    {
        m_needsClear    = false;
        m_current       = 1 - m_current;
        m_historyFrames = m_uniforms.grid.w != 0 ? std::min(m_historyFrames + 1u, kMaxHistoryFrames) : 1u;
        // The history copy was submitted; a declined frame records it again.
        for (RHIBuffer*& replaced : m_resizedFrom)
        {
            m_device->DestroyBuffer(replaced);
            replaced = nullptr;
        }
        m_resizedSlots = 0;
    }
    if (m_recordedDemand != nullptr && submitted)
    {
        m_pendingDemand.push_back({m_recordedDemand, m_device->CaptureResourceRetirement()});
    }
    else if (m_recordedDemand != nullptr && result == RenderFrameResult::eDeclined)
    {
        m_freeDemand.push_back(m_recordedDemand);
    }
    else
    {
        // A failed submission may still write the copy: never reuse it.
        m_device->DestroyBuffer(m_recordedDemand);
    }
    m_recordedDemand = nullptr;
    m_recorded       = false;
}

void ScreenProbeGather::Destroy()
{
    for (uint32_t side = 0; side < 2; ++side)
    {
        m_device->DestroyBuffer(m_probes[side]);
        m_device->DestroyBuffer(m_subtiles[side]);
        m_device->DestroyBuffer(m_resizedFrom[side]);
        m_probes[side]      = nullptr;
        m_subtiles[side]    = nullptr;
        m_resizedFrom[side] = nullptr;
        for (uint32_t texture = 0; texture < Count; ++texture)
        {
            m_device->DestroyTexture(m_history[side][texture]);
            m_history[side][texture] = nullptr;
        }
    }
    m_device->DestroyBuffer(m_statistics);
    m_statistics = nullptr;
    m_device->DestroyBuffer(m_retraceHeader);
    m_retraceHeader      = nullptr;
    m_statisticsRecorded = false;
    for (const PendingDemand& pending : m_pendingDemand)
    {
        m_device->DestroyBuffer(pending.readback);
    }
    for (RHIBuffer* readback : m_freeDemand)
    {
        m_device->DestroyBuffer(readback);
    }
    m_device->DestroyBuffer(m_recordedDemand);
    m_pendingDemand.clear();
    m_freeDemand.clear();
    m_recordedDemand  = nullptr;
    m_lastDemand      = glm::uvec2(0);
    m_resizedSlots    = 0;
    m_lowDemandFrames = 0;
    m_lowDemandRows   = 0;
    m_extent          = glm::uvec2(0);
    m_current         = 0;
    m_historyFrames   = 0;
    m_needsClear      = true;
    m_recorded        = false;
}
} // namespace zen::rc
