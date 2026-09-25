#include "Graphics/RenderCore/V2/Renderer/DynamicVoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/ComputeDispatch.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include <cmath>

namespace zen::rc
{
namespace
{
bool StaticChunks(uint32_t count, const RHIGPUInfo& gpu, HeapVector<ComputeDispatchChunk>& chunks,
                   uint32_t itemsPerGroup = GI_QUERY_GROUP_SIZE)
{
    bool valid     = true;
    uint32_t first = 0;
    while (valid && first < count)
    {
        ComputeDispatchChunk chunk;
        valid = BuildComputeDispatchChunk(first, count - first, itemsPerGroup, gpu, chunk);
        if (valid)
        {
            chunks.push_back(chunk);
            first += chunk.itemCount;
        }
    }
    return valid;
}

void RecordStaticPass(RDGPassCmdEncoder& encoder,
                      const HeapVector<ComputeDispatchChunk>& chunks,
                      uint32_t face,
                      uint32_t control)
{
    for (const ComputeDispatchChunk& chunk : chunks)
    {
        encoder.SetPushConstants(glm::uvec4(chunk.firstItem, chunk.itemCount, face, control));
        encoder.Dispatch(chunk.groups.x, chunk.groups.y, chunk.groups.z);
    }
}

void AddStaticPass(RenderGraph& graph,
                   RDGComputePassDesc&& pass,
                   const HeapVector<ComputeDispatchChunk>& chunks,
                   uint32_t face    = 0,
                   uint32_t control = 0)
{
    pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    pass.independentDispatches = true;
    graph.AddComputePass(std::move(pass))
        .RecordPassCommands([chunks, face, control](RDGPassCmdEncoder& encoder) {
            RecordStaticPass(encoder, chunks, face, control);
        });
}
void RecordReceiverArguments(RDGPassCmdEncoder& encoder,
                             const HeapVector<ComputeDispatchChunk>& chunks)
{
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        for (uint32_t index = 0; index < chunks.size(); ++index)
        {
            const ComputeDispatchChunk& chunk = chunks[index];
            encoder.SetPushConstants(
                glm::uvec4(chunk.firstItem, chunk.itemCount, kind, kind * chunks.size() + index));
            encoder.Dispatch(1, 1, 1);
        }
    }
}
void RecordReceiverGather(RDGPassCmdEncoder& encoder,
                          const HeapVector<ComputeDispatchChunk>& chunks,
                          RHIBuffer* arguments,
                          uint32_t face,
                          uint32_t kind)
{
    for (uint32_t index = 0; index < chunks.size(); ++index)
    {
        const ComputeDispatchChunk& chunk = chunks[index];
        encoder.SetPushConstants(
            glm::uvec4(chunk.firstItem, chunk.itemCount, face, kind == 0 ? GI_STATIC : GI_DYNAMIC));
        encoder.DispatchIndirect(arguments,
                                 (kind * static_cast<uint32_t>(chunks.size()) + index) * 16);
    }
}
} // namespace

