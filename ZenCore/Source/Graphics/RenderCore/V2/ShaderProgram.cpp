#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/RenderCore/V2/Renderer/SceneCulling.h"
#include "Graphics/RenderCore/V2/Renderer/ForwardDepthSort.h"
#include "Graphics/RenderCore/V2/Renderer/ForwardGPUDraws.h"
#include "ZenOpacityMicromapShaders.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"
#include <algorithm>

namespace zen::rc
{
NameID MaterialUVShaderName(NameID name, uint32_t uvSetCount)
{
    std::string specialized(name.CStr());
    if (uvSetCount == 2u)
    {
        specialized.insert(specialized.size() - 2u, "UV2");
    }
    return NameID(specialized);
}

namespace
{
bool HasOpacityMicromapVariant(const std::string& path)
{
    bool found = false;
    for (const char* candidate : kOpacityMicromapShaderFiles)
    {
        if (path == candidate)
        {
            found = true;
            break;
        }
    }
    return found;
}

// The generated list is the build's source of truth, including buffered/no-guide variants.
bool HasHitStreamVariant(const std::string& path)
{
    bool found = false;
    for (const char* candidate : kHitStreamShaderFiles)
    {
        found = found || path == candidate;
    }
    return found;
}

void StoreBufferedTracePrograms(ShaderProgramManager& manager, RenderDevice* device, const char* name, const char* path)
{
    const RenderConfig& config         = RenderConfig::GetInstance();
    const bool          overrideModule = NameID(name)
                                          == NameID(config.hybridTraceReferenceModule ? "HybridTraceHardwareBufferedSP"
                                                                                      : "HybridTraceReSTIRHardwareBufferedSP")
                                      && config.hybridTraceBufferedModule && !config.hybridTraceModule.empty();
    for (bool noGuide : {false, true})
    {
        std::string program(name);
        std::string source(path);
        if (noGuide)
        {
            program.insert(program.size() - 2u, "NoGuide");
            source.insert(source.size() - 4u, ".noguide");
        }
        if (overrideModule)
        {
            source = config.hybridTraceModule;
        }
        manager.StoreProgram(ZEN_NEW() ComputeFileSP(device, NameID(program), source.c_str()));
    }
}

void StoreMaterialUVPrograms(ShaderProgramManager&                                  manager,
                             RenderDevice*                                          device,
                             NameID                                                 name,
                             const char*                                            vertex,
                             const char*                                            fragment,
                             const HashMap<uint32_t, RHIShaderSpecializationValue>& constants = {})
{
    manager.StoreProgram(ZEN_NEW() HybridGraphicsSP(device, name, vertex, fragment, constants));
    HashMap<uint32_t, RHIShaderSpecializationValue> compactConstants(constants);
    compactConstants.emplace(7u, uint32_t(2));
    manager.StoreProgram(ZEN_NEW()
                             HybridGraphicsSP(device, MaterialUVShaderName(name, 2u), vertex, fragment, compactConstants));
}

void StoreForwardMaterialPrograms(ShaderProgramManager& manager, RenderDevice* device, const char* name, const char* fragment)
{
    StoreMaterialUVPrograms(manager, device, name, "SceneRenderer/offscreen.vert.spv", fragment);
    std::string basicName(name);
    basicName.insert(basicName.size() - 2u, "Basic");
    const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{0u, true}};
    StoreMaterialUVPrograms(manager, device, NameID(basicName), "SceneRenderer/offscreen.vert.spv", fragment, constants);
    if (SupportsForwardGPUDraws(device->GetGPUInfo(), 1))
    {
        basicName.insert(basicName.size() - 2u, "GPU");
        std::string gpuFragment(fragment);
        gpuFragment.insert(gpuFragment.size() - 4u, ".gpu");
        StoreMaterialUVPrograms(manager, device, NameID(basicName), "SceneRenderer/offscreen_forward_gpu.vert.spv",
                                gpuFragment.c_str(), constants);
    }
}

// Both names use one SPIR-V module. The capture specialization reserves the
// larger exact accumulator only when that pipeline is selected.
void StoreGuideTracePrograms(ShaderProgramManager& manager, RenderDevice* device, const char* name, const char* path)
{
    const RenderConfig& config = RenderConfig::GetInstance();
    const std::string&  module = config.hybridTraceModule;
    if (NameID(name) == NameID("HybridTraceReSTIRHardwareSP") && !module.empty() && !config.hybridTraceBufferedModule)
    {
        // The caller supplies the exact module, including the OMM variant when required.
        path = module.c_str();
        LOGI("HybridTrace measurement module: {}", path);
    }
    if (!config.hybridTraceOverrideProgram.empty() && NameID(name) == NameID(config.hybridTraceOverrideProgram.c_str()))
    {
        path = config.hybridTraceOverrideModule.c_str();
        LOGI("HybridTrace override module for {}: {}", name, path);
    }
    manager.StoreProgram(ZEN_NEW() ComputeFileSP(device, name, path));
    std::string exactName(name);
    exactName.insert(exactName.size() - 2u, "Exact");
    const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{0u, true}};
    manager.StoreProgram(ZEN_NEW() ComputeFileSP(device, NameID(exactName), path, constants));
}

