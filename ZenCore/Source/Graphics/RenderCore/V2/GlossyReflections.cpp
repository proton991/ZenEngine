#include "Math/Math.h"
#include "Utils/Errors.h"
#include "Graphics/RenderCore/V2/Renderer/GlossyReflections.h"
#include "Graphics/RenderCore/V2/Renderer/HybridGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/Shared/ScreenProbeGI.h"

namespace zen::rc
{
GlossyReflections::GlossyReflections(RenderDevice* device) : m_device(device) {}
GlossyReflections::~GlossyReflections()
{
    Destroy();
}
void GlossyReflections::Destroy()
{
    for (uint32_t side = 0; side < 2; ++side)
    {
        m_device->DestroyBuffer(m_history[side]);
        m_history[side] = nullptr;
    }
    m_extent   = glm::uvec2(0);
    m_valid    = false;
    m_recorded = false;
}
uint64_t GlossyReflections::GetHistoryBytes() const
{
    return uint64_t(m_extent.x) * m_extent.y * 80u * 2u;
}
bool GlossyReflections::Prepare(glm::uvec2 extent)
{
    constexpr uint32_t spacing = ZEN_GLOSSY_RECEIVER_SPACING;
    const glm::uvec2   grid    = (extent + glm::uvec2(spacing - 1u)) / spacing;
    bool               ready   = true;
    if (m_extent != grid)
    {
        Destroy();
        const uint64_t bytes = uint64_t(grid.x) * grid.y * 80u;
        ready                = bytes != 0 && bytes <= m_device->GetGPUInfo().maxStorageBufferRange;
        for (uint32_t side = 0; side < 2 && ready; ++side)
        {
            m_history[side] = m_device->CreateStorageBuffer(uint32_t(bytes), nullptr, "GlossyHistory");
            ready           = m_history[side] != nullptr;
        }
        if (ready)
        {
            m_extent = grid;
        }
        else
        {
            Destroy();
            LOGE("Unable to allocate glossy reflection history for {}x{}", extent.x, extent.y);
        }
    }
    return ready;
}
void GlossyReflections::Bind(RDGPassDescBase&           pass,
                             const HybridGIUniformData& receiver,
                             RHISampler*                sampler,
                             const RenderScene&         scene,
                             uint32_t                   filterPass) const
{
    BindHybridReceiverGuides(pass, sampler, receiver);
    Uniforms uniforms = m_uniforms;
    uniforms.extent.w = filterPass;
    pass.BindValue("uGlossyData", uniforms);
    pass.BindValue("uSceneData", scene.GetSceneUniformData(), sizeof(SceneUniformData));
    pass.BindStorageBuffer("GlossyCurrent", m_history[m_current]);
    pass.BindStorageBuffer("GlossyPrevious", m_history[1u - m_current]);
}
void GlossyReflections::BindGather(RDGPassDescBase& pass) const
{
    pass.BindValue("uGlossyData", m_uniforms);
    pass.BindStorageBuffer("GlossyCurrent", m_history[m_current]);
    pass.BindStorageBuffer("GlossyPrevious", m_history[1u - m_current]);
    pass.BindStorageBuffer("GlossyFiltered", m_filtered);
}
void GlossyReflections::Dispatch(RDGComputePassDesc&& pass, glm::uvec2 extent) const
{
    const glm::uvec2 groups = (extent + glm::uvec2(7)) / 8u;
    m_device->GetCurrentFrameRDG()->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups.x, groups.y, 1);
    });
}
void GlossyReflections::BindTrace(RDGPassDescBase&           pass,
                                  VoxelGIRenderer&           voxelGI,
                                  RenderScene&               scene,
                                  const WorldRadianceCache&  cache,
                                  const HybridGIUniformData& receiver,
                                  RHISampler*                sampler,
                                  RDGBuffer                  raw,
                                  RDGTexture                 specular) const
{
    Bind(pass, receiver, sampler, scene);
    cache.Bind(pass);
    if (specular)
    {
        pass.BindStorageImage("probeSpecular", specular);
    }
    else
    {
        pass.BindStorageBuffer("GlossyRaw", raw, RDGContentGuarantee::eFullWrite);
    }
    if (voxelGI.UsesRayTracingPipeline())
    {
        voxelGI.BindHardwareRayInputs(pass, false);
        voxelGI.BindLightingInputs(pass, true);
        voxelGI.BindEnvironmentSamplingInputs(pass);
    }
    else
    {
        voxelGI.BindNativeFieldInputs(pass, false);
    }
}