bool DynamicVoxelGIRenderer::Init(const DynamicVoxelGISettings& settings,
                                  bool averaged,
                                  uint64_t receiverSurfaceBytes,
                                  uint64_t retiringBytes)
{
    bool valid = m_status != nullptr;
    if (!valid)
    {
        uint32_t capacity             = 0;
        uint64_t peak                 = 0;
        const GIResourceStatus status =
            PlanStaticVoxelGIResources(settings.resolution, averaged, settings.memoryBudgetBytes,
                                       m_device->GetGPUInfo(), capacity, peak, receiverSurfaceBytes,
                                       retiringBytes, settings.compactCache, settings.raysPerFace);
        valid = (settings.raysPerFace == 32 || settings.raysPerFace == 64 ||
                 settings.raysPerFace == 128) &&
            static_cast<uint32_t>(settings.temporal) <= 2 &&
            settings.backend != VoxelGIQueryBackend::eHardwareRT &&
            status == GIResourceStatus::eSuccess &&
            (settings.neighborRadius == 1 || settings.neighborRadius == 2) &&
            StaticChunks(settings.resolution * settings.resolution * settings.resolution,
                         m_device->GetGPUInfo(), m_workChunks, 1);
        if (valid)
        {
            m_resourceBytes = peak;
            SetFiltering(settings.temporal, settings.spatialFilter);
            SetLighting(settings.analyticLighting, settings.environmentLighting,
                        settings.emissiveLighting);
            m_uniform.volume =
                glm::uvec4(settings.resolution, capacity, settings.neighborRadius, 0);
            RHIBufferCreateInfo buffer;
            buffer.allocateType = RHIBufferAllocateType::eGPU;
            buffer.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer,
                                       RHIBufferUsageFlagBits::eTransferSrcBuffer);
            m_work.cache       = glm::uvec4(settings.compactCache ? 1u : 0u, 0, 0, 0);
            m_uniform.sampling = glm::uvec4(settings.raysPerFace, 0, 0, 0);
            buffer.size        = capacity * m_uniform.sampling.x * GetCacheStride();
            buffer.tag  = "static_gi_hits";
            TextureFormat volume;
            volume.dimension = TextureDimension::e3D;
            volume.format    = DataFormat::eR16G16B16A16SFloat;
            volume.width = volume.height = volume.depth = settings.resolution;
            volume.mipmaps                              = 1;
            for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
            {
                m_hits[face] = m_device->CreateBuffer(buffer);
                m_raw[face] =
                    m_device->CreateTextureStorage(volume, {.copyUsage = true}, "static_gi_raw");
                m_padded[face] =
                    m_device->CreateTextureStorage(volume, {.copyUsage = true}, "static_gi_padded");
                m_dynamicRaw[face] =
                    m_device->CreateTextureStorage(volume, {.copyUsage = true}, "dynamic_gi_raw");
                m_dynamicFiltered[face] = m_device->CreateTextureStorage(
                    volume, {.copyUsage = true}, "dynamic_gi_filtered");
                TextureFormat historyVolume = volume;
                historyVolume.format        = DataFormat::eR32G32B32A32SFloat;
                for (uint32_t kind = 0; kind < 2; ++kind)
                {
                    m_history[kind][face] = m_device->CreateTextureStorage(
                        historyVolume, {.copyUsage = true}, "gi_history");
                    valid = valid && m_history[kind][face] != nullptr;
                }
                valid = valid && m_dynamicRaw[face] != nullptr && m_hits[face] != nullptr &&
                    m_dynamicFiltered[face] != nullptr && m_raw[face] != nullptr &&
                    m_padded[face] != nullptr;
            }
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                m_senderEnvironment[kind] = m_device->CreateTextureStorage(
                    volume, {.copyUsage = true}, "gi_sender_environment");
                valid = valid && m_senderEnvironment[kind] != nullptr;
            }
            buffer.size =
                settings.resolution * settings.resolution * settings.resolution * sizeof(uint32_t);
            buffer.tag                   = "static_gi_light_mask";
            m_lightMask                  = m_device->CreateBuffer(buffer);
            buffer.tag                   = "dynamic_gi_light_mask";
            m_dynamicLightMask           = m_device->CreateBuffer(buffer);
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                buffer.tag            = "gi_receiver_flags";
                m_receiverFlags[kind] = m_device->CreateBuffer(buffer);
                buffer.tag            = "gi_receiver_list";
                m_receiverLists[kind] = m_device->CreateBuffer(buffer);
                valid =
                    valid && m_receiverFlags[kind] != nullptr && m_receiverLists[kind] != nullptr;
            }
            buffer.size = sizeof(glm::uvec4);
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                buffer.size =
                    uint64_t(settings.resolution) * settings.resolution * settings.resolution * 16;
                buffer.tag              = "gi_sender_positions";
                m_senderPositions[kind] = m_device->CreateBuffer(buffer);
                valid                   = valid && m_senderPositions[kind] != nullptr;
                buffer.tag              = "gi_history_metadata";
                m_historyMetadata[kind] = m_device->CreateBuffer(buffer);
                valid                   = valid && m_historyMetadata[kind] != nullptr;
            }
            buffer.size             = sizeof(glm::uvec4);
            buffer.tag              = "gi_work_counts";
            m_workCounts            = m_device->CreateBuffer(buffer);
            const uint32_t commands = 2 * static_cast<uint32_t>(m_workChunks.size());
            buffer.size = commands * 16;
            buffer.tag  = "gi_receiver_arguments";
            buffer.usageFlags.SetFlag(RHIBufferUsageFlagBits::eIndirectBuffer);
            m_indirect = m_device->CreateBuffer(buffer);
            buffer.usageFlags.ClearFlag(RHIBufferUsageFlagBits::eIndirectBuffer);
            const RHIGPUInfo& gpu = m_device->GetGPUInfo();
            m_work.limits         = glm::uvec4(
                std::min(gpu.maxComputeWorkGroupCount[0], uint32_t(INT32_MAX)),
                std::min(gpu.maxComputeWorkGroupCount[1], uint32_t(INT32_MAX)),
                std::min(gpu.maxComputeWorkGroupCount[2], uint32_t(INT32_MAX)), commands);
            buffer.size                  = sizeof(glm::uvec4);
            buffer.tag                   = "static_gi_status";
            m_status                     = m_device->CreateBuffer(buffer);
            buffer.tag                   = "gi_light_mask_status";
            m_lightMaskStatus            = m_device->CreateBuffer(buffer);
            RHISamplerCreateInfo sampler = RHISamplerCreateInfo::CreateLinearRepeat();
            sampler.repeatU = sampler.repeatV = sampler.repeatW =
                RHISamplerRepeatMode::eClampToEdge;
            m_sampler = m_device->CreateSampler(sampler);
            m_surfaceSampler = m_device->CreateSampler(RHISamplerCreateInfo{});
            valid            = valid && m_lightMask != nullptr && m_status != nullptr &&
                m_sampler != nullptr && m_workCounts != nullptr && m_indirect != nullptr &&
                m_dynamicLightMask != nullptr && m_surfaceSampler != nullptr &&
                m_lightMaskStatus != nullptr;
            LOGI(
                "Dynamic voxel GI: {} cached static receivers, {} bytes transition peak; hits=6x{} bytes, face volumes={} bytes, receiver surfaces={} bytes",
                capacity, peak, uint64_t(capacity) * m_uniform.sampling.x * GetCacheStride(),
                uint64_t(settings.resolution) * settings.resolution * settings.resolution * 400,
                receiverSurfaceBytes);
        }
        else
        {
            LOGW("Static voxel GI preflight rejected: status {}, peak {} bytes",
                 static_cast<uint32_t>(status), peak);
        }
        if (!valid)
        {
            Destroy();
        }
    }
    return valid;
}

void DynamicVoxelGIRenderer::BindFrameInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uStaticGI", m_uniform);
    pass.BindValue("uGIWork", m_work);
}