class LightBallSP final : public ShaderProgram
{
public:
    explicit LightBallSP(RenderDevice* device) : ShaderProgram(device, "LightBallSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/light_ball.vert.spv");

        AddShaderStage(RHIShaderStage::eFragment, "SceneRenderer/light_ball.frag.spv");

        Init();
    }
};

class DebugViewSP final : public ShaderProgram
{
public:
    DebugViewSP(RenderDevice* device, NameID name, const char* fragment) : ShaderProgram(device, name)
    {
        AddShaderStage(RHIShaderStage::eVertex, "SceneRenderer/deferred.vert.spv");

        AddShaderStage(RHIShaderStage::eFragment, fragment);

        Init();
    }
};

} // namespace

ShaderProgram::ShaderProgram(RenderDevice* pRenderDevice, NameID name) : m_pRenderDevice(pRenderDevice), m_name(name) {}

ShaderProgram::~ShaderProgram()
{
    if (m_pShader != nullptr)
    {
        if (m_pRenderDevice != nullptr)
        {
            m_pRenderDevice->DeferReleaseResource(m_pShader);
        }
        else
        {
            m_pShader->ReleaseReference();
        }
    }
}

void ShaderProgram::UpdateUniformBuffer(NameID name, const uint8_t* pData, uint32_t offset)
{
    if (m_uniformBufferMap.contains(name))
    {
        m_pRenderDevice->UpdateBuffer(m_uniformBufferMap[name], m_namedSRDLut[name]->blockSize, pData, offset);
    }
}

bool ShaderProgram::Init()
{
    return Init({});
}

bool ShaderProgram::Init(const HashMap<uint32_t, RHIShaderSpecializationValue>& specializationConstants)
{
    bool result{};

    if (GDynamicRHI == nullptr)
    {
        LOGE("Shader '{}' initialization failed: missing RHI", m_name.CStr());

        result = false;
    }
    else
    {
        RHIShaderCreateInfo info{};

        std::ranges::copy(m_stageSources, info.spirvFileName);
        if (RenderConfig::GetInstance().hybridHitStream)
        {
            for (std::string& source : info.spirvFileName)
            {
                if (source != RenderConfig::GetInstance().hybridTraceModule && HasHitStreamVariant(source))
                {
                    source.insert(source.size() - 4u, ".stream");
                }
            }
        }
        if (GDynamicRHI->QueryGPUInfo().rayQuery.opacityMicromap)
        {
            for (std::string& source : info.spirvFileName)
            {
                if (HasOpacityMicromapVariant(source))
                {
                    source.insert(source.size() - 4u, ".omm");
                }
            }
        }

        info.stageFlags = m_stageFlags;

        if (RHIOptions::GetInstance().PipelineStatistics())
        {
            for (const std::string& source : info.spirvFileName)
            {
                if (!source.empty())
                {
                    LOGI("Shader module: shader='{}' source='{}'", m_name.CStr(), source);
                }
            }
        }

        info.name                    = m_name;

        info.specializationConstants = specializationConstants;

        RHIShader* shader            = GDynamicRHI->CreateShader(info);

        if (shader == nullptr)
        {
            LOGE("Shader '{}' initialization failed", m_name.CStr());

            result = false;
        }
        else
        {
            RHIShader* previous = m_pShader;

            m_pShader           = shader;

            ResolveShaderResources();

            if (previous != nullptr)
            {
                if (m_pRenderDevice != nullptr)
                {
                    m_pRenderDevice->DeferReleaseResource(previous);
                }
                else
                {
                    previous->ReleaseReference();
                }
            }

            result = true;
        }
    }

    return result;
}

