#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"
#include <algorithm>

namespace zen::rc
{
namespace
{
// Both names use one SPIR-V module. The capture specialization reserves the
// larger exact accumulator only when that pipeline is selected.
void StoreGuideTracePrograms(ShaderProgramManager& manager, RenderDevice* device, const char* name, const char* path)
{
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

class VoxelCalibrationReferenceSP : public ShaderProgram
{
public:
    explicit VoxelCalibrationReferenceSP(RenderDevice* device) : ShaderProgram(device, "VoxelCalibrationReferenceSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");

        AddShaderStage(RHIShaderStage::eGeometry, "VoxelGI/Calibration/reference.geom.spv");

        AddShaderStage(RHIShaderStage::eFragment, "VoxelGI/Calibration/reference.frag.spv");

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

        info.stageFlags              = m_stageFlags;

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

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "CaptureVoxelVolumeSP", "VoxelGI/capture_volume.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelBufferSP", "VoxelGI/capture_buffer.comp.spv"));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelCaptureOwnersSP", "VoxelGI/Calibration/capture_owners.comp.spv"));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelCaptureGBufferSP", "VoxelGI/Calibration/capture_gbuffer.comp.spv"));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelVisibilityCheckSP", "VoxelGI/Calibration/visibility_check.comp.spv"));

    StoreProgram(ZEN_NEW() DeferredVoxelGISP(pRenderDevice));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "HybridReceiverSP", "SceneRenderer/receiver.vert.spv",
                                            "SceneRenderer/receiver.frag.spv"));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHybridGISP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/hybrid_gi.frag.spv"));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardHybridGISP", "SceneRenderer/offscreen.vert.spv",
                                            "SceneRenderer/forward_material_hybrid.frag.spv"));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardBackgroundDepthSP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/forward_background_depth.frag.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceLegacySP", "VoxelGI/sky_irradiance_legacy.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureHybridEnvironmentSP", "VoxelGI/capture_environment.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureSkyCacheSP", "VoxelGI/capture_sky_cache.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridCaptureSP", "VoxelGI/hybrid_capture.comp.spv"));
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceSP", "VoxelGI/hybrid_trace.comp.spv");
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceCountsSP", "VoxelGI/hybrid_trace_counts.comp.spv");
    // Brick scene construction (BrickScene.cpp) and the per-pixel trace with brick traversal.
    const char* const brickPrograms[][2] = {
        {"BrickClearSP", "VoxelGI/brick_clear.comp.spv"},
        {"BrickMarkSP", "VoxelGI/brick_mark.comp.spv"},
        {"BrickFinalizeSP", "VoxelGI/brick_finalize.comp.spv"},
        {"BrickMarkLargeSP", "VoxelGI/brick_mark_large.comp.spv"},
        {"BrickAllocateSP", "VoxelGI/brick_allocate.comp.spv"},
        {"BrickRasterizeSP", "VoxelGI/brick_rasterize.comp.spv"},
        {"BrickRasterizeLargeSP", "VoxelGI/brick_rasterize_large.comp.spv"},
        {"BrickPruneSP", "VoxelGI/brick_prune.comp.spv"},
        {"BrickDistanceXSP", "VoxelGI/brick_distance_x.comp.spv"},
        {"BrickDistanceYSP", "VoxelGI/brick_distance_y.comp.spv"},
        {"BrickDistanceZSP", "VoxelGI/brick_distance_z.comp.spv"},
        {"HybridTraceBricksSP", "VoxelGI/hybrid_trace_bricks.comp.spv"},
        {"HybridTraceCountsBricksSP", "VoxelGI/hybrid_trace_counts_bricks.comp.spv"},
        {"HybridTraceHalfBricksSP", "VoxelGI/hybrid_trace_half_bricks.comp.spv"},
        {"HybridTraceHalfCountsBricksSP", "VoxelGI/hybrid_trace_half_counts_bricks.comp.spv"},
        {"HybridTraceFallbackBricksSP", "VoxelGI/hybrid_trace_fallback_bricks.comp.spv"},
        {"HybridTraceFallbackCountsBricksSP", "VoxelGI/hybrid_trace_fallback_counts_bricks.comp.spv"},
        // Reference accumulation re-traces near-lamp hits inline instead of queueing them.
        {"HybridTraceReferenceBricksSP", "VoxelGI/hybrid_trace_reference_bricks.comp.spv"},
        {"HybridTraceReferenceCountsBricksSP", "VoxelGI/hybrid_trace_reference_counts_bricks.comp.spv"},
        {"HybridRetraceBricksSP", "VoxelGI/hybrid_retrace_bricks.comp.spv"},
        {"HybridRetraceCountsBricksSP", "VoxelGI/hybrid_retrace_counts_bricks.comp.spv"}};
    for (const char* const* program : brickPrograms)
    {
        const std::string name(program[0]);
        if (name.starts_with("HybridTrace") && !name.starts_with("HybridTraceFallback"))
        {
            StoreGuideTracePrograms(*this, pRenderDevice, program[0], program[1]);
        }
        else
        {
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program[0], program[1]));
        }
    }
    const char* const probePrograms[][2] = {{"ProbeClearSP", "VoxelGI/probe_clear.comp.spv"},
                                            {"ProbeClaimSP", "VoxelGI/probe_claim.comp.spv"},
                                            {"ProbePlaceSP", "VoxelGI/probe_place.comp.spv"},
                                            {"ProbeAdaptiveSP", "VoxelGI/probe_adaptive.comp.spv"},
                                            {"ProbeAllocateCountSP", "VoxelGI/probe_allocate_count.comp.spv"},
                                            {"ProbeAllocateSP", "VoxelGI/probe_allocate.comp.spv"},
                                            {"ProbeTraceSP", "VoxelGI/probe_trace.comp.spv"},
                                            {"ProbeTraceBricksSP", "VoxelGI/probe_trace_bricks.comp.spv"},
                                            {"ProbeRetraceBricksSP", "VoxelGI/probe_retrace_bricks.comp.spv"},
                                            {"ProbeTemporalSP", "VoxelGI/probe_temporal.comp.spv"},
                                            {"ProbeTemporalBricksSP", "VoxelGI/probe_temporal_bricks.comp.spv"},
                                            {"ProbeFilterSP", "VoxelGI/probe_filter.comp.spv"},
                                            {"ProbeIntegrateSP", "VoxelGI/probe_integrate.comp.spv"},
                                            {"ProbeSpecularTraceSP", "VoxelGI/probe_specular_trace.comp.spv"},
                                            {"ProbeSpecularTraceBricksSP", "VoxelGI/probe_specular_trace_bricks.comp.spv"},
                                            {"ProbeSpecularResolveSP", "VoxelGI/probe_specular_resolve.comp.spv"},
                                            {"ProbePixelTemporalSP", "VoxelGI/probe_pixel_temporal.comp.spv"},
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
        const char* const hardwareProbePrograms[][2] = {
            {"ProbeTraceHardwareSP", "VoxelGI/probe_trace_hardware.comp.spv"},
            {"ProbeTraceHardwareBricksSP", "VoxelGI/probe_trace_hardware_bricks.comp.spv"},
            {"ProbeSpecularTraceHardwareSP", "VoxelGI/probe_specular_trace_hardware.comp.spv"},
            {"ProbeSpecularTraceHardwareBricksSP", "VoxelGI/probe_specular_trace_hardware_bricks.comp.spv"}};
        for (const char* const* program : hardwareProbePrograms)
        {
            StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, program[0], program[1]));
        }
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareSP", "VoxelGI/hybrid_trace_hardware.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareCountsSP",
                                "VoxelGI/hybrid_trace_hardware_counts.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareBricksSP",
                                "VoxelGI/hybrid_trace_hardware_bricks.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareCountsBricksSP",
                                "VoxelGI/hybrid_trace_hardware_counts_bricks.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareReferenceBricksSP",
                                "VoxelGI/hybrid_trace_hardware_reference_bricks.comp.spv");
        StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHardwareReferenceCountsBricksSP",
                                "VoxelGI/hybrid_trace_hardware_reference_counts_bricks.comp.spv");
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
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardHardwareShadowsSP", "SceneRenderer/offscreen.vert.spv",
                                                "SceneRenderer/forward_material_shadows.frag.spv"));
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
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardHardwareShadowsCaptureSP",
                                                    "SceneRenderer/offscreen.vert.spv",
                                                    "SceneRenderer/forward_material_shadows_capture.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsSP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHardwareShadowsCountsVisibilitySP",
                                                    "SceneRenderer/deferred.vert.spv",
                                                    "SceneRenderer/hybrid_gi_shadows_counts_visibility.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardHardwareShadowsCountsSP",
                                                    "SceneRenderer/offscreen.vert.spv",
                                                    "SceneRenderer/forward_material_shadows_counts.frag.spv"));
            StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardScatterHardwareShadowsCountsSP",
                                                    "SceneRenderer/offscreen.vert.spv",
                                                    "SceneRenderer/forward_scatter_shadows_counts.frag.spv"));
        }
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectSoftwareRadianceSP",
                                         "VoxelGI/inject_software_radiance.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectSoftwareRadianceAveragedSP",
                                         "VoxelGI/inject_software_radiance_averaged.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceCountsSP", "VoxelGI/sky_irradiance_counts.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectSoftwareRadianceCountsSP",
                                         "VoxelGI/inject_software_radiance_counts.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectSoftwareRadianceAveragedCountsSP",
                                         "VoxelGI/inject_software_radiance_averaged_counts.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsSP", "SceneRenderer/deferred.vert.spv",
                                            "SceneRenderer/hybrid_gi_software_shadows.frag.spv"));
    StoreProgram(ZEN_NEW()
                     HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsVisibilitySP", "SceneRenderer/deferred.vert.spv",
                                      "SceneRenderer/hybrid_gi_software_shadows_visibility.frag.spv"));
    StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardSoftwareShadowsSP", "SceneRenderer/offscreen.vert.spv",
                                            "SceneRenderer/forward_material_software_shadows.frag.spv"));
    StoreProgram(ZEN_NEW()
                     HybridGraphicsSP(pRenderDevice, "ForwardScatterSoftwareShadowsSP", "SceneRenderer/offscreen.vert.spv",
                                      "SceneRenderer/forward_scatter_software_shadows.frag.spv"));
    if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
    {
        StoreProgram(ZEN_NEW()
                         HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsCaptureSP", "SceneRenderer/deferred.vert.spv",
                                          "SceneRenderer/hybrid_gi_software_shadows_capture.frag.spv"));
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsCaptureVisibilitySP",
                                                "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_software_shadows_capture_visibility.frag.spv"));
        StoreProgram(ZEN_NEW()
                         HybridGraphicsSP(pRenderDevice, "ForwardSoftwareShadowsCaptureSP", "SceneRenderer/offscreen.vert.spv",
                                          "SceneRenderer/forward_material_software_shadows_capture.frag.spv"));
        StoreProgram(ZEN_NEW()
                         HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsCountsSP", "SceneRenderer/deferred.vert.spv",
                                          "SceneRenderer/hybrid_gi_software_shadows_counts.frag.spv"));
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredSoftwareShadowsCountsVisibilitySP",
                                                "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_software_shadows_counts_visibility.frag.spv"));
        StoreProgram(ZEN_NEW()
                         HybridGraphicsSP(pRenderDevice, "ForwardSoftwareShadowsCountsSP", "SceneRenderer/offscreen.vert.spv",
                                          "SceneRenderer/forward_material_software_shadows_counts.frag.spv"));
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardScatterSoftwareShadowsCountsSP",
                                                "SceneRenderer/offscreen.vert.spv",
                                                "SceneRenderer/forward_scatter_software_shadows_counts.frag.spv"));
    }
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHalfSP", "VoxelGI/hybrid_trace_half.comp.spv");
    StoreGuideTracePrograms(*this, pRenderDevice, "HybridTraceHalfCountsSP", "VoxelGI/hybrid_trace_half_counts.comp.spv");
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceFallbackSP", "VoxelGI/hybrid_trace_fallback.comp.spv"));
    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceFallbackCountsSP", "VoxelGI/hybrid_trace_fallback_counts.comp.spv"));
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
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "ForwardHybridGICaptureSP", "SceneRenderer/offscreen.vert.spv",
                                                "SceneRenderer/forward_material_hybrid_capture.frag.spv"));
        StoreProgram(ZEN_NEW() HybridGraphicsSP(pRenderDevice, "DeferredHybridGICaptureSP", "SceneRenderer/deferred.vert.spv",
                                                "SceneRenderer/hybrid_gi_capture.frag.spv"));

        StoreProgram(
            ZEN_NEW() ComputeFileSP(pRenderDevice, "ClearLightingCaptureSP", "SceneRenderer/clear_lighting_capture.comp.spv"));
    }

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelReflectanceSP", "VoxelGI/capture_reflectance.comp.spv",
                                         volumeConstants));

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

    StoreProgram(ZEN_NEW() ForwardMaterialSP(pRenderDevice));

    StoreProgram(ZEN_NEW() ForwardMaterialSP(pRenderDevice, true));

    StoreProgram(ZEN_NEW() ForwardFullscreenSP(pRenderDevice, false));

    StoreProgram(ZEN_NEW() ForwardFullscreenSP(pRenderDevice, true));

    StoreProgram(ZEN_NEW() ForwardScatterSP(pRenderDevice));

    StoreProgram(ZEN_NEW() ForwardScatterSP(pRenderDevice, true));

    StoreProgram(ZEN_NEW() SceneShadowSP(pRenderDevice));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelFilterAlbedoSP", "VoxelGI/filter_albedo.comp.spv", volumeConstants));

    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelFilterRadianceSP", "VoxelGI/filter_radiance.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectMeshRadianceSP", "VoxelGI/inject_mesh_radiance.comp.spv",
                                         volumeConstants));

    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectMeshRadianceAveragedSP",
                                         "VoxelGI/inject_mesh_radiance_averaged.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceSP", "VoxelGI/sky_irradiance.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelClearOwnersSP", "VoxelGI/clear_owners.comp.spv", volumeConstants));

    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelResolveAlbedoSP", "VoxelGI/resolve_albedo.comp.spv", volumeConstants));

    StoreProgram(
        ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveSurfaceSP", "VoxelGI/resolve_surface.comp.spv", volumeConstants));

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() GBufferSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() DeferredLightingSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

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

    if (ResolveVoxelizerMode(platform::VoxelizerMode::eGeometry, pRenderDevice->GetGPUInfo())
        == platform::VoxelizerMode::eGeometry)
    {
        StoreProgram(ZEN_NEW() VoxelCalibrationReferenceSP(pRenderDevice));
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

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelizationLargeTriangleCompSP(pRenderDevice);

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
        ShaderProgram* pShaderProgram = ZEN_NEW() ShadowMapRenderSP(pRenderDevice);

        StoreProgram(pShaderProgram);
    }

    {
        ShaderProgram* pShaderProgram = ZEN_NEW()
            ComputeFileSP(pRenderDevice, "VoxelInjectRadianceSP", "VoxelGI/inject_radiance.comp.spv", volumeConstants);

        StoreProgram(pShaderProgram);
    }
}
} // namespace zen::rc