void DynamicVoxelGIRenderer::SetFiltering(GITemporalMode temporal, bool spatial)
{
    const glm::uvec2 settings(static_cast<uint32_t>(temporal), spatial ? 1 : 0);
    if (glm::uvec2(m_filter.control) != settings)
    {
        m_filter.control.x = settings.x;
        m_filter.control.y = settings.y;
        m_historyValid     = false;
    }
}

void DynamicVoxelGIRenderer::SetLighting(bool analytic, bool environment, bool emissive)
{
    const glm::uvec3 enabled(analytic ? 1 : 0, environment ? 1 : 0, emissive ? 1 : 0);
    if (glm::uvec3(m_lighting.enabled) != enabled)
    {
        m_lighting.enabled = glm::uvec4(enabled, 0);
        m_historyValid     = false;
        m_environmentValid = false;
    }
}

void DynamicVoxelGIRenderer::PrepareLightMasks(const StaticVoxelGIInputs& inputs,
                                               const GIVisibilityInfo& visibility,
                                               const SceneUniformData& scene,
                                               bool shadows)
{
    const LightMaskState current{visibility,
                                 scene,
                                 inputs.grid.minimumCellSize,
                                 inputs.listGeneration,
                                 shadows,
                                 m_lighting.enabled.x != 0,
                                 inputs.dynamicOwner != nullptr,
                                 inputs.scene != nullptr && inputs.shadowMaps != nullptr};
    const LightMaskState& previous = m_lightMaskState;
    // Any occluder change can affect every sender. A provider generation includes
    // opacity changes; keep this conservative even for surface-only generations.
    const bool rebuild = !m_lightMasksValid ||
        current.visibility.backend != previous.visibility.backend ||
        current.visibility.generation != previous.visibility.generation ||
        current.visibility.precision != previous.visibility.precision ||
        current.visibility.grid.minimumCellSize != previous.visibility.grid.minimumCellSize ||
        current.visibility.grid.dimensions != previous.visibility.grid.dimensions ||
        current.minimumCellSize != previous.minimumCellSize ||
        current.listGeneration != previous.listGeneration || current.shadows != previous.shadows ||
        current.analytic != previous.analytic || current.dynamicInputs != previous.dynamicInputs ||
        current.meshLightVisibility != previous.meshLightVisibility;
    uint32_t updates = rebuild ? UINT32_MAX : 0;
    for (uint32_t slot = 0; slot < MaxSceneLights && !rebuild; ++slot)
    {
        const bool enabled    = slot < static_cast<uint32_t>(scene.lightInfo.x);
        const bool wasEnabled = slot < static_cast<uint32_t>(previous.scene.lightInfo.x);
        const GPULight& light = scene.lights[slot];
        const GPULight& old   = previous.scene.lights[slot];
        if (enabled != wasEnabled ||
            (enabled &&
             (light.positionRange != old.positionRange ||
              light.directionType != old.directionType || light.coneShadow != old.coneShadow)))
        {
            updates |= uint32_t(1) << slot;
        }
    }
    m_lighting.enabled.w = updates;
    // Like the other recorded keys, this is usable only after successful handoff.
    m_lightMaskState = current;
}

bool DynamicVoxelGIRenderer::BuildEnvironmentGraph(const StaticVoxelGIInputs& inputs,
                                                   const GIVisibilityProvider& provider,
                                                   const SceneUniformData& scene,
                                                   const HeapVector<ComputeDispatchChunk>& cells)
{
    bool valid         = true;
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(provider.GetInfo().backend == GIVisibilityBackend::eVoxelDDA ?
                                      "GISenderEnvironmentDDASP" :
                                      "GISenderEnvironmentReferenceSP");
        pass.SetPassTag(kind == 0 ? "GIStaticSenderEnvironment" : "GIDynamicSenderEnvironment");
        valid = provider.BindQueryInputs(pass) && valid;
        BindFrameInputs(pass);
        pass.BindValue("uSceneData", scene);
        pass.BindSampledTexture("environmentMap", inputs.environmentSampler, inputs.environment);
        pass.BindStorageBuffer("SenderMap",
                               kind == 0 || inputs.dynamicMap == nullptr ? inputs.gridToList :
                                                                           inputs.dynamicMap);
        pass.BindStorageImage(
            "senderNormal",
            (kind == 0 || inputs.dynamicNormal == nullptr ? inputs.normal : inputs.dynamicNormal)
                ->GetDefaultView());
        pass.BindStorageImage("senderEnvironment", m_senderEnvironment[kind]->GetDefaultView(),
                              RDGContentGuarantee::eFullWrite);
        pass.BindStorageBuffer("GIStatus", m_status);
        AddStaticPass(graph, std::move(pass), cells, 0, kind == 0 ? GI_STATIC : GI_DYNAMIC);
    }
    return valid;
}

void DynamicVoxelGIRenderer::BuildHistoryMetadata(const StaticVoxelGIInputs& inputs,
                                                  const HeapVector<ComputeDispatchChunk>& cells,
                                                  bool clear)
{
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(clear ? "GIHistoryClearSP" : "GIHistoryMetadataSP");
        pass.SetPassTag(clear ? "GIClearHistoryMetadata" : "GIUpdateHistoryMetadata");
        BindFrameInputs(pass);
        pass.BindStorageBuffer("HistoryMetadata", m_historyMetadata[kind],
                               clear ? RDGContentGuarantee::eFullWrite :
                                       RDGContentGuarantee::eNone);
        if (!clear)
        {
            pass.BindValue("uGIFilter", m_filter);
            pass.BindStorageBuffer("ReceiverFlags", m_receiverFlags[kind]);
            pass.BindStorageBuffer("GIStatus", m_status);
            pass.BindStorageImage("history", m_history[kind][0]->GetDefaultView());
            pass.BindStorageImage(
                "owner",
                (kind == 0 || inputs.dynamicOwner == nullptr ? inputs.owner : inputs.dynamicOwner)
                    ->GetDefaultView());
        }
        AddStaticPass(graph, std::move(pass), cells);
    }
}