void ShaderProgram::ResolveShaderResources()
{
    m_namedSRDLut.clear();

    m_storageBuffers.clear();

    m_sampledTextures.clear();

    m_storageImages.clear();

    if (m_pShader != nullptr)
    {
        m_storageBuffers.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eStorageBuffer));

        m_sampledTextures.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eSamplerWithTexture));

        m_storageImages.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eImage));

        const RHIShaderResourceDescriptorTable* SRDTable = m_pShader->GetSRDTable();

        for (SmallVector<RHIShaderResourceDescriptor> const& setSRD : *SRDTable)
        {
            for (RHIShaderResourceDescriptor const& srd : setSRD)
            {
                m_namedSRDLut[srd.name] = &srd;

                if (srd.type == RHIShaderResourceType::eStorageBuffer)
                {
                    m_storageBuffers.emplace_back(srd);
                }
                else if (srd.type == RHIShaderResourceType::eImage)
                {
                    m_storageImages.emplace_back(srd);
                }
                else if (srd.type == RHIShaderResourceType::eTexture || srd.type == RHIShaderResourceType::eSamplerWithTexture)
                {
                    m_sampledTextures.emplace_back(srd);
                }
            }
        }
    }
}

ShaderProgram* ShaderProgramManager::CreateShaderProgram(RenderDevice* pRenderDevice, NameID name)
{
    ShaderProgram* pShaderProgram = ZEN_NEW() ShaderProgram(pRenderDevice, name);

    StoreProgram(pShaderProgram);

    return pShaderProgram;
}

void ShaderProgramManager::StoreProgram(ShaderProgram* program)
{
    const std::pair<HashMap<NameID, ShaderProgram*>::iterator, bool> itResult =
        m_programCache.try_emplace(program->GetName(), program);

    HashMap<NameID, ShaderProgram*>::iterator it = itResult.first;

    bool inserted                                = itResult.second;

    if (!inserted && it->second != program)
    {
        ShaderProgram* previous = it->second;

        it->second              = program;

        ZEN_DELETE(previous);
    }
}

void ShaderProgramManager::StoreWarmedComputeProgram(ShaderProgram* program)
{
    const NameID name = program->GetName();

    StoreProgram(program);

    if (std::find(m_initializationWarmups.begin(), m_initializationWarmups.end(), name) == m_initializationWarmups.end())
    {
        m_initializationWarmups.push_back(name);
    }
}

void ShaderProgramManager::Destroy()
{
    for (std::pair<const NameID, ShaderProgram*>& kv : m_programCache)
    {
        ZEN_DELETE(kv.second);
    }

    m_programCache.clear();

    m_initializationWarmups.clear();
}

