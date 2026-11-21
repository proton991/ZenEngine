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
#include "Platform/FileSystem.h"
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

// The generated list is the build's source of truth, including no-guide variants.
bool HasHitStreamVariant(const std::string& path)
{
    bool found = false;
    for (const char* candidate : kHitStreamShaderFiles)
    {
        found = found || path == candidate;
    }
    return found;
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
void StoreGuideTracePrograms(ShaderProgramManager& manager,
                             RenderDevice*         device,
                             std::string           name,
                             const std::string&    path,
                             bool                  guide = true)
{
    manager.StoreProgram(ZEN_NEW() ComputeFileSP(device, NameID(name), path));
    if (guide)
    {
        name.insert(name.size() - 2u, "Exact");
        const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{0u, true}};
        manager.StoreProgram(ZEN_NEW() ComputeFileSP(device, NameID(name), path, constants));
    }
}

void StoreRayGuidePrograms(ShaderProgramManager&                       manager,
                           RenderDevice*                               device,
                           std::string                                 name,
                           const HeapVector<ShaderProgram::StageFile>& modules,
                           bool                                        guide)
{
    manager.StoreProgram(ZEN_NEW() RayTracingFileSP(device, NameID(name), modules));
    if (guide)
    {
        name.insert(name.size() - 2u, "Exact");
        const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{0u, true}};
        manager.StoreProgram(ZEN_NEW() RayTracingFileSP(device, NameID(name), modules, true, constants));
    }
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
                if (HasHitStreamVariant(source))
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

        bool loaded                  = true;
        if (!m_rayTracingSources.empty())
        {
            info.spirv = MakeRefCountPtr<RHIShaderGroupSPIRV>();
            for (const StageFile& source : m_rayTracingSources)
            {
                std::string path = source.path;
                if (m_allowInvocationReorder
                    && std::find(std::begin(kInvocationReorderShaderFiles), std::end(kInvocationReorderShaderFiles), path)
                           != std::end(kInvocationReorderShaderFiles))
                {
                    const RHIShaderInvocationReorder reorder = GDynamicRHI->QueryGPUInfo().rayTracing.invocationReorder;
                    if (reorder != RHIShaderInvocationReorder::eNone)
                    {
                        path.insert(path.size() - 4u, reorder == RHIShaderInvocationReorder::eEXT ? ".ser_ext" : ".ser_nv");
                    }
                }
                if (RenderConfig::GetInstance().hybridHitStream && HasHitStreamVariant(path))
                {
                    path.insert(path.size() - 4u, ".stream");
                }
                platform::FileLoadError error = platform::FileLoadError::eNone;
                HeapVector<uint8_t>     code  = platform::FileSystem::LoadSpvFile(path, &error);
                loaded                        = error == platform::FileLoadError::eNone;
                if (!loaded)
                {
                    LOGE("Shader '{}' cannot load ray-tracing module '{}'", m_name.CStr(), path);
                    break;
                }
                info.spirv->AppendStageSPIRV(source.stage, std::move(code));
                if (RHIOptions::GetInstance().PipelineStatistics())
                {
                    LOGI("Shader module: shader='{}' source='{}'", m_name.CStr(), path);
                }
            }
        }

        RHIShader* shader = loaded ? GDynamicRHI->CreateShader(info) : nullptr;

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
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureHybridEnvironmentSP", "VoxelGI/capture_environment.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridDownsampleSP", "VoxelGI/hybrid_downsample.comp.spv"));
    // Footprint receivers: the reduced texel's shading normal and roughness describe its whole pixel block.
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridDownsampleFootprintSP", "VoxelGI/hybrid_downsample.comp.spv",
                                         HashMap<uint32_t, RHIShaderSpecializationValue>{{1u, true}}));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridReconstructSP", "VoxelGI/hybrid_reconstruct.comp.spv"));
    for (bool skyLight : {true, false})
    {
        const char*                                           suffix = skyLight ? "" : "NoSky";
        const HashMap<uint32_t, RHIShaderSpecializationValue> constants{{1u, skyLight}};
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoisePreblur{}SP", suffix)),
                                             "VoxelGI/gi_denoise_preblur.comp.spv", constants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseStabilize{}SP", suffix)),
                                             "VoxelGI/gi_denoise_stabilize.comp.spv", constants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseTemporal{}SP", suffix)),
                                             "VoxelGI/gi_denoise_temporal.comp.spv", constants));
        for (uint32_t bits : {32u, 64u})
        {
            if (bits == 32u || pRenderDevice->GetGPUInfo().supportBufferInt64Atomics)
            {
                StoreProgram(ZEN_NEW() ComputeFileSP(
                    pRenderDevice, NameID(fmt::format("GIDenoiseTemporalAntiLagBicubicSeed{}{}SP", bits, suffix)),
                    fmt::format("VoxelGI/gi_denoise_temporal_antilag_bicubic_seed{}.comp.spv", bits), constants));
            }
        }
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseHistoryFix{}SP", suffix)),
                                             "VoxelGI/gi_denoise_history_fix.comp.spv", constants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseTemporalValidation{}SP", suffix)),
                                             "VoxelGI/gi_denoise_temporal_validation.comp.spv", constants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseHistoryFixValidation{}SP", suffix)),
                                             "VoxelGI/gi_denoise_history_fix_validation.comp.spv", constants));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseTemporalBicubic{}SP", suffix)),
                                             "VoxelGI/gi_denoise_temporal_bicubic.comp.spv", constants));
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseTemporalValidationBicubic{}SP", suffix)),
                                       "VoxelGI/gi_denoise_temporal_validation_bicubic.comp.spv", constants));
        for (bool validation : {false, true})
        {
            const char* diagnostic   = validation ? "Validation" : "";
            const char* temporalFile = validation ? "VoxelGI/gi_denoise_temporal_validation_antilag.comp.spv"
                                                  : "VoxelGI/gi_denoise_temporal_antilag.comp.spv";
            const char* repairFile   = validation ? "VoxelGI/gi_denoise_history_fix_validation_antilag.comp.spv"
                                                  : "VoxelGI/gi_denoise_history_fix_antilag.comp.spv";
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice,
                                                 NameID(fmt::format("GIDenoiseTemporal{}AntiLag{}SP", diagnostic, suffix)),
                                                 temporalFile, constants));
            StoreProgram(ZEN_NEW() ComputeFileSP(
                pRenderDevice, NameID(fmt::format("GIDenoiseTemporal{}AntiLagBicubic{}SP", diagnostic, suffix)),
                validation ? "VoxelGI/gi_denoise_temporal_validation_antilag_bicubic.comp.spv"
                           : "VoxelGI/gi_denoise_temporal_antilag_bicubic.comp.spv",
                constants));
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice,
                                                 NameID(fmt::format("GIDenoiseHistoryFix{}AntiLag{}SP", diagnostic, suffix)),
                                                 repairFile, constants));
            for (bool antiLag : {false, true})
            {
                // Anti-firefly limits young texels to their brightest matching neighbour.
                StoreProgram(ZEN_NEW() ComputeFileSP(
                    pRenderDevice,
                    NameID(fmt::format("GIDenoiseHistoryFix{}{}AntiFirefly{}SP", diagnostic, antiLag ? "AntiLag" : "", suffix)),
                    fmt::format("VoxelGI/gi_denoise_history_fix{}{}_antifirefly.comp.spv", validation ? "_validation" : "",
                                antiLag ? "_antilag" : ""),
                    constants));
            }
        }
        if (!skyLight)
        {
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridReconstructNoSkySP",
                                                 "VoxelGI/hybrid_reconstruct.comp.spv", constants));
        }
        for (uint32_t iteration = 0; iteration < 3; ++iteration)
        {
            HashMap<uint32_t, RHIShaderSpecializationValue> spatialConstants(constants);
            spatialConstants.emplace(0u, uint32_t(1u << iteration));
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GIDenoiseAtrous{}{}SP", iteration, suffix)),
                                                 "VoxelGI/gi_denoise_atrous.comp.spv", spatialConstants));
        }
    }
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "GIDenoiseDebugSP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/debug_gi_denoise.frag.spv"));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "HybridCaptureValidationSP", "VoxelGI/hybrid_capture_validation.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridCaptureSP", "VoxelGI/hybrid_capture.comp.spv"));
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceFieldYoungSP", "VoxelGI/hybrid_trace_field_young.comp.spv");
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceNoGuideFieldYoungSP",
                                         "VoxelGI/hybrid_trace_noguide_field_young.comp.spv"));
    // Compute GI traces the distance field (DistanceFieldScene); trace variants without Field are HW RT only.
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceFieldSP", "VoxelGI/hybrid_trace_field.comp.spv");
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "HybridTraceNoGuideFieldSP", "VoxelGI/hybrid_trace_noguide_field.comp.spv"));
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceCountsFieldSP", "VoxelGI/hybrid_trace_counts_field.comp.spv");
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceFieldSP", "VoxelGI/sky_irradiance_field.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceFieldCountsSP",
                                         "VoxelGI/sky_irradiance_field_counts.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"));
    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "ScreenProbeFilterVerticalSP", "VoxelGI/screen_probe_filter_vertical.comp.spv"));
    const char* probeStages[] = {"Clear",  "Place",    "Proposal",     "Integrate",    "Filter",
                                 "Gather", "Specular", "ClearDiffuse", "GatherDiffuse"};
    const char* probeFiles[]  = {"clear",  "place",    "proposal",      "integrate",     "filter",
                                 "gather", "specular", "clear_diffuse", "gather_diffuse"};
    for (uint32_t stage = 0; stage < std::size(probeStages); ++stage)
    {
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("ScreenProbe{}SP", probeStages[stage])),
                                             fmt::format("VoxelGI/screen_probe_{}.comp.spv", probeFiles[stage]).c_str()));
    }
    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "ScreenProbeAdaptiveSP", "VoxelGI/screen_probe_place.comp.spv", {{0u, true}}));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "WorldCacheInitializeSP", "VoxelGI/world_cache_initialize.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "WorldCacheLightingSP", "VoxelGI/world_cache_lighting.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GlossyClearSP", "VoxelGI/glossy_reflection_clear.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GlossyTemporalSP", "VoxelGI/glossy_reflection_temporal.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GlossyFilterSP", "VoxelGI/glossy_reflection_filter.comp.spv"));
    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "ScreenProbeSpecularFilterSP", "VoxelGI/screen_probe_specular_filter.comp.spv"));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "WorldCacheDisplaySP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/debug_world_cache.frag.spv"));
    for (uint32_t bits : {32u, 64u})
    {
        if (bits == 32u || pRenderDevice->GetGPUInfo().supportBufferInt64Atomics)
        {
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("WorldCacheCapture{}SP", bits)),
                                                 fmt::format("VoxelGI/world_cache_capture{}.comp.spv", bits)));
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("ScreenProbeCapture{}SP", bits)),
                                                 bits == 32u ? "VoxelGI/screen_probe_capture.comp.spv"
                                                             : "VoxelGI/screen_probe_capture64.comp.spv"));
            for (bool noGuide : {false, true})
            {
                for (bool young : {false, true})
                {
                    StoreGuideTracePrograms(
                        *this, pRenderDevice,
                        fmt::format("WorldCacheTraceField{}{}{}SP", noGuide ? "NoGuide" : "", young ? "Young" : "", bits),
                        fmt::format("VoxelGI/hybrid_trace{}_field{}.comp.world{}.spv", noGuide ? "_noguide" : "",
                                    young ? "_young" : "", bits),
                        !noGuide);
                }
            }
            const char* worldStages[] = {"Start", "Resolve", "Debug"};
            const char* worldFiles[]  = {"start", "resolve", "debug"};
            for (uint32_t stage = 0; stage < std::size(worldStages); ++stage)
            {
                StoreProgram(
                    ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("WorldCache{}{}SP", worldStages[stage], bits)),
                                            fmt::format("VoxelGI/world_cache_{}{}.comp.spv", worldFiles[stage], bits).c_str()));
            }
            const bool wave = pRenderDevice->GetGPUInfo().supportComputeSubgroupReduction;
            for (bool field : {false, true})
            {
                if (field || SupportsHybridRayTracingPipeline(pRenderDevice->GetGPUInfo()))
                {
                    const NameID program(
                        fmt::format("WorldCacheShade{}{}{}SP", field ? "Field" : "", wave ? "Wave" : "", bits));
                    const std::string file = fmt::format("VoxelGI/world_cache_shade{}{}{}.comp.spv", field ? "_field" : "",
                                                         wave ? "_wave" : "", bits);
                    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program, file.c_str()));
                }
            }
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("ScreenProbeTraceField{}SP", bits)),
                                                 fmt::format("VoxelGI/screen_probe_trace_field{}.comp.spv", bits).c_str()));
            StoreProgram(ZEN_NEW()
                             ComputeFileSP(pRenderDevice, NameID(fmt::format("GlossyTraceField{}SP", bits)),
                                           fmt::format("VoxelGI/glossy_reflection_trace_field{}.comp.spv", bits).c_str()));
            StoreProgram(ZEN_NEW()
                             ComputeFileSP(pRenderDevice, NameID(fmt::format("GlossyUncoveredField{}SP", bits)),
                                           fmt::format("VoxelGI/glossy_reflection_uncovered_field{}.comp.spv", bits).c_str()));
        }
    }
    if (SupportsHybridRayTracingPipeline(pRenderDevice->GetGPUInfo()))
    {
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, "ScreenProbeAccumulateSP", "VoxelGI/screen_probe_accumulate.comp.spv"));
        for (uint32_t bits : {32u, 64u})
        {
            if (bits == 32u || pRenderDevice->GetGPUInfo().supportBufferInt64Atomics)
            {
                const HeapVector<ShaderProgram::StageFile> worldModules = {
                    {RHIShaderStage::eRayGeneration, "VoxelGI/world_cache_trace.rgen.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_radiance.rmiss.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                    {RHIShaderStage::eClosestHit, fmt::format("VoxelGI/world_cache_hit_record{}.rchit.spv", bits)},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance.rahit.spv"},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
                StoreProgram(ZEN_NEW() RayTracingFileSP(pRenderDevice, NameID(fmt::format("WorldCacheTraceHits{}SP", bits)),
                                                        worldModules));
                HeapVector<ShaderProgram::StageFile> screenModules = worldModules;
                screenModules[0].path                              = "VoxelGI/screen_probe_trace_hits.rgen.spv";
                StoreProgram(ZEN_NEW() RayTracingFileSP(pRenderDevice, NameID(fmt::format("ScreenProbeTraceHits{}SP", bits)),
                                                        screenModules));
                const char* splitStages[] = {"SampleHits", "ShadeHits"};
                const char* splitFiles[]  = {"sample_hits", "shade_hits"};
                for (uint32_t stage = 0; stage < std::size(splitStages); ++stage)
                {
                    StoreProgram(ZEN_NEW() ComputeFileSP(
                        pRenderDevice, NameID(fmt::format("ScreenProbe{}{}SP", splitStages[stage], bits)),
                        fmt::format("VoxelGI/screen_probe_{}{}.comp.spv", splitFiles[stage], bits).c_str()));
                }
                screenModules[0].path = "VoxelGI/glossy_reflection_trace.rgen.spv";
                screenModules[3].path = fmt::format("VoxelGI/glossy_reflection_hit{}.rchit.spv", bits);
                StoreProgram(ZEN_NEW()
                                 RayTracingFileSP(pRenderDevice, NameID(fmt::format("GlossyTrace{}SP", bits)), screenModules));
                screenModules[0].path = "VoxelGI/glossy_reflection_uncovered.rgen.spv";
                StoreProgram(
                    ZEN_NEW() RayTracingFileSP(pRenderDevice, NameID(fmt::format("GlossyUncovered{}SP", bits)), screenModules));
                const HeapVector<ShaderProgram::StageFile> probeModules = {
                    {RHIShaderStage::eRayGeneration, "VoxelGI/native_probe_benchmark.rgen.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_radiance_cache.rmiss.spv"},
                    {RHIShaderStage::eClosestHit, fmt::format("VoxelGI/native_probe_hit{}.rchit.spv", bits)},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance_cache.rahit.spv"}};
                StoreProgram(ZEN_NEW() RayTracingFileSP(pRenderDevice, NameID(fmt::format("NativeProbeBenchmark{}SP", bits)),
                                                        probeModules));
                StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, NameID(fmt::format("GICacheResolve{}SP", bits)),
                                                     fmt::format("VoxelGI/gi_cache_resolve{}.comp.spv", bits).c_str()));
                StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, NameID(fmt::format("GICacheDebug{}SP", bits)),
                                                        "SceneRenderer/deferred.vert.spv",
                                                        fmt::format("SceneRenderer/debug_gi_cache{}.frag.spv", bits).c_str()));
                const HeapVector<ShaderProgram::StageFile> cacheCountModules = {
                    {RHIShaderStage::eRayGeneration, "VoxelGI/hybrid_trace_counts_cache.rgen.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_radiance_counts_cache.rmiss.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                    {RHIShaderStage::eClosestHit, fmt::format("VoxelGI/gi_cache_query{}_counts.rchit.spv", bits)},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance_counts_cache.rahit.spv"},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_shadow_counts.rahit.spv"}};
                StoreRayGuidePrograms(*this, pRenderDevice, fmt::format("GICacheTraceCounts{}SP", bits), cacheCountModules,
                                      true);
                for (bool noGuide : {false, true})
                {
                    const char*                          suffix  = noGuide ? "_noguide" : "";
                    HeapVector<ShaderProgram::StageFile> modules = {
                        {RHIShaderStage::eRayGeneration, "VoxelGI/gi_cache_update.rgen.spv"},
                        {RHIShaderStage::eMiss, "VoxelGI/ray_radiance_cache.rmiss.spv"},
                        {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                        {RHIShaderStage::eClosestHit, fmt::format("VoxelGI/gi_cache_update{}{}.rchit.spv", bits, suffix)},
                        {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance_cache.rahit.spv"},
                        {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
                    StoreProgram(ZEN_NEW() RayTracingFileSP(
                        pRenderDevice, NameID(fmt::format("GICacheUpdate{}{}SP", bits, noGuide ? "NoGuide" : "")), modules));
                    modules[3] = {RHIShaderStage::eClosestHit,
                                  fmt::format("VoxelGI/gi_cache_query{}{}.rchit.spv", bits, suffix)};
                    for (bool restir : {false, true})
                    {
                        for (bool young : {false, true})
                        {
                            modules[0] = {RHIShaderStage::eRayGeneration,
                                          fmt::format("VoxelGI/hybrid_trace{}{}{}_cache.rgen.spv", restir ? "_restir" : "",
                                                      suffix, young ? "_young" : "")};
                            HeapVector<ShaderProgram::StageFile> worldTraceModules = worldModules;
                            worldTraceModules[0] = {RHIShaderStage::eRayGeneration,
                                                    fmt::format("VoxelGI/hybrid_trace{}{}{}.rgen.spv", restir ? "_restir" : "",
                                                                suffix, young ? "_young" : "")};
                            worldTraceModules[3] = {RHIShaderStage::eClosestHit,
                                                    fmt::format("VoxelGI/world_cache_pixel_hit{}.rchit.spv", bits)};
                            StoreRayGuidePrograms(*this, pRenderDevice,
                                                  fmt::format("WorldCacheTrace{}{}{}{}SP", restir ? "ReSTIR" : "",
                                                              noGuide ? "NoGuide" : "", young ? "Young" : "", bits),
                                                  worldTraceModules, !noGuide);
                            StoreRayGuidePrograms(*this, pRenderDevice,
                                                  fmt::format("GICacheTrace{}{}{}{}SP", restir ? "ReSTIR" : "",
                                                              noGuide ? "NoGuide" : "", young ? "Young" : "", bits),
                                                  modules, !noGuide);
                        }
                    }
                }
            }
        }
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridGuidePrepareSP", "VoxelGI/hybrid_guide_prepare.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridGuidePrepareCountsSP",
                                "VoxelGI/hybrid_guide_prepare_counts.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridGuideFinishSP", "VoxelGI/hybrid_guide_finish.comp.spv");
        for (bool restir : {false, true})
        {
            for (bool noGuide : {false, true})
            {
                HeapVector<ShaderProgram::StageFile> modules = {
                    {RHIShaderStage::eRayGeneration,
                     noGuide
                         ? (restir ? "VoxelGI/hybrid_trace_restir_noguide.rgen.spv" : "VoxelGI/hybrid_trace_noguide.rgen.spv")
                         : (restir ? "VoxelGI/hybrid_trace_restir.rgen.spv" : "VoxelGI/hybrid_trace.rgen.spv")},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_radiance.rmiss.spv"},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                    {RHIShaderStage::eClosestHit,
                     noGuide ? "VoxelGI/hybrid_hit_noguide.rchit.spv" : "VoxelGI/hybrid_hit.rchit.spv"},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance.rahit.spv"},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
                const char* name = noGuide ? (restir ? "HybridTraceReSTIRNoGuidePipelineSP" : "HybridTraceNoGuidePipelineSP")
                                           : (restir ? "HybridTraceReSTIRPipelineSP" : "HybridTracePipelineSP");
                for (bool young : {false, true})
                {
                    std::string traceName(name);
                    if (young)
                    {
                        traceName.insert(traceName.size() - 2u, "Young");
                        modules[0].path.insert(modules[0].path.find(".rgen"), "_young");
                    }
                    StoreRayGuidePrograms(*this, pRenderDevice, traceName, modules, !noGuide);
                }
            }
        }
        const HeapVector<ShaderProgram::StageFile> countModules = {
            {RHIShaderStage::eRayGeneration, "VoxelGI/hybrid_trace_counts.rgen.spv"},
            {RHIShaderStage::eMiss, "VoxelGI/ray_radiance_counts.rmiss.spv"},
            {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
            {RHIShaderStage::eClosestHit, "VoxelGI/hybrid_hit_counts.rchit.spv"},
            {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_radiance_counts.rahit.spv"},
            {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_shadow_counts.rahit.spv"}};
        StoreRayGuidePrograms(*this, pRenderDevice, "HybridTracePipelineCountsSP", countModules, true);
        const char* const restirPrograms[][2] = {
            {"ReSTIRTemporalPipelineSP", "VoxelGI/restir_temporal.rgen.spv"},
            {"ReSTIRSpatialDiffusePipelineSP", "VoxelGI/restir_spatial_diffuse.rgen.spv"},
            {"ReSTIRSpatialReflectionPipelineSP", "VoxelGI/restir_spatial_reflection.rgen.spv"}};
        for (const char* const(&program)[2] : restirPrograms)
        {
            const HeapVector<ShaderProgram::StageFile> modules = {{RHIShaderStage::eRayGeneration, program[1]},
                                                                  {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                                                                  {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
            StoreProgram(ZEN_NEW() RayTracingFileSP(pRenderDevice, program[0], modules));
        }
        for (bool counts : {false, true})
        {
            const HeapVector<ShaderProgram::StageFile> occluders = {
                {RHIShaderStage::eRayGeneration,
                 counts ? "ShadowMapping/light_occluders_counts.rgen.spv" : "ShadowMapping/light_occluders.rgen.spv"},
                {RHIShaderStage::eMiss, "VoxelGI/ray_triangle.rmiss.spv"},
                {RHIShaderStage::eClosestHit, "VoxelGI/ray_triangle.rchit.spv"},
                {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha_triangle.rahit.spv"}};
            StoreProgram(ZEN_NEW() RayTracingFileSP(
                pRenderDevice, counts ? "LightOccludersPipelineCountsSP" : "LightOccludersPipelineSP", occluders));
            for (bool wide : {false, true})
            {
                const HeapVector<ShaderProgram::StageFile> modules = {
                    {RHIShaderStage::eRayGeneration, wide ? (counts ? "ShadowMapping/receiver_visibility_wide_counts.rgen.spv"
                                                                    : "ShadowMapping/receiver_visibility_wide.rgen.spv")
                                                          : (counts ? "ShadowMapping/receiver_visibility_counts.rgen.spv"
                                                                    : "ShadowMapping/receiver_visibility.rgen.spv")},
                    {RHIShaderStage::eMiss, "VoxelGI/ray_shadow.rmiss.spv"},
                    {RHIShaderStage::eAnyHit, "VoxelGI/ray_alpha.rahit.spv"}};
                const char* name = wide ? (counts ? "ReceiverVisibilityWideCountsSP" : "ReceiverVisibilityWideSP")
                                        : (counts ? "ReceiverVisibilityCountsSP" : "ReceiverVisibilitySP");
                StoreProgram(ZEN_NEW() RayTracingFileSP(pRenderDevice, name, modules));
                if (!counts)
                {
                    for (bool coarse : {false, true})
                    {
                        HeapVector<ShaderProgram::StageFile> nativeModules = modules;
                        nativeModules[0].path = fmt::format("ShadowMapping/receiver_visibility_{}{}.rgen.spv",
                                                            coarse ? "coarse" : "refine", wide ? "_wide" : "");
                        StoreProgram(ZEN_NEW() RayTracingFileSP(
                            pRenderDevice,
                            NameID(fmt::format("ReceiverVisibility{}{}SP", coarse ? "Coarse" : "Refine", wide ? "Wide" : "")),
                            nativeModules));
                    }
                }
            }
        }
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredPipelineShadowsSP", "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_visibility_read.frag.spv"));
        if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
        {
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredPipelineShadowsCaptureSP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_visibility_read_capture.frag.spv"));
        }
    }
    if (pRenderDevice->GetGPUInfo().rayQuery.IsUsable())
    {
        // Ray-query programs, which only hardware providers select.
        StoreProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "LightOccludersCountsSP", "ShadowMapping/light_occluders_counts.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "LightOccludersSP", "ShadowMapping/light_occluders.comp.spv"));
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareYoungSP",
                                "VoxelGI/hybrid_trace_hardware_young.comp.spv");
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceHardwareNoGuideYoungSP",
                                             "VoxelGI/hybrid_trace_hardware_noguide_young.comp.spv"));
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceReSTIRHardwareYoungSP",
                                "VoxelGI/hybrid_trace_restir_hardware_young.comp.spv");
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceReSTIRHardwareNoGuideYoungSP",
                                             "VoxelGI/hybrid_trace_restir_hardware_noguide_young.comp.spv"));
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareSP", "VoxelGI/hybrid_trace_hardware.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceReSTIRHardwareSP",
                                "VoxelGI/hybrid_trace_restir_hardware.comp.spv");
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceHardwareNoGuideSP",
                                             "VoxelGI/hybrid_trace_hardware_noguide.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceReSTIRHardwareNoGuideSP",
                                             "VoxelGI/hybrid_trace_restir_hardware_noguide.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRTemporalSP", "VoxelGI/restir_temporal.comp.spv"));
        // Spatial reuse runs one signal per dispatch; see restir_reuse.glsl.
        StoreProgram(ZEN_NEW()
                         ComputeFileSP(pRenderDevice, "ReSTIRSpatialDiffuseSP", "VoxelGI/restir_spatial_diffuse.comp.spv"));
        StoreProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRSpatialReflectionSP", "VoxelGI/restir_spatial_reflection.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ReSTIRLobeIntegralSP", "VoxelGI/restir_lobe_integral.comp.spv"));
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareCountsSP",
                                "VoxelGI/hybrid_trace_hardware_counts.comp.spv");
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsSP", "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_shadows.frag.spv"));
        // Visibility variants also export each receiver's shadow-ray results for resting decals.
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsVisibilitySP",
                                                "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_shadows_visibility.frag.spv"));
        StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareShadowsSP",
                                     "SceneRenderer/forward_material_shadows.frag.spv");
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
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsSP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsVisibilitySP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts_visibility.frag.spv"));
            StoreForwardMaterialPrograms(*this, pRenderDevice, "ForwardHardwareShadowsCountsSP",
                                         "SceneRenderer/forward_material_shadows_counts.frag.spv");
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardScatterHardwareShadowsCountsSP",
                                                    "SceneRenderer/offscreen.vert.spv",
                                                    "SceneRenderer/forward_scatter_shadows_counts.frag.spv"));
        }
    }
    // Reference diagnostic mean; every other GI frame uses the GIDenoise passes.
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "HybridTemporalReferenceSP", "VoxelGI/hybrid_temporal_reference.comp.spv"));


    if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
    {
        StoreProgram(ZEN_NEW() DeferredLightingSP(pRenderDevice, true));

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