void DynamicVoxelGIRenderer::BuildFilterGraph(const StaticVoxelGIInputs& inputs,
                                              const HeapVector<ComputeDispatchChunk>& cells,
                                              uint32_t face)
{
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        RDGComputePassDesc temporal;
        temporal.SetShaderProgramName("GITemporalSP");
        temporal.SetPassTag(kind == 0 ? "GIStaticTemporal" : "GIDynamicTemporal");
        BindFrameInputs(temporal);
        temporal.BindValue("uGIFilter", m_filter);
        temporal.BindStorageImage("rawIrradiance",
                                  (kind == 0 ? m_raw[face] : m_dynamicRaw[face])->GetDefaultView());
        // Metadata prevents reads of uninitialized history; every cell is written below.
        temporal.BindStorageImage("history", m_history[kind][face]->GetDefaultView(),
                                  RDGContentGuarantee::eFullWrite);
        temporal.BindStorageBuffer("HistoryMetadata", m_historyMetadata[kind]);
        temporal.BindStorageBuffer("GIStatus", m_status);
        temporal.BindStorageImage(
            "owner",
            (kind == 0 || inputs.dynamicOwner == nullptr ? inputs.owner : inputs.dynamicOwner)
                ->GetDefaultView());
        AddStaticPass(graph, std::move(temporal), cells);

        RDGComputePassDesc spatial;
        spatial.SetShaderProgramName("GISpatialSP");
        spatial.SetPassTag(kind == 0 ? "GIStaticSpatial" : "GIDynamicSpatial");
        BindFrameInputs(spatial);
        spatial.BindValue("uGIFilter", m_filter);
        spatial.BindStorageImage("sourceIrradiance", m_history[kind][face]->GetDefaultView());
        spatial.BindStorageImage(
            "surfaceNormal",
            (kind == 0 || inputs.dynamicNormal == nullptr ? inputs.normal : inputs.dynamicNormal)
                ->GetDefaultView());
        spatial.BindStorageImage(
            "surfaceOwner",
            (kind == 0 || inputs.dynamicOwner == nullptr ? inputs.owner : inputs.dynamicOwner)
                ->GetDefaultView());
        spatial.BindStorageImage("filteredIrradiance", m_dynamicFiltered[face]->GetDefaultView(),
                                 RDGContentGuarantee::eFullWrite);
        AddStaticPass(graph, std::move(spatial), cells);
        if (kind == 0)
        {
            // Reuse the dynamic composition target as static filter scratch. RDG
            // orders its later overwrite after padding has consumed this version.
            RDGComputePassDesc pad;
            pad.SetShaderProgramName("GIStaticPadSP");
            pad.SetPassTag("GIStaticPad");
            BindFrameInputs(pad);
            pad.BindStorageImage("staticOwner", inputs.owner->GetDefaultView());
            pad.BindStorageImage("rawIrradiance", m_dynamicFiltered[face]->GetDefaultView());
            pad.BindStorageImage("paddedIrradiance", m_padded[face]->GetDefaultView(),
                                 RDGContentGuarantee::eFullWrite);
            AddStaticPass(graph, std::move(pad), cells, face);
        }
    }
}

