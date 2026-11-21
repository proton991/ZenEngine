#include "Graphics/RenderCore/V2/Renderer/ScreenProbeGI.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"

namespace zen::rc
{
static uint64_t ProbeTraceBytesPerProbe(bool useSpecular)
{
    return ZEN_SCREEN_PROBE_RAYS * 8u * sizeof(float)
         + (useSpecular ? ZEN_SCREEN_PROBE_SPECULAR_RAYS * 6u * sizeof(float) : 0u);
}

static uint32_t HitBatchesPerProbe(bool useSpecular)
{
    return useSpecular ? ZEN_SCREEN_PROBE_MAX_BATCHES : ZEN_SCREEN_PROBE_DIFFUSE_BATCHES;
}

ScreenProbeGI::ScreenProbeGI(RenderDevice* device) : m_device(device), m_glossy(device) {}
ScreenProbeGI::~ScreenProbeGI()
{
    Destroy();
}

void ScreenProbeGI::Destroy()
{
    m_glossy.Destroy();
    for (uint32_t side = 0; side < ZEN_SCREEN_PROBE_HISTORY_FRAMES; ++side)
    {
        m_device->DestroyBuffer(m_probes[side]);
        m_device->DestroyBuffer(m_lighting[side]);
        m_device->DestroyBuffer(m_specular[side]);
        m_device->DestroyBuffer(m_cdf[side]);
        m_probes[side]   = nullptr;
        m_lighting[side] = nullptr;
        m_specular[side] = nullptr;
        m_cdf[side]      = nullptr;
    }
    m_useSpecular   = false;
    m_extent        = glm::uvec2(0);
    m_uniforms.grid = glm::uvec4(0);
    m_needsClear    = true;
    m_validHistory  = 0;
    m_current       = 0;
    m_recorded      = false;
}

bool ScreenProbeGI::Prepare(glm::uvec2 extent, bool useSpecular, bool hardwareTracing)
{
    // Spend the diffuse-only memory headroom on uncovered surfaces within each base tile.
    const uint32_t    spacing       = ZEN_SCREEN_PROBE_SPACING;
    const uint32_t    adaptiveTiles = useSpecular ? ZEN_SCREEN_PROBE_ADAPTIVE_TILES : 1u;
    const glm::uvec2  tiles         = (extent + glm::uvec2(spacing - 1u)) / spacing;
    const uint64_t    base          = uint64_t(tiles.x) * tiles.y;
    const uint64_t    count         = base + (base + adaptiveTiles - 1u) / adaptiveTiles;
    const RHIGPUInfo& info          = m_device->GetGPUInfo();
    const uint64_t    hitBytes      = hardwareTracing ? HitBatchesPerProbe(useSpecular) * ZEN_SCREEN_PROBE_RAYS
                                                            * ZEN_SCREEN_PROBE_HIT_WORDS * sizeof(uint32_t)
                                                      : 0;
    const uint64_t    bytesPerProbe = std::max(ProbeTraceBytesPerProbe(useSpecular), hitBytes);
    bool ready = count != 0 && count <= uint64_t(info.maxComputeWorkGroupCount[0]) * info.maxComputeWorkGroupCount[1]
              && count <= info.maxStorageBufferRange / bytesPerProbe;
    if (ready && (m_extent != extent || m_useSpecular != useSpecular))
    {
        Destroy();
        for (uint32_t side = 0; side < ZEN_SCREEN_PROBE_HISTORY_FRAMES && ready; ++side)
        {
            m_probes[side]   = m_device->CreateStorageBuffer(uint32_t(count * 48u), nullptr, "ScreenProbes");
            m_lighting[side] = m_device->CreateStorageBuffer(uint32_t(count * ZEN_SCREEN_PROBE_LIGHTING_VECTORS * sizeof(Vec4)),
                                                             nullptr, "ScreenProbeLighting");
            if (useSpecular)
            {
                m_specular[side] = m_device->CreateStorageBuffer(uint32_t(count * ZEN_SCREEN_PROBE_SPECULAR_TEXELS * 8u),
                                                                 nullptr, "ScreenProbeSpecular");
            }
            m_cdf[side] = m_device->CreateStorageBuffer(uint32_t(count * ZEN_SCREEN_PROBE_PROPOSAL_WORDS * sizeof(uint32_t)),
                                                        nullptr, "ScreenProbeCDF");
            ready = m_probes[side] != nullptr && m_lighting[side] != nullptr && (!useSpecular || m_specular[side] != nullptr)
                 && m_cdf[side] != nullptr;
        }
        if (ready && useSpecular)
        {
            ready = m_glossy.Prepare(extent);
        }
        if (ready)
        {
            m_useSpecular            = useSpecular;
            m_extent                 = extent;
            m_uniforms.grid          = glm::uvec4(tiles, uint32_t(base), uint32_t(count));
            m_uniforms.spacing       = spacing;
            m_uniforms.adaptiveTiles = adaptiveTiles;
        }
    }
    if (!ready)
    {
        Destroy();
        LOGE("Unable to allocate screen probes for {}x{}", extent.x, extent.y);
    }
    return ready;
}

void ScreenProbeGI::Bind(RDGPassDescBase& pass, const HybridGIUniformData& receiver, RHISampler* sampler) const
{
    BindHybridReceiverGuides(pass, sampler, receiver);
    pass.BindValue("uScreenProbeData", m_uniforms);
    RHIBuffer* previousProbes[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
    RHIBuffer* previousLighting[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
    for (uint32_t history = 0; history < ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1; ++history)
    {
        const uint32_t side = (m_current + ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1u - history) % ZEN_SCREEN_PROBE_HISTORY_FRAMES;
        previousProbes[history]   = m_probes[side];
        previousLighting[history] = m_lighting[side];
    }
    pass.BindStorageBuffer("ScreenProbes", m_probes[m_current]);
    pass.BindStorageBuffer("PreviousScreenProbes", MakeVecView(previousProbes));
    pass.BindStorageBuffer("ScreenProbeLighting", m_lighting[m_current]);
    pass.BindStorageBuffer("PreviousScreenProbeLighting", MakeVecView(previousLighting));
}

void ScreenProbeGI::BindHistory(RDGPassDescBase& pass,
                                const char*      currentName,
                                const char*      previousName,
                                RHIBuffer* const (&buffers)[ZEN_SCREEN_PROBE_HISTORY_FRAMES],
                                bool             initialize) const
{
    RHIBuffer* previous[ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1];
    for (uint32_t history = 0; history < ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1; ++history)
    {
        const uint32_t side = (m_current + ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1u - history) % ZEN_SCREEN_PROBE_HISTORY_FRAMES;
        previous[history]   = buffers[side];
    }
    pass.BindStorageBuffer(currentName, buffers[m_current], RDGContentGuarantee::eFullWrite);
    pass.BindStorageBuffer(previousName, MakeVecView(previous),
                           initialize ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);
}

void ScreenProbeGI::Dispatch(RenderGraph& graph, RDGComputePassDesc&& pass, uint32_t groups) const
{
    const uint32_t   width = std::min(groups, 256u);
    const glm::uvec2 extent(width, (groups + width - 1u) / width);
    graph.AddComputePass(std::move(pass)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(extent.x, extent.y, 1);
    });
}

void ScreenProbeGI::BindTrace(RDGPassDescBase&           pass,
                              VoxelGIRenderer&           voxelGI,
                              RenderScene&               scene,
                              const WorldRadianceCache&  cache,
                              const HybridGIUniformData& receiver,
                              RHISampler*                sampler) const
{
    Bind(pass, receiver, sampler);
    cache.Bind(pass);
    pass.BindStorageBuffer("ScreenProbeCDF", m_cdf[m_current]);
    pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
    if (voxelGI.UsesRayTracingPipeline())
    {
        voxelGI.BindHardwareRayInputs(pass, false);
        voxelGI.BindLightingInputs(pass, true);
        voxelGI.BindEnvironmentSamplingInputs(pass);
    }
    else
    {
        voxelGI.BindNativeFieldInputs(pass);
        // Probe rays importance-sample the environment in both tiers.
        voxelGI.BindEnvironmentSamplingInputs(pass);
    }
}

void ScreenProbeGI::AddTracePass(RDGRayTracingPassDesc&& pass, uint32_t width, uint32_t height) const
{
    SetHybridRayTracingGroups(pass, 2);
    m_device->GetCurrentFrameRDG()
        ->AddRayTracingPass(std::move(pass))
        .RecordPassCommands([width, height](RDGPassCmdEncoder& encoder) { encoder.TraceRays(width, height, 1); });
}

void ScreenProbeGI::RecordClear(RenderGraph& graph) const
{
    RDGComputePassDesc clear;
    clear.SetShaderProgramName(m_useSpecular ? "ScreenProbeClearSP" : "ScreenProbeClearDiffuseSP");
    clear.SetPassTag("ScreenProbeInitialize");
    clear.BindValue("uScreenProbeData", m_uniforms);
    BindHistory(clear, "ScreenProbes", "PreviousScreenProbes", m_probes, true);
    BindHistory(clear, "ScreenProbeLighting", "PreviousScreenProbeLighting", m_lighting, true);
    BindHistory(clear, "ScreenProbeCDF", "PreviousScreenProbeCDF", m_cdf, true);
    if (m_useSpecular)
    {
        BindHistory(clear, "ScreenProbeSpecular", "PreviousScreenProbeSpecular", m_specular, true);
    }
    Dispatch(graph, std::move(clear), GetProbeCount());
    if (m_useSpecular)
    {
        m_glossy.RecordClear(graph);
    }
}

bool ScreenProbeGI::InitializeHistories()
{
    bool ready = true;
    if (m_needsClear && GetProbeCount() != 0)
    {
        RenderGraph& graph = *m_device->GetCurrentFrameRDG();
        ready              = graph.Begin();
        if (ready)
        {
            RecordClear(graph);
            ready        = graph.End() && m_device->ExecuteRenderGraph(graph);
            m_needsClear = !ready;
        }
    }
    return ready;
}

void ScreenProbeGI::Record(VoxelGIRenderer&           voxelGI,
                           RenderScene&               scene,
                           const WorldRadianceCache&  cache,
                           const HybridGIUniformData& receiver,
                           const Mat4&                projectionView,
                           RHISampler*                sampler,
                           bool                       reset)
{
    RenderGraph&           graph     = *m_device->GetCurrentFrameRDG();
    RDGResourceManager&    resources = *graph.GetResourceManager();
    const VoxelGISettings& settings  = voxelGI.GetSettings();
    const uint32_t         count     = GetProbeCount();
    m_uniforms.projectionView        = projectionView;
    m_projectionViews[m_current]     = projectionView;
    for (uint32_t history = 0; history < ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1; ++history)
    {
        const uint32_t side = (m_current + ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1u - history) % ZEN_SCREEN_PROBE_HISTORY_FRAMES;
        m_uniforms.previousProjectionViews[history] = m_projectionViews[side];
    }
    m_uniforms.control = Vec4(scene.GetVoxelSceneBounds().GetMaxExtent(), reset ? 0 : m_validHistory, settings.temporal ? 1 : 0,
                              settings.filter ? 1 : 0);
    if (m_needsClear)
    {
        RecordClear(graph);
    }
    RDGBufferDesc buffer;
    buffer.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    buffer.name                = "screen_probe_rays";
    buffer.size                = uint64_t(count) * ProbeTraceBytesPerProbe(m_useSpecular);
    const RDGBuffer rays       = resources.CreateBuffer(buffer);
    buffer.name                = "screen_probe_filtered";
    buffer.size                = uint64_t(count) * 8u * sizeof(Vec4);
    const RDGBuffer filtered   = resources.CreateBuffer(buffer);
    buffer.name                = "screen_probe_horizontal";
    const RDGBuffer horizontal = resources.CreateBuffer(buffer);


    RDGComputePassDesc place;
    place.SetShaderProgramName("ScreenProbePlaceSP");
    place.SetPassTag("ScreenProbePlacement");
    Bind(place, receiver, sampler);
    Dispatch(graph, std::move(place), (m_uniforms.grid.z + 63u) / 64u);
    // Adaptive support must observe the actual world-anchored base probes.
    // A separate pass makes those writes visible before selecting extra receivers.
    RDGComputePassDesc adaptive;
    adaptive.SetShaderProgramName("ScreenProbeAdaptiveSP");
    adaptive.SetPassTag("ScreenProbeAdaptive");
    Bind(adaptive, receiver, sampler);
    Dispatch(graph, std::move(adaptive), count - m_uniforms.grid.z);
    RDGComputePassDesc proposal;
    proposal.SetShaderProgramName("ScreenProbeProposalSP");
    proposal.SetPassTag("ScreenProbeProposal");
    Bind(proposal, receiver, sampler);
    cache.BindLighting(proposal);
    BindHistory(proposal, "ScreenProbeCDF", "PreviousScreenProbeCDF", m_cdf);
    proposal.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
    Dispatch(graph, std::move(proposal), count);

    const uint32_t bits = m_device->GetGPUInfo().supportBufferInt64Atomics ? 64 : 32;
    if (voxelGI.UsesRayTracingPipeline())
    {
        // Give each scheduled direction its own record from sampling through accumulation.
        const uint32_t slotBatches = HitBatchesPerProbe(m_useSpecular);
        const uint32_t slots       = count * ZEN_SCREEN_PROBE_RAYS * slotBatches;
        buffer.name                = "screen_probe_hits";
        buffer.size                = uint64_t(slots) * ZEN_SCREEN_PROBE_HIT_WORDS * sizeof(uint32_t);
        const RDGBuffer    hits    = resources.CreateBuffer(buffer);
        RDGComputePassDesc sampleHits;
        sampleHits.SetShaderProgramName(NameID(fmt::format("ScreenProbeSampleHits{}SP", bits)));
        sampleHits.SetPassTag("ScreenProbeSampleHits");
        BindTrace(sampleHits, voxelGI, scene, cache, receiver, sampler);
        sampleHits.BindStorageBuffer("ScreenProbeHits", hits, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(sampleHits), (count * ZEN_SCREEN_PROBE_RAYS + 63u) / 64u);
        RDGRayTracingPassDesc findHits;
        findHits.SetShaderProgramName(NameID(fmt::format("ScreenProbeTraceHits{}SP", bits)));
        findHits.SetPassTag("ScreenProbeTraceHits");
        BindTrace(findHits, voxelGI, scene, cache, receiver, sampler);
        findHits.BindStorageBuffer("ScreenProbeHits", hits);
        AddTracePass(std::move(findHits), ZEN_SCREEN_PROBE_RAYS, count * slotBatches);
        RDGComputePassDesc shadeHits;
        shadeHits.SetShaderProgramName(NameID(fmt::format("ScreenProbeShadeHits{}SP", bits)));
        shadeHits.SetPassTag("ScreenProbeShadeHits");
        BindTrace(shadeHits, voxelGI, scene, cache, receiver, sampler);
        shadeHits.BindStorageBuffer("ScreenProbeHits", hits);
        Dispatch(graph, std::move(shadeHits), (slots + 63u) / 64u);
        RDGComputePassDesc accumulate;
        accumulate.SetShaderProgramName("ScreenProbeAccumulateSP");
        accumulate.SetPassTag("ScreenProbeAccumulate");
        Bind(accumulate, receiver, sampler);
        accumulate.BindStorageBuffer("ScreenProbeCDF", m_cdf[m_current]);
        accumulate.BindStorageBuffer("ScreenProbeRays", rays, RDGContentGuarantee::eFullWrite);
        accumulate.BindStorageBuffer("ScreenProbeHits", hits);
        Dispatch(graph, std::move(accumulate), count);
    }
    else
    {
        RDGComputePassDesc trace;
        trace.SetShaderProgramName(NameID(fmt::format("ScreenProbeTraceField{}SP", bits)));
        trace.SetPassTag("ScreenProbeTrace");
        BindTrace(trace, voxelGI, scene, cache, receiver, sampler);
        trace.BindStorageBuffer("ScreenProbeRays", rays, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(trace), count);
    }
    RDGComputePassDesc integrate;
    integrate.SetShaderProgramName("ScreenProbeIntegrateSP");
    integrate.SetPassTag("ScreenProbeTemporal");
    Bind(integrate, receiver, sampler);
    cache.BindLighting(integrate);
    integrate.BindStorageBuffer("ScreenProbeCDF", m_cdf[m_current]);
    integrate.BindStorageBuffer("ScreenProbeRays", rays);
    Dispatch(graph, std::move(integrate), count);
    for (uint32_t axis = 0; axis < 2; ++axis)
    {
        RDGComputePassDesc filter;
        filter.SetShaderProgramName(axis == 0 ? "ScreenProbeFilterSP" : "ScreenProbeFilterVerticalSP");
        filter.SetPassTag(axis == 0 ? "ScreenProbeSpatialHorizontal" : "ScreenProbeSpatial");
        Bind(filter, receiver, sampler);
        if (axis != 0)
        {
            filter.BindStorageBuffer("HorizontalScreenProbeLighting", horizontal);
        }
        filter.BindStorageBuffer("FilteredScreenProbeLighting", axis == 0 ? horizontal : filtered,
                                 RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(filter), (count + 63u) / 64u);
    }
    RDGComputePassDesc gather;
    gather.SetShaderProgramName(m_useSpecular ? "ScreenProbeGatherSP" : "ScreenProbeGatherDiffuseSP");
    gather.SetPassTag("ScreenProbeGather");
    Bind(gather, receiver, sampler);
    voxelGI.BindLightingInputs(gather);
    gather.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
    gather.BindStorageBuffer("FilteredScreenProbeLighting", filtered);
    if (m_useSpecular)
    {
        RDGComputePassDesc specular;
        specular.SetShaderProgramName("ScreenProbeSpecularSP");
        specular.SetPassTag("ScreenProbePrefilter");
        Bind(specular, receiver, sampler);
        BindHistory(specular, "ScreenProbeSpecular", "PreviousScreenProbeSpecular", m_specular);
        specular.BindStorageBuffer("ScreenProbeRays", rays);
        Dispatch(graph, std::move(specular), count);
        buffer.name                         = "screen_probe_specular_filtered";
        buffer.size                         = uint64_t(count) * ZEN_SCREEN_PROBE_SPECULAR_TEXELS * 8u;
        const RDGBuffer    filteredSpecular = resources.CreateBuffer(buffer);
        RDGComputePassDesc specularFilter;
        specularFilter.SetShaderProgramName("ScreenProbeSpecularFilterSP");
        specularFilter.SetPassTag("ScreenProbeSpecularSpatial");
        Bind(specularFilter, receiver, sampler);
        specularFilter.BindStorageBuffer("ScreenProbeSpecular", m_specular[m_current]);
        specularFilter.BindStorageBuffer("FilteredScreenProbeSpecular", filteredSpecular, RDGContentGuarantee::eFullWrite);
        Dispatch(graph, std::move(specularFilter), count);

        m_glossy.Record(voxelGI, scene, cache, receiver, sampler, reset);
        m_glossy.BindGather(gather);
        gather.BindStorageBuffer("FilteredScreenProbeSpecular", filteredSpecular);
    }
    const char* names[] = {"probeSky", "probeBounce", "probeSpecular"};
    for (uint32_t signal = 0; signal < 3; ++signal)
    {
        RDGTextureDesc texture;
        texture.name                = names[signal];
        texture.texFormat.dimension = TextureDimension::e2D;
        texture.texFormat.width     = m_extent.x;
        texture.texFormat.height    = m_extent.y;
        texture.texFormat.depth     = 1;
        texture.texFormat.format    = DataFormat::eR16G16B16A16SFloat;
        texture.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eSampled);
        m_output[signal] = resources.CreateTexture(texture);
        gather.BindStorageImage(names[signal], m_output[signal], RDGContentGuarantee::eFullWrite);
    }
    const glm::uvec2 extent = (m_extent + glm::uvec2(ZEN_SCREEN_PROBE_SPACING - 1u)) / ZEN_SCREEN_PROBE_SPACING;
    graph.AddComputePass(std::move(gather)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(extent.x, extent.y, 1);
    });
    if (m_useSpecular)
    {
        m_glossy.RecordTrace(voxelGI, scene, cache, receiver, sampler, {}, m_output[2]);
    }
    m_recorded = true;
}

void ScreenProbeGI::OnRenderGraphExecuted(RenderFrameResult result)
{
    if (m_useSpecular)
    {
        m_glossy.OnRenderGraphExecuted(result);
    }
    if (m_recorded && result == RenderFrameResult::eSubmitted)
    {
        m_validHistory = std::min(uint32_t(m_uniforms.control.y) + 1u, uint32_t(ZEN_SCREEN_PROBE_HISTORY_FRAMES - 1));
        m_needsClear   = false;
        m_current      = (m_current + 1u) % ZEN_SCREEN_PROBE_HISTORY_FRAMES;
    }
    else if (result != RenderFrameResult::eDeclined && result != RenderFrameResult::eSubmitted)
    {
        Destroy();
    }
    m_recorded = false;
}
} // namespace zen::rc
