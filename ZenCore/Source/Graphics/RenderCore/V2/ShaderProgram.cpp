#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"

namespace zen::rc
{
namespace
{
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

void ShaderProgramManager::Destroy()
{
    for (std::pair<const NameID, ShaderProgram*>& kv : m_programCache)
    {
        ZEN_DELETE(kv.second);
    }

    m_programCache.clear();
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
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceLegacySP", "VoxelGI/sky_irradiance_legacy.comp.spv",
                                         volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureHybridEnvironmentSP", "VoxelGI/capture_environment.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureSkyCacheSP", "VoxelGI/capture_sky_cache.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridCaptureSP", "VoxelGI/hybrid_capture.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceSP", "VoxelGI/hybrid_trace.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentColumnsSP", "VoxelGI/environment_columns.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentRowsSP", "VoxelGI/environment_rows.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "EnvironmentTilesSP", "VoxelGI/environment_tiles.comp.spv"));
    if (pRenderDevice->GetGPUInfo().rayQuery.IsUsable())
    {
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTraceHardwareSP", "VoxelGI/hybrid_trace_hardware.comp.spv"));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceHardwareSP",
                                             "VoxelGI/sky_irradiance_hardware.comp.spv", volumeConstants));
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridTemporalSP", "VoxelGI/hybrid_temporal.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridFilterSP", "VoxelGI/hybrid_filter.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "HybridBiasSP", "VoxelGI/hybrid_bias.comp.spv"));


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

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() ResetDrawIndirectSP(pRenderDevice);

        StoreProgram(pShaderProgram);
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

    {
        ShaderProgram* pShaderProgram = ZEN_NEW() VoxelPreDrawSP(pRenderDevice, volumeConstants);

        StoreProgram(pShaderProgram);
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