bool DynamicVoxelGIRenderer::BuildReceiverSelection(const StaticVoxelGIInputs& inputs,
                                                    const HeapVector<ComputeDispatchChunk>& cells)
{
    bool valid         = true;
    RenderGraph& graph = *m_device->GetCurrentFrameRDG();
    RDGComputePassDesc reset;
    reset.SetShaderProgramName("GIFrameResetSP");
    reset.SetPassTag("GIReceiverReset");
    BindFrameInputs(reset);
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        reset.BindStorageBuffer(kind == 0 ? "StaticFlags" : "DynamicFlags", m_receiverFlags[kind],
                                RDGContentGuarantee::eFullWrite);
        reset.BindStorageBuffer(kind == 0 ? "StaticReceivers" : "DynamicReceivers",
                                m_receiverLists[kind], RDGContentGuarantee::eFullWrite);
    }
    AddStaticPass(graph, std::move(reset), cells);
    if (m_work.state.z != 0)
    {
        HeapVector<ComputeDispatchChunk> pixels;
        valid = StaticChunks(inputs.viewportExtent.x * inputs.viewportExtent.y,
                             m_device->GetGPUInfo(), pixels);
        RDGComputePassDesc select;
        select.SetShaderProgramName("GIFrameSelectSP");
        select.SetPassTag("GISelectStaticReceivers");
        BindFrameInputs(select);
        select.BindValue("uGIFilter", m_filter);
        select.BindValue("uSurfaceLookup", glm::uvec4(inputs.viewportExtent, 0, 0));
        const NameID names[] = {"positionMap", "normalMap", "geometricNormalMap", "receiverMap",
                                "depthMap"};
        const NameID tags[]  = {"offscreen_position", "offscreen_normal",
                                "offscreen_geometric_normal", "offscreen_receiver",
                                "offscreen_depth"};
        for (uint32_t i = 0; i < 5; ++i)
        {
            if (inputs.surfaceViews[i] != nullptr)
            {
                select.BindSampledTexture(names[i], m_surfaceSampler, inputs.surfaceViews[i]);
            }
            else
            {
                select.BindSampledTexture(names[i], m_surfaceSampler, tags[i]);
            }
        }
        select.BindStorageBuffer("SelectionTaps", m_receiverLists[0]);
        AddStaticPass(graph, std::move(select), pixels);
        RDGComputePassDesc expand;
        expand.SetShaderProgramName("GIFrameExpandSelectionSP");
        expand.SetPassTag("GIExpandStaticReceivers");
        BindFrameInputs(expand);
        expand.BindValue("uGIFilter", m_filter);
        expand.BindStorageImage("staticOwner", inputs.owner->GetDefaultView());
        expand.BindStorageBuffer("SelectionTaps", m_receiverLists[0]);
        expand.BindStorageBuffer("StaticFlags", m_receiverFlags[0],
                                 RDGContentGuarantee::eFullWrite);
        AddStaticPass(graph, std::move(expand), cells);
    }
    RDGComputePassDesc neighborhood;
    neighborhood.SetShaderProgramName("GIFrameNeighborhoodSP");
    neighborhood.SetPassTag("GIDynamicNeighborhood");
    BindFrameInputs(neighborhood);
    neighborhood.BindStorageImage(
        "dynamicOwner",
        (inputs.dynamicOwner != nullptr ? inputs.dynamicOwner : inputs.owner)->GetDefaultView());
    neighborhood.BindStorageBuffer("DynamicFlags", m_receiverFlags[1]);
    AddStaticPass(graph, std::move(neighborhood), cells);

    RDGComputePassDesc compact;
    compact.SetShaderProgramName("GIFrameCompactSP");
    compact.SetPassTag("GICompactReceivers");
    BindFrameInputs(compact);
    compact.BindValue("uGIFilter", m_filter);
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        compact.BindStorageBuffer(kind == 0 ? "StaticFlags" : "DynamicFlags",
                                  m_receiverFlags[kind]);
        compact.BindStorageBuffer(kind == 0 ? "StaticReceivers" : "DynamicReceivers",
                                  m_receiverLists[kind]);
    }
    compact.BindStorageBuffer("GIWorkCounts", m_workCounts);
    compact.BindStorageBuffer("HistoryMetadata", m_historyMetadata[1]);
    compact.BindStorageImage("staticOwner", inputs.owner->GetDefaultView());
    AddStaticPass(graph, std::move(compact), cells);
    RDGComputePassDesc arguments;
    arguments.SetShaderProgramName("GIFrameArgumentsSP");
    arguments.SetPassTag("GIReceiverArguments");
    arguments.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
    arguments.independentDispatches = true;
    BindFrameInputs(arguments);
    arguments.BindStorageBuffer("GIWorkCounts", m_workCounts);
    arguments.BindStorageBuffer("GIArguments", m_indirect);
    graph.AddComputePass(std::move(arguments))
        .RecordPassCommands([chunks = m_workChunks](RDGPassCmdEncoder& encoder) {
            RecordReceiverArguments(encoder, chunks);
        });
    return valid;
}