void GlossyReflections::RecordTrace(VoxelGIRenderer&           voxelGI,
                                    RenderScene&               scene,
                                    const WorldRadianceCache&  cache,
                                    const HybridGIUniformData& receiver,
                                    RHISampler*                sampler,
                                    RDGBuffer                  raw,
                                    RDGTexture                 specular) const
{
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    VERIFY_EXPR(bool(raw) != bool(specular));
    const char*      stage  = specular ? "GlossyUncovered" : "GlossyTrace";
    const uint32_t   bits   = m_device->GetGPUInfo().supportBufferInt64Atomics ? 64u : 32u;
    const glm::uvec2 extent = specular ? glm::uvec2(receiver.resolution) : m_extent;
    if (voxelGI.UsesRayTracingPipeline())
    {
        RDGRayTracingPassDesc trace;
        trace.SetShaderProgramName(NameID(fmt::format("{}{}SP", stage, bits)));
        trace.SetPassTag(stage);
        BindTrace(trace, voxelGI, scene, cache, receiver, sampler, raw, specular);
        SetHybridRayTracingGroups(trace, 2);
        graph.AddRayTracingPass(std::move(trace)).RecordPassCommands([extent](RDGPassCmdEncoder& encoder) {
            encoder.TraceRays(extent.x, extent.y, 1);
        });
    }
    else
    {
        RDGComputePassDesc trace;
        trace.SetShaderProgramName(NameID(fmt::format("{}Field{}SP", stage, bits)));
        trace.SetPassTag(stage);
        BindTrace(trace, voxelGI, scene, cache, receiver, sampler, raw, specular);
        Dispatch(std::move(trace), extent);
    }
}

void GlossyReflections::RecordClear(RenderGraph& graph) const
{
    RDGComputePassDesc clear;
    clear.SetShaderProgramName("GlossyClearSP");
    clear.SetPassTag("GlossyInitialize");
    Uniforms uniforms{};
    uniforms.extent = glm::uvec4(m_extent, 0u, 0u);
    clear.BindValue("uGlossyData", uniforms);
    clear.BindStorageBuffer("GlossyCurrent", m_history[m_current], RDGContentGuarantee::eFullWrite);
    clear.BindStorageBuffer("GlossyPrevious", m_history[1u - m_current], RDGContentGuarantee::eFullWrite);
    const uint32_t groups = (m_extent.x * m_extent.y + 255u) / 256u;
    graph.AddComputePass(std::move(clear)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch(groups, 1, 1);
    });
}

void GlossyReflections::Record(VoxelGIRenderer&           voxelGI,
                               RenderScene&               scene,
                               const WorldRadianceCache&  cache,
                               const HybridGIUniformData& receiver,
                               RHISampler*                sampler,
                               bool                       reset)
{
    RenderGraph&           graph     = *m_device->GetCurrentFrameRDG();
    RDGResourceManager&    resources = *graph.GetResourceManager();
    const VoxelGISettings& settings  = voxelGI.GetSettings();
    m_uniforms.extent                = glm::uvec4(m_extent, m_valid && !reset ? 1u : 0u, 0u);
    const bool dualHistory           = receiver.reflection.w == 2u && settings.glossySurfaceHistory;
    m_uniforms.control               = Vec4(settings.temporal ? (dualHistory ? 2 : 1) : 0, settings.filter ? 1 : 0, .4f, .08f);
    RDGBufferDesc desc;
    desc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    desc.name                 = "glossy_raw";
    desc.size                 = uint64_t(m_extent.x) * m_extent.y * 32u;
    const RDGBuffer raw       = resources.CreateBuffer(desc);
    desc.name                 = "glossy_repaired";
    desc.size                /= 2u;
    const RDGBuffer repaired  = resources.CreateBuffer(desc);
    desc.name                 = "glossy_filtered";
    m_filtered                = resources.CreateBuffer(desc);
    RecordTrace(voxelGI, scene, cache, receiver, sampler, raw, {});
    RDGComputePassDesc temporal;
    temporal.SetShaderProgramName("GlossyTemporalSP");
    temporal.SetPassTag("GlossyTemporal");
    Bind(temporal, receiver, sampler, scene);
    temporal.BindStorageBuffer("GlossyRaw", raw);
    Dispatch(std::move(temporal), m_extent);
    if (receiver.reflection.w == 2u && settings.temporal)
    {
        RDGComputePassDesc clamp;
        clamp.SetShaderProgramName("GlossyClampSP");
        clamp.SetPassTag("GlossyClamp");
        Bind(clamp, receiver, sampler, scene);
        Dispatch(std::move(clamp), m_extent);
    }
    for (uint32_t filterPass : {1u, 2u})
    {
        RDGComputePassDesc filter;
        filter.SetShaderProgramName("GlossyFilterSP");
        filter.SetPassTag(filterPass == 1u ? "GlossyHistoryFix" : "GlossySpatial");
        Bind(filter, receiver, sampler, scene, filterPass);
        // The first pass reads history; its unused sampled input is still a defined buffer.
        filter.BindStorageBuffer("GlossyFiltered", filterPass == 1u ? raw : repaired);
        filter.BindStorageBuffer("GlossyOutput", filterPass == 1u ? repaired : m_filtered, RDGContentGuarantee::eFullWrite);
        Dispatch(std::move(filter), m_extent);
    }
    m_recorded = true;
}
void GlossyReflections::OnRenderGraphExecuted(RenderFrameResult result)
{
    if (m_recorded && result == RenderFrameResult::eSubmitted)
    {
        m_valid   = true;
        m_current = 1u - m_current;
    }
    else if (result != RenderFrameResult::eSubmitted && result != RenderFrameResult::eDeclined)
    {
        Destroy();
    }
    m_recorded = false;
}
} // namespace zen::rc