void ShaderProgramManager::BuildShaderPrograms(RenderDevice* pRenderDevice)
{
    const char* debugNames[] = {"RenderDebug2DSP", "RenderDebugArraySP", "RenderDebugVolumeSP", "RenderDebugNormalSP"};

    const char* debugFiles[] = {"SceneRenderer/debug_2d.frag.spv", "SceneRenderer/debug_array.frag.spv",
                                "SceneRenderer/debug_volume.frag.spv", "SceneRenderer/debug_normal.frag.spv"};

    for (uint32_t index = 0; index < 4; ++index)
    {
        StoreProgram(ZEN_NEW() DebugViewSP(pRenderDevice, debugNames[index], debugFiles[index]));
    }
    StoreProgram(ZEN_NEW() DebugViewSP(pRenderDevice, "HybridDebugSP", "SceneRenderer/debug_hybrid.frag.spv"));
    StoreProgram(ZEN_NEW() DebugViewSP(pRenderDevice, "ProbeDebugSP", "SceneRenderer/debug_probe.frag.spv"));

    const glm::uvec3 volumeGroupSize = ResolveVoxelVolumeWorkgroupSize(pRenderDevice->GetGPUInfo());

    const HashMap<uint32_t, RHIShaderSpecializationValue> volumeConstants{
        {ZEN_VOXEL_VOLUME_GROUP_X_ID, static_cast<int>(volumeGroupSize.x)},
        {ZEN_VOXEL_VOLUME_GROUP_Y_ID, static_cast<int>(volumeGroupSize.y)},
        {ZEN_VOXEL_VOLUME_GROUP_Z_ID, static_cast<int>(volumeGroupSize.z)}};

    LOGI("Voxel volume workgroup: {}x{}x{} (device invocation limit {})", volumeGroupSize.x, volumeGroupSize.y,
         volumeGroupSize.z, pRenderDevice->GetGPUInfo().maxComputeWorkGroupInvocations);

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureFrameSP", "SceneRenderer/capture_frame.comp.spv"));
    if (SupportsForwardDepthSort(pRenderDevice->GetGPUInfo(), 1))
    {
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, "ForwardHiZCopySP", "SceneRenderer/transparency_hiz_copy.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardHiZSP", "SceneRenderer/transparency_hiz.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardCullSP", "SceneRenderer/transparency_cull.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardPrefixSP", "SceneRenderer/transparency_prefix.comp.spv"));
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, "ForwardDepthKeysSP", "SceneRenderer/transparency_depth_keys.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardEmitSP", "SceneRenderer/transparency_emit.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardSortHistogramSP",
                                             "SceneRenderer/transparency_radix_histogram.comp.spv"));
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, "ForwardSortScanSP", "SceneRenderer/transparency_scan_groups.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ForwardSortScatterSP",
                                             "SceneRenderer/transparency_radix_scatter.comp.spv"));
    }

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelBufferSP", "VoxelGI/capture_buffer.comp.spv"));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelVisibilityCheckSP", "VoxelGI/Calibration/visibility_check.comp.spv"));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "DistanceFieldCheckSP", "VoxelGI/Calibration/distance_field_check.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "DistanceFieldCompositeCheckSP",
                                         "VoxelGI/Calibration/distance_field_composite_check.comp.spv"));

    const char* fieldPrograms[][2] = {
        {"DistanceFieldSnapshotSP", "distance_field_snapshot"},
        {"DistanceFieldSnapshotVerticesSP", "distance_field_snapshot_vertices"},
        {"DistanceFieldClearSP", "distance_field_clear"},
        {"DistanceFieldRetainSP", "distance_field_retain"},
        {"DistanceFieldRecycleSP", "distance_field_recycle"},
        {"DistanceFieldBatchClearSP", "distance_field_batch_clear"},
        {"DistanceFieldJobsSP", "distance_field_jobs"},
        {"DistanceFieldCoverageSP", "distance_field_coverage"},
        {"DistanceFieldPrepareJobsSP", "distance_field_prepare_jobs"},
        {"DistanceFieldPrepareSP", "distance_field_prepare"},
        {"DistanceFieldAdvanceSP", "distance_field_advance"},
        {"DistanceFieldCountSP", "distance_field_count"},
        {"DistanceFieldScanSP", "distance_field_scan"},
        {"DistanceFieldScanSumsSP", "distance_field_scan_sums"},
        {"DistanceFieldAllocateSP", "distance_field_allocate"},
        {"DistanceFieldCompactSP", "distance_field_compact"},
        {"DistanceFieldEmitSP", "distance_field_emit"},
        {"DistanceFieldHierarchySP", "distance_field_hierarchy"},
    };
    for (const char* const(&program)[2] : fieldPrograms)
    {
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program[0], std::string("VoxelGI/") + program[1] + ".comp.spv"));
    }

    StoreProgram(ZEN_NEW() DeferredVoxelGISP(pRenderDevice));
    StoreMaterialUVPrograms(*this, pRenderDevice, "HybridReceiverSP", "SceneRenderer/receiver.vert.spv",
                            "SceneRenderer/receiver.frag.spv");
    StoreMaterialUVPrograms(*this, pRenderDevice, "HybridReceiverMaskedSP", "SceneRenderer/receiver.vert.spv",
                            "SceneRenderer/receiver_masked.frag.spv");
    if (SupportsHybridGBufferCulling(pRenderDevice->GetGPUInfo()))
    {
        StoreMaterialUVPrograms(*this, pRenderDevice, "HybridReceiverIndirectSP", "SceneRenderer/receiver_indirect.vert.spv",
                                "SceneRenderer/receiver_indirect.frag.spv");
        StoreMaterialUVPrograms(*this, pRenderDevice, "HybridReceiverMaskedIndirectSP",
                                "SceneRenderer/receiver_indirect.vert.spv", "SceneRenderer/receiver_masked_indirect.frag.spv");
    }
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHybridGISP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/hybrid_gi.frag.spv"));
    // Also exports each receiver's exact shadow-map results for resting decals.
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHybridGIVisibilitySP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/hybrid_gi_visibility.frag.spv"));
    StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHybridGISP", "SceneRenderer/forward_material_hybrid.frag.spv");
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardBackgroundDepthSP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/forward_background_depth.frag.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceLegacySP", "VoxelGI/sky_irradiance_legacy.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureHybridEnvironmentSP", "VoxelGI/capture_environment.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridDownsampleSP", "VoxelGI/hybrid_downsample.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridReconstructSP", "VoxelGI/hybrid_reconstruct.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridCaptureSP", "VoxelGI/hybrid_capture.comp.spv"));
    // Compute GI traces the distance field (DistanceFieldScene); trace variants without Field are HW RT only.
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceFieldSP", "VoxelGI/hybrid_trace_field.comp.spv");
    StoreBufferedTracePrograms(*this, pRenderDevice, "HybridTraceBufferedFieldSP",
                               "VoxelGI/hybrid_trace_buffered_field.comp.spv");
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridGuidePrepareSP", "VoxelGI/hybrid_guide_prepare.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridGuideFinishSP", "VoxelGI/hybrid_guide_finish.comp.spv"));
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceCountsFieldSP", "VoxelGI/hybrid_trace_counts_field.comp.spv");
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHalfFieldSP", "VoxelGI/hybrid_trace_half_field.comp.spv");
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHalfCountsFieldSP",
                            "VoxelGI/hybrid_trace_half_counts_field.comp.spv");
    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceFallbackFieldSP", "VoxelGI/hybrid_trace_fallback_field.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceFallbackCountsFieldSP",
                                         "VoxelGI/hybrid_trace_fallback_counts_field.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceFieldSP", "VoxelGI/sky_irradiance_field.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceFieldCountsSP",
                                         "VoxelGI/sky_irradiance_field_counts.comp.spv", volumeConstants));
    const char* const probePrograms[][2] = {{"ProbeClearSP", "VoxelGI/probe_clear.comp.spv"},
                                            {"ProbeClaimSP", "VoxelGI/probe_claim.comp.spv"},
                                            {"ProbePlaceSP", "VoxelGI/probe_place.comp.spv"},
                                            {"ProbeAdaptiveSP", "VoxelGI/probe_adaptive.comp.spv"},
                                            {"ProbeAllocateCountSP", "VoxelGI/probe_allocate_count.comp.spv"},
                                            {"ProbeAllocateSP", "VoxelGI/probe_allocate.comp.spv"},
                                            {"ProbeTraceFieldSP", "VoxelGI/probe_trace_field.comp.spv"},
                                            {"ProbeSkySourcesFieldSP", "VoxelGI/probe_sky_sources_field.comp.spv"},
                                            {"ProbeTemporalSP", "VoxelGI/probe_temporal.comp.spv"},
                                            {"ProbeFilterSP", "VoxelGI/probe_filter.comp.spv"},
                                            {"ProbeIntegrateSP", "VoxelGI/probe_integrate.comp.spv"},
                                            {"ProbeSpecularTraceFieldSP", "VoxelGI/probe_specular_trace_field.comp.spv"},
                                            {"ProbeSpecularResolveSP", "VoxelGI/probe_specular_resolve.comp.spv"},
                                            {"ProbeReconstructSP", "VoxelGI/probe_reconstruct.comp.spv"},
                                            {"ProbeCaptureSP", "VoxelGI/probe_capture.comp.spv"}};
    for (const char* const* program : probePrograms)
    {
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program[0], program[1]));
    }
    // Specular sample filter passes at block steps 1, 2, 4 and 8 (ScreenProbeGather.cpp).
    for (uint32_t iteration = 0; iteration < 4; ++iteration)
    {
        const HashMap<uint32_t, RHIShaderSpecializationValue> filterConstants{{0u, static_cast<int>(1u << iteration)}};
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("ProbeSpecularFilter{}SP", iteration)),
                                             "VoxelGI/probe_specular_filter.comp.spv", filterConstants));
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"));
    if (pRenderDevice->GetGPUInfo().rayQuery.IsUsable())
    {
        // Ray-query programs: hardware shadow occluders and the reduced-resolution fallback, which
        // only hardware providers select.
        StoreProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "LightOccludersCountsSP", "ShadowMapping/light_occluders_counts.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "LightOccludersSP", "ShadowMapping/light_occluders.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridReconstructFallbackSP",
                                             "VoxelGI/hybrid_reconstruct_fallback.comp.spv"));
        const char* const hardwareProbePrograms[][2] = {
            {"ProbeTraceHardwareSP", "VoxelGI/probe_trace_hardware.comp.spv"},
            {"ProbeTraceHardwareFieldSP", "VoxelGI/probe_trace_hardware_field.comp.spv"},
            {"ProbeSkySourcesHardwareSP", "VoxelGI/probe_sky_sources_hardware.comp.spv"},
            {"ProbeSkySourcesHardwareFieldSP", "VoxelGI/probe_sky_sources_hardware_field.comp.spv"},
            {"ProbeSpecularTraceHardwareSP", "VoxelGI/probe_specular_trace_hardware.comp.spv"},
            {"ProbeSpecularTraceHardwareFieldSP", "VoxelGI/probe_specular_trace_hardware_field.comp.spv"}};
        for (const char* const* program : hardwareProbePrograms)
        {
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program[0], program[1]));
        }
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareSP", "VoxelGI/hybrid_trace_hardware.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceReSTIRHardwareSP",
                                "VoxelGI/hybrid_trace_restir_hardware.comp.spv");
        const char* bufferedTracePrograms[][2] = {
            {"HybridTraceHardwareBufferedSP", "VoxelGI/hybrid_trace_hardware_buffered.comp.spv"},
            {"HybridTraceHardwareBufferedFieldSP", "VoxelGI/hybrid_trace_hardware_buffered_field.comp.spv"},
            {"HybridTraceReSTIRHardwareBufferedSP", "VoxelGI/hybrid_trace_restir_hardware_buffered.comp.spv"},
            {"HybridTraceReSTIRHardwareBufferedFieldSP", "VoxelGI/hybrid_trace_restir_hardware_buffered_field.comp.spv"}};
        for (const char* const* program : bufferedTracePrograms)
        {
            StoreBufferedTracePrograms(*this, pRenderDevice, program[0], program[1]);
        }
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceReSTIRHardwareFieldSP",
                                "VoxelGI/hybrid_trace_restir_hardware_field.comp.spv");
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRTemporalSP", "VoxelGI/restir_temporal.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRSpatialSP", "VoxelGI/restir_spatial.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRLobeIntegralSP", "VoxelGI/restir_lobe_integral.comp.spv"));
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareCountsSP",
                                "VoxelGI/hybrid_trace_hardware_counts.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareFieldSP",
                                "VoxelGI/hybrid_trace_hardware_field.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareCountsFieldSP",
                                "VoxelGI/hybrid_trace_hardware_counts_field.comp.spv");
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceHardwareSP",
                                             "VoxelGI/sky_irradiance_hardware.comp.spv", volumeConstants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectHardwareRadianceSP",
                                             "VoxelGI/inject_hardware_radiance.comp.spv", volumeConstants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectHardwareRadianceAveragedSP",
                                             "VoxelGI/inject_hardware_radiance_averaged.comp.spv", volumeConstants));
        // Diagnostic variants count the rays they issue (gi_ray_totals.glsl); never used for timing.
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceHardwareCountsSP",
                                             "VoxelGI/sky_irradiance_hardware_counts.comp.spv", volumeConstants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectHardwareRadianceCountsSP",
                                             "VoxelGI/inject_hardware_radiance_counts.comp.spv", volumeConstants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectHardwareRadianceAveragedCountsSP",
                                             "VoxelGI/inject_hardware_radiance_averaged_counts.comp.spv", volumeConstants));
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsSP", "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_shadows.frag.spv"));
        // Visibility variants also export each receiver's shadow-ray results for resting decals.
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsVisibilitySP",
                                                "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_shadows_visibility.frag.spv"));
        StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareShadowsSP",
                                     "SceneRenderer/forward_material_shadows.frag.spv");
        StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareLightingSP",
                                     "SceneRenderer/forward_material_rt_lighting.frag.spv");
        StoreProgram(ZEN_NEW()
                         HybridGraphicsSP(pRenderDevice, "ForwardScatterHardwareShadowsSP", "SceneRenderer/offscreen.vert.spv",
                                          "SceneRenderer/forward_scatter_shadows.frag.spv"));
        if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
        {
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCaptureSP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_capture.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCaptureVisibilitySP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_capture_visibility.frag.spv"));
            StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareShadowsCaptureSP",
                                         "SceneRenderer/forward_material_shadows_capture.frag.spv");
            StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareLightingCaptureSP",
                                         "SceneRenderer/forward_material_rt_lighting_capture.frag.spv");
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsSP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsVisibilitySP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts_visibility.frag.spv"));
            StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareShadowsCountsSP",
                                         "SceneRenderer/forward_material_shadows_counts.frag.spv");
            StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareLightingCountsSP",
                                         "SceneRenderer/forward_material_rt_lighting_counts.frag.spv");
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardScatterHardwareShadowsCountsSP",
                                                    "SceneRenderer/offscreen.vert.spv",
                                                    "SceneRenderer/forward_scatter_shadows_counts.frag.spv"));
        }
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridDiffuseUpsampleSP", "VoxelGI/hybrid_diffuse_upsample.comp.spv"));
    // First used after temporal history settles; warm it so that later frame does not stall.
    StoreWarmedComputeProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTemporalSP", "VoxelGI/hybrid_temporal.comp.spv"));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "HybridTemporalReferenceSP", "VoxelGI/hybrid_temporal_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTemporalCachedSP", "VoxelGI/hybrid_temporal_cached.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridFilterPrepareSP", "VoxelGI/hybrid_filter_prepare.comp.spv"));
    for (uint32_t iteration = 0; iteration < 5; ++iteration)
    {
        const HashMap<uint32_t, RHIShaderSpecializationValue> filterConstants{{0u, static_cast<int>(1u << iteration)}};
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("HybridFilter{}SP", iteration)),
                                             iteration == 4  ? "VoxelGI/hybrid_filter_final.comp.spv"
                                             : iteration < 2 ? "VoxelGI/hybrid_filter_cached.comp.spv"
                                                             : "VoxelGI/hybrid_filter.comp.spv",
                                             filterConstants));
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridSkySupportSP", "VoxelGI/hybrid_sky_support.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridSkySearchSP", "VoxelGI/hybrid_sky_search.comp.spv"));


    if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
    {
        StoreProgram(ZEN_NEW() DeferredLightingSP(pRenderDevice, true));

        StoreProgram(ZEN_NEW() DeferredVoxelGISP(pRenderDevice, true));
        StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHybridGICaptureSP",
                                     "SceneRenderer/forward_material_hybrid_capture.frag.spv");
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHybridGICaptureSP", "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_capture.frag.spv"));

        StoreProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "ClearLightingCaptureSP", "SceneRenderer/clear_lighting_capture.comp.spv"));
    }

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelClearOwnersAveragedSP", "VoxelGI/clear_owners_averaged.comp.spv",
                                         volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveAlbedoAveragedSP",
                                         "VoxelGI/resolve_albedo_averaged.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveSurfaceAveragedSP",
                                         "VoxelGI/resolve_surface_averaged.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectRadianceAveragedSP",
                                         "VoxelGI/inject_radiance_averaged.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelizationCompAveragedSP", "VoxelGI/voxelization_averaged.comp.spv"));

    StoreProgram(ZEN_NEW() LightMarkerSP(pRenderDevice));

    StoreProgram(ZEN_NEW() LightBallSP(pRenderDevice));

    StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardMaterialSP", "SceneRenderer/forward_material.frag.spv");

    StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardMaterialVoxelGISP",
                                 "SceneRenderer/forward_material_voxel.frag.spv");

    StoreProgram(ZEN_NEW() ForwardFullscreenSP(pRenderDevice, false));

    StoreProgram(ZEN_NEW() ForwardFullscreenSP(pRenderDevice, true));

    StoreProgram(ZEN_NEW() ForwardScatterSP(pRenderDevice));

    StoreProgram(ZEN_NEW() ForwardScatterSP(pRenderDevice, true));

    StoreProgram(ZEN_NEW() SceneShadowSP(pRenderDevice));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "SceneShadowMaxDepthSP", "ShadowMapping/scene_shadow_max_depth.comp.spv"));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv", volumeConstants));

    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectMeshRadianceSP", "VoxelGI/inject_mesh_radiance.comp.spv",
                                         volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectMeshRadianceAveragedSP",
                                         "VoxelGI/inject_mesh_radiance_averaged.comp.spv", volumeConstants));


    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelClearOwnersSP", "VoxelGI/clear_owners.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelResolveAlbedoSP", "VoxelGI/resolve_albedo.comp.spv", volumeConstants));

    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveSurfaceSP", "VoxelGI/resolve_surface.comp.spv", volumeConstants));

    StoreMaterialUVPrograms(*this, pRenderDevice, "GBufferSP", "SceneRenderer/offscreen.vert.spv",
                            "SceneRenderer/offscreen.frag.spv");
    StoreMaterialUVPrograms(*this, pRenderDevice, "GBufferMaskedSP", "SceneRenderer/offscreen.vert.spv",
                            "SceneRenderer/offscreen_masked.frag.spv");
    if (SupportsGBufferCulling(pRenderDevice->GetGPUInfo()))
    {
        StoreMaterialUVPrograms(*this, pRenderDevice, "GBufferIndirectSP", "SceneRenderer/offscreen_indirect.vert.spv",
                                "SceneRenderer/offscreen_indirect.frag.spv");
        StoreMaterialUVPrograms(*this, pRenderDevice, "GBufferMaskedIndirectSP", "SceneRenderer/offscreen_indirect.vert.spv",
                                "SceneRenderer/offscreen_masked_indirect.frag.spv");
        StoreWarmedComputeProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "SceneFrustumCullSP", "SceneRenderer/frustum_cull.comp.spv"));
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() DeferredLightingSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    // PBR with scene shadow maps while Compute GI waits for its distance field (RendererServer.cpp).
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredShadowMapsSP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/deferred_shadows.frag.spv"));

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapIrradianceSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapPrefilteredSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() SkyboxRenderSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() EnvMapBRDFLutGenSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    // Both supported producers must be available for runtime voxelizer changes.
    if (pRenderDevice->GetGPUInfo().supportGeometryShader)
    {
        {
            StoreProgram(ZEN_NEW() VoxelizationSP(pRenderDevice, true));

            ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationSP(pRenderDevice);

            StoreProgram(pShaderProgram);
        }

        {
            ShaderProgram* pShaderProgram = ZEN_NEW() VoxelDrawSP(pRenderDevice);

            StoreProgram(pShaderProgram);
        }
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetComputeIndirectSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    // Voxel debug views first use this program when selected; warm it to avoid that stall.
    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetDrawIndirectSP(pRenderDevice);

        StoreWarmedComputeProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetVoxelTextureSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationCompSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    // Also first used by voxel debug views.
    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelPreDrawSP(pRenderDevice, volumeConstants);

        StoreWarmedComputeProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelDrawSP2(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW()
            ComputeFileSP(pRenderDevice, "VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv", volumeConstants);

        StoreProgram(pShaderProgram);
    }
}
} // namespace zen::rc