bool DynamicVoxelGIRenderer::BuildRenderGraph(const StaticVoxelGIInputs& inputs,
                                              const GIVisibilityProvider& provider,
                                              const SceneUniformData& scene,
                                              float indirectGain,
                                              bool shadows)
{
    const uint32_t n             = m_uniform.volume.x;
    const GIVisibilityInfo& info = provider.GetInfo();
    const glm::uvec4 querySettings = info.backend == GIVisibilityBackend::eVoxelDDA ?
        glm::uvec4(info.grid.dimensions.y & GI_STATIC, info.grid.dimensions.z, info.grid.averaged.x,
                   0) :
        glm::uvec4(0);
    const uint64_t pixels        = uint64_t(inputs.viewportExtent.x) * inputs.viewportExtent.y;
    const double time            = inputs.timeSeconds == -1 ?
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_clockStart).count() :
        inputs.timeSeconds;
    const bool environment =
        m_lighting.enabled.y != 0 && scene.environment.x * scene.environment.z != 0;
    const uint64_t environmentResource =
        inputs.environment != nullptr ? inputs.environment->GetStableId() : 0;
    const bool environmentChanged = !m_environmentValid || m_backend != info.backend ||
        m_listGeneration != inputs.listGeneration ||
        m_uniform.minimumCellSize != inputs.grid.minimumCellSize ||
        m_environmentGeneration != info.generation ||
        m_environmentRevision != inputs.environmentRevision ||
        m_environmentResource != environmentResource ||
        m_environmentSettings != Vec3(scene.environment);
    bool valid = m_status != nullptr && info.ready && inputs.listGeneration != 0 &&
        (m_work.cache.x == 0 || info.backend == GIVisibilityBackend::eVoxelDDA) &&
        inputs.grid.dimensions.x == n && inputs.grid.dimensions.y == GI_ALL &&
        inputs.owner != nullptr && inputs.normal != nullptr &&
        (inputs.dynamicOwner == nullptr || inputs.dynamicNormal != nullptr) &&
        inputs.occupiedList != nullptr && inputs.gridToList != nullptr &&
        inputs.occupiedCount != nullptr && inputs.dynamicCount != nullptr &&
        (inputs.dynamicOwner == nullptr) == (inputs.dynamicMap == nullptr) &&
        pixels <= UINT32_MAX && scene.lightInfo.x >= 0 && scene.lightInfo.x <= MaxSceneLights &&
        scene.lightInfo.x == std::floor(scene.lightInfo.x) &&
        (!environment ||
         (inputs.environment != nullptr && inputs.environmentSampler != nullptr &&
          inputs.normal != nullptr &&
          (inputs.dynamicOwner == nullptr || inputs.dynamicNormal != nullptr))) &&
        std::isfinite(indirectGain) && indirectGain >= 0 && std::isfinite(time) && time >= 0 &&
        time <= FLT_MAX;
    HeapVector<ComputeDispatchChunk> cells, initialize, prepare;
    valid = valid && StaticChunks(n * n * n, m_device->GetGPUInfo(), cells) &&
        StaticChunks(m_uniform.volume.y * m_uniform.sampling.x, m_device->GetGPUInfo(),
                     initialize) &&
        StaticChunks(m_work.limits.w, m_device->GetGPUInfo(), prepare);
    if (valid)
    {
        // DDA static visibility depends on the static compact-list generation, not
        // on moving geometry in the combined provider. Reference tables own their generation.
        const uint64_t generation = info.backend == GIVisibilityBackend::eVoxelDDA ?
            inputs.listGeneration :
            info.generation;
        const bool changed        = m_generation != generation ||
            m_listGeneration != inputs.listGeneration || m_backend != info.backend ||
            m_uniform.minimumCellSize != inputs.grid.minimumCellSize ||
            m_staticQuerySettings != querySettings;
        const bool resetHistory = changed || !m_historyValid || time < m_lastTime ||
            time - m_lastTime > 0.3 || inputs.historyRevision != m_historyRevision ||
            m_uniform.lighting.x != indirectGain ||
            m_uniform.lighting.y != (shadows ? 1.0f : 0.0f) ||
            m_uniform.volume.w != static_cast<uint32_t>(scene.lightInfo.x) ||
            m_environmentRevision != inputs.environmentRevision ||
            m_environmentResource != environmentResource ||
            m_environmentSettings != Vec3(scene.environment);
        m_filter.timing           = Vec4(static_cast<float>(time), 0.3f, 0.03f, 60.0f);
        m_filter.control.z        = resetHistory ? 1 : 0;
        m_recordedTime            = time;
        m_recordedHistoryRevision = inputs.historyRevision;
        m_recordedFrame           = true;
        PrepareLightMasks(inputs, info, scene, shadows);
        const uint32_t start = changed ? 0 : m_cacheEnd;
        const uint32_t end   = std::min(m_uniform.volume.y, start + GI_CACHE_BATCH);
        HeapVector<ComputeDispatchChunk> rays;
        uint32_t first = start * m_uniform.sampling.x;
        while (valid && first < end * m_uniform.sampling.x)
        {
            ComputeDispatchChunk chunk;
            valid = BuildComputeDispatchChunk(first, end * m_uniform.sampling.x - first,
                                              GI_QUERY_GROUP_SIZE, m_device->GetGPUInfo(), chunk);
            if (valid)
            {
                rays.push_back(chunk);
                first += chunk.itemCount;
            }
        }
        m_recordedGeneration      = generation;
        m_staticQuerySettings           = querySettings;
        m_recordedListGeneration  = inputs.listGeneration;
        m_recordedBackend         = info.backend;
        m_recordedCacheEnd        = end;
        m_recordedCacheBatch      = end > start;
        m_uniform.minimumCellSize = inputs.grid.minimumCellSize;
        m_uniform.volume.w              = static_cast<uint32_t>(scene.lightInfo.x);
        m_recordedEnvironmentGeneration = info.generation;
        m_environmentRevision           = inputs.environmentRevision;
        m_environmentResource           = environmentResource;
        m_environmentSettings           = Vec3(scene.environment);
        m_uniform.lighting        = Vec4(indirectGain, shadows ? 1.0f : 0.0f,
                                         2.0f * std::sqrt(3.0f) * n * inputs.grid.minimumCellSize.w,
                                         inputs.grid.minimumCellSize.w * 1e-4f);
        m_work.state =
            glm::uvec4(end, start, pixels != 0 ? 1 : 0, inputs.dynamicOwner != nullptr ? 1 : 0);
        RenderGraph& graph = *m_device->GetCurrentFrameRDG();
        if (resetHistory)
        {
            BuildHistoryMetadata(inputs, cells, true);
        }
        RDGComputePassDesc setup;
        setup.SetShaderProgramName("GIStaticPrepareSP");
        setup.SetPassTag("GIFramePrepare");
        BindFrameInputs(setup);
        setup.BindStorageBuffer("StaticCount", inputs.occupiedCount);
        setup.BindStorageBuffer("DynamicCount", inputs.dynamicCount);
        setup.BindStorageBuffer("GIStatus", m_status, RDGContentGuarantee::eFullWrite);
        setup.BindStorageBuffer("GIWorkCounts", m_workCounts, RDGContentGuarantee::eFullWrite);
        setup.BindStorageBuffer("GIArguments", m_indirect, RDGContentGuarantee::eFullWrite);
        setup.BindValue("uGILighting", m_lighting);
        setup.BindStorageBuffer("GILightMaskStatus", m_lightMaskStatus,
                                m_lighting.enabled.w == UINT32_MAX ?
                                    RDGContentGuarantee::eFullWrite :
                                    RDGContentGuarantee::eNone);
        AddStaticPass(graph, std::move(setup), prepare);
        for (uint32_t face = 0; face < GI_FACE_COUNT && valid; ++face)
        {
            for (uint32_t phase = 0; phase < 2; ++phase)
            {
                if ((phase == 0 && changed) || (phase == 1 && !rays.empty()))
                {
                    RDGComputePassDesc cache;
                    cache.SetShaderProgramName(info.backend == GIVisibilityBackend::eVoxelDDA ?
                                                   "GIStaticCacheDDASP" :
                                                   "GIStaticCacheReferenceSP");
                    cache.SetPassTag(phase == 0 ? "GIStaticCacheClear" : "GIStaticCacheBatch");
                    valid = provider.BindQueryInputs(cache) && valid;
                    BindFrameInputs(cache);
                    cache.BindStorageBuffer("StaticCount", inputs.occupiedCount);
                    cache.BindStorageBuffer("StaticList", inputs.occupiedList);
                    cache.BindStorageBuffer("StaticHits", m_hits[face],
                                            phase == 0 ? RDGContentGuarantee::eFullWrite :
                                                         RDGContentGuarantee::eNone);
                    AddStaticPass(graph, std::move(cache), phase == 0 ? initialize : rays, face,
                                  phase == 0 ? 1 : 0);
                }
            }
        }
        valid = BuildReceiverSelection(inputs, cells) && valid;
        for (uint32_t kind = 0; kind < 2 && m_lighting.enabled.w != 0; ++kind)
        {
            RDGComputePassDesc light;
            const bool meshVisibility = info.backend == GIVisibilityBackend::eVoxelDDA &&
                inputs.scene != nullptr && inputs.shadowMaps != nullptr;
            light.SetShaderProgramName(meshVisibility ? "GIStaticLightMeshSP" :
                                           info.backend == GIVisibilityBackend::eVoxelDDA ?
                                                        "GIStaticLightDDASP" :
                                                        "GIStaticLightReferenceSP");
            light.SetPassTag(kind == 0 ? "GIStaticLight" : "GIDynamicLight");
            valid = provider.BindQueryInputs(light) && valid;
            if (meshVisibility)
            {
                inputs.shadowMaps->BindLightingInputs(light);
                light.BindStorageBuffer("VertexBuffer", inputs.scene->GetVertexBuffer());
                light.BindStorageBuffer("IndexBuffer", inputs.scene->GetIndexBuffer());
                light.BindStorageBuffer("NodeBuffer", inputs.scene->GetNodesDataSSBO());
                light.BindStorageBuffer("TriangleRecords", inputs.scene->GetVoxelTriangleBuffer());
            }
            BindFrameInputs(light);
            light.BindValue("uSceneData", scene);
            light.BindValue("uGILighting", m_lighting);
            light.BindStorageBuffer("StaticMap",
                                    kind == 0 || inputs.dynamicMap == nullptr ? inputs.gridToList :
                                                                                inputs.dynamicMap);
            light.BindStorageBuffer("StaticLightMask", kind == 0 ? m_lightMask : m_dynamicLightMask,
                                    m_lighting.enabled.w == UINT32_MAX ?
                                        RDGContentGuarantee::eFullWrite :
                                        RDGContentGuarantee::eNone);
            light.BindStorageBuffer("SenderPositions", m_senderPositions[kind],
                                    RDGContentGuarantee::eFullWrite);
            light.BindStorageBuffer("GIStatus", m_status);
            light.BindStorageBuffer("GILightMaskStatus", m_lightMaskStatus);
            AddStaticPass(graph, std::move(light), cells, 0, kind == 0 ? GI_STATIC : GI_DYNAMIC);
        }
        if (environment && environmentChanged)
        {
            valid = BuildEnvironmentGraph(inputs, provider, scene, cells) && valid;
        }
        for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
        {
            RDGComputePassDesc clear;
            clear.SetShaderProgramName("GIFrameClearSP");
            clear.SetPassTag("GIClearIrradiance");
            BindFrameInputs(clear);
            clear.BindStorageImage("rawIrradiance", m_raw[face]->GetDefaultView(),
                                   RDGContentGuarantee::eFullWrite);
            clear.BindStorageImage("dynamicIrradiance", m_dynamicRaw[face]->GetDefaultView(),
                                   RDGContentGuarantee::eFullWrite);
            AddStaticPass(graph, std::move(clear), cells, face);
            for (uint32_t kind = 0; kind < 2; ++kind)
            {
                RDGComputePassDesc gather;
                gather.SetShaderProgramName(
                    info.backend == GIVisibilityBackend::eVoxelDDA ?
                        (environment ? "GIFrameGatherEnvironmentDDASP" : "GIStaticGatherSP") :
                        (environment ? "GIFrameGatherEnvironmentReferenceSP" :
                                       "GIFrameGatherReferenceSP"));
                gather.SetPassTag(kind == 0 ? "GIStaticGather" : "GIDynamicGather");
                gather.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
                gather.independentDispatches = true;
                valid                        = provider.BindQueryInputs(gather) && valid;
                BindFrameInputs(gather);
                gather.BindValue("uSceneData", scene);
                gather.BindValue("uGILighting", m_lighting);
                if (environment)
                {
                    gather.BindSampledTexture("environmentMap", inputs.environmentSampler,
                                              inputs.environment);
                    gather.BindStorageImage("staticEnvironment",
                                            m_senderEnvironment[0]->GetDefaultView());
                    gather.BindStorageImage("dynamicEnvironment",
                                            m_senderEnvironment[1]->GetDefaultView());
                }
                gather.BindStorageBuffer("GIReceivers", m_receiverLists[kind]);
                gather.BindStorageBuffer("GIWorkCounts", m_workCounts);
                gather.BindStorageBuffer("StaticMap", inputs.gridToList);
                gather.BindStorageBuffer("StaticLightMask", m_lightMask);
                gather.BindStorageBuffer("DynamicLightMask", m_dynamicLightMask);
                gather.BindStorageBuffer("StaticSenderPositions", m_senderPositions[0]);
                gather.BindStorageBuffer("DynamicSenderPositions", m_senderPositions[1]);
                gather.BindStorageBuffer("StaticHits", m_hits[face]);
                gather.BindStorageBuffer("GIStatus", m_status);
                gather.BindStorageImage(
                    "dynamicOwner",
                    (inputs.dynamicOwner != nullptr ? inputs.dynamicOwner : inputs.owner)
                        ->GetDefaultView());
                gather.BindStorageImage(
                    "rawIrradiance",
                    (kind == 0 ? m_raw[face] : m_dynamicRaw[face])->GetDefaultView());
                gather.UseIndirectBuffer(m_indirect);
                graph.AddComputePass(std::move(gather))
                    .RecordPassCommands([chunks = m_workChunks, arguments = m_indirect, face,
                                         kind](RDGPassCmdEncoder& encoder) {
                        RecordReceiverGather(encoder, chunks, arguments, face, kind);
                    });
            }
            BuildFilterGraph(inputs, cells, face);
        }
        BuildHistoryMetadata(inputs, cells, false);
    }
    return valid;
}

void DynamicVoxelGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uStaticGI", m_uniform);
    pass.BindStorageBuffer("GIStatus", m_status);
    for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
    {
        pass.BindSampledTexture(NameID(fmt::format("staticIrradiance{}", face)), m_sampler,
                                m_padded[face]->GetDefaultView());
        pass.BindSampledTexture(NameID(fmt::format("dynamicIrradiance{}", face)), m_sampler,
                                m_dynamicFiltered[face]->GetDefaultView());
    }
}

void DynamicVoxelGIRenderer::OnRenderGraphExecuted(bool succeeded)
{
    if (m_recordedFrame)
    {
        m_lightMasksValid       = succeeded;
        m_historyValid    = succeeded;
        m_environmentValid      = succeeded;
        m_environmentGeneration = succeeded ? m_recordedEnvironmentGeneration : 0;
        m_historyRevision = m_recordedHistoryRevision;
        m_lastTime        = m_recordedTime;
        m_recordedFrame   = false;
    }
    if (!succeeded)
    {
        m_lightMasksValid  = false;
        m_historyValid = false;
        m_environmentValid = false;
    }
    m_generation     = succeeded ? m_recordedGeneration : 0;
    m_listGeneration = succeeded ? m_recordedListGeneration : 0;
    m_backend        = m_recordedBackend;
    m_cacheEnd       = succeeded ? m_recordedCacheEnd : 0;
    if (succeeded && m_recordedCacheBatch)
    {
        ++m_cacheBuildBatches;
    }
    m_recordedCacheBatch = false;
}

void DynamicVoxelGIRenderer::Destroy()
{
    m_resourceBytes = 0;
    for (uint32_t face = 0; face < GI_FACE_COUNT; ++face)
    {
        m_device->DestroyBuffer(m_hits[face]);
        m_device->DestroyTexture(m_raw[face]);
        m_device->DestroyTexture(m_padded[face]);
        m_device->DestroyTexture(m_dynamicRaw[face]);
        m_device->DestroyTexture(m_dynamicFiltered[face]);
        m_dynamicFiltered[face] = nullptr;
        for (uint32_t kind = 0; kind < 2; ++kind)
        {
            m_device->DestroyTexture(m_history[kind][face]);
            m_history[kind][face] = nullptr;
        }
        m_dynamicRaw[face] = nullptr;
        m_hits[face] = nullptr;
        m_raw[face] = m_padded[face] = nullptr;
    }
    for (uint32_t kind = 0; kind < 2; ++kind)
    {
        m_device->DestroyBuffer(m_receiverFlags[kind]);
        m_device->DestroyBuffer(m_receiverLists[kind]);
        m_device->DestroyBuffer(m_historyMetadata[kind]);
        m_device->DestroyBuffer(m_senderPositions[kind]);
        m_senderPositions[kind] = nullptr;
        m_device->DestroyTexture(m_senderEnvironment[kind]);
        m_senderEnvironment[kind] = nullptr;
        m_historyMetadata[kind] = nullptr;
        m_receiverFlags[kind] = m_receiverLists[kind] = nullptr;
    }
    m_device->DestroyBuffer(m_dynamicLightMask);
    m_device->DestroyBuffer(m_workCounts);
    m_device->DestroyBuffer(m_indirect);
    m_dynamicLightMask = m_workCounts = m_indirect = nullptr;
    m_workChunks.clear();
    m_cacheEnd = m_recordedCacheEnd = 0;
    m_recordedCacheBatch            = false;
    m_historyValid = m_recordedFrame = m_environmentValid = false;
    m_device->DestroyBuffer(m_status);
    m_device->DestroyBuffer(m_lightMask);
    m_device->DestroyBuffer(m_lightMaskStatus);
    m_lightMaskStatus = nullptr;
    m_lightMasksValid = false;
    m_status = m_lightMask = nullptr;
    m_generation = m_recordedGeneration = 0;
}
} // namespace zen::rc
