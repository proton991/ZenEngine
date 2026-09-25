#include "Graphics/RHI/RHIShaderUtil.h"
#include "Graphics/RenderCore/V2/ShaderProgram.h"
#include "Graphics/RenderCore/V2/RenderDevice.h"
#include "Memory/Memory.h"
#include "Utils/Errors.h"

namespace zen::rc
{
namespace
{
class VoxelCalibrationReferenceSP : public ShaderProgram
{
public:
    explicit VoxelCalibrationReferenceSP(RenderDevice* device) :
        ShaderProgram(device, "VoxelCalibrationReferenceSP")
    {
        AddShaderStage(RHIShaderStage::eVertex, "VoxelGI/voxelization.vert.spv");
        AddShaderStage(RHIShaderStage::eGeometry, "VoxelGI/Calibration/reference.geom.spv");
        AddShaderStage(RHIShaderStage::eFragment, "VoxelGI/Calibration/reference.frag.spv");
        Init();
    }
};
} // namespace

ShaderProgram::ShaderProgram(RenderDevice* pRenderDevice, NameID name) :
    m_pRenderDevice(pRenderDevice), m_name(name)
{}

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
        m_pRenderDevice->UpdateBuffer(m_uniformBufferMap[name], m_namedSRDLut[name]->blockSize,
                                      pData, offset);
    }
}

bool ShaderProgram::Init()
{
    return Init({});
}

bool ShaderProgram::Init(const HashMap<uint32_t, int>& specializationConstants)
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

    if (m_pShader == nullptr)
    {
        return;
    }

    m_storageBuffers.reserve(m_pShader->GetSRDCountByType(RHIShaderResourceType::eStorageBuffer));
    m_sampledTextures.reserve(
        m_pShader->GetSRDCountByType(RHIShaderResourceType::eSamplerWithTexture));
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
            else if (srd.type == RHIShaderResourceType::eTexture ||
                     srd.type == RHIShaderResourceType::eSamplerWithTexture)
            {
                m_sampledTextures.emplace_back(srd);
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
    const glm::uvec3 volumeGroupSize = ResolveVoxelVolumeWorkgroupSize(pRenderDevice->GetGPUInfo());
    const HashMap<uint32_t, int> volumeConstants{
        {ZEN_VOXEL_VOLUME_GROUP_X_ID, static_cast<int>(volumeGroupSize.x)},
        {ZEN_VOXEL_VOLUME_GROUP_Y_ID, static_cast<int>(volumeGroupSize.y)},
        {ZEN_VOXEL_VOLUME_GROUP_Z_ID, static_cast<int>(volumeGroupSize.z)}};
    LOGI("Voxel volume workgroup: {}x{}x{} (device invocation limit {})", volumeGroupSize.x,
         volumeGroupSize.y, volumeGroupSize.z,
         pRenderDevice->GetGPUInfo().maxComputeWorkGroupInvocations);
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureFrameSP",
                                         "SceneRenderer/capture_frame.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticCaptureSP",
                                         "VoxelGI/Dynamic/static_capture.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticProbeSP",
                                         "VoxelGI/Dynamic/static_probe.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticPrepareSP",
                                         "VoxelGI/Dynamic/static_prepare.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticCacheDDASP",
                                         "VoxelGI/Dynamic/static_cache_dda.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticCacheReferenceSP",
                                         "VoxelGI/Dynamic/static_cache_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticLightDDASP",
                                         "VoxelGI/Dynamic/static_light_dda.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticLightMeshSP",
                                         "VoxelGI/Dynamic/static_light_mesh.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticLightReferenceSP",
                                         "VoxelGI/Dynamic/static_light_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameDispatchProbeSP",
                                         "VoxelGI/Dynamic/frame_dispatch_probe.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameResetSP",
                                         "VoxelGI/Dynamic/frame_reset.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameSelectSP",
                                         "VoxelGI/Dynamic/frame_select.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameExpandSelectionSP",
                                         "VoxelGI/Dynamic/frame_expand_selection.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameNeighborhoodSP",
                                         "VoxelGI/Dynamic/frame_neighborhood.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameCompactSP",
                                         "VoxelGI/Dynamic/frame_compact.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameArgumentsSP",
                                         "VoxelGI/Dynamic/frame_arguments.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameClearSP",
                                         "VoxelGI/Dynamic/frame_clear.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIHistoryMetadataSP",
                                         "VoxelGI/Dynamic/frame_history_metadata.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIHistoryClearSP",
                                         "VoxelGI/Dynamic/frame_history_clear.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFilterCaptureSP",
                                         "VoxelGI/Dynamic/filter_capture.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GITemporalSP",
                                         "VoxelGI/Dynamic/frame_temporal.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GISpatialSP",
                                         "VoxelGI/Dynamic/frame_spatial.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameGatherReferenceSP",
                                         "VoxelGI/Dynamic/frame_gather_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticGatherSP",
                                         "VoxelGI/Dynamic/static_gather.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GISenderEnvironmentDDASP",
                                         "VoxelGI/Dynamic/sender_environment_dda.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GISenderEnvironmentReferenceSP",
                                         "VoxelGI/Dynamic/sender_environment_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIFrameGatherEnvironmentDDASP",
                                         "VoxelGI/Dynamic/frame_gather_environment_dda.comp.spv"));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "GIFrameGatherEnvironmentReferenceSP",
                                   "VoxelGI/Dynamic/frame_gather_environment_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIStaticPadSP",
                                         "VoxelGI/Dynamic/static_pad.comp.spv"));
    StoreProgram(ZEN_NEW() GBufferSP(pRenderDevice, true));
    StoreProgram(ZEN_NEW() DeferredDynamicVoxelGISP(pRenderDevice));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIQueryDDASP",
                                         "VoxelGI/Dynamic/query_dda.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "GIQueryReferenceSP",
                                         "VoxelGI/Dynamic/query_reference.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelCompactClearSP",
                                         "VoxelGI/compact_clear.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelCompactSP",
                                         "VoxelGI/compact.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelVolumeSP",
                                         "VoxelGI/capture_volume.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelBufferSP",
                                         "VoxelGI/capture_buffer.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelCaptureOwnersSP",
                                         "VoxelGI/Calibration/capture_owners.comp.spv"));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelCaptureGBufferSP",
                                         "VoxelGI/Calibration/capture_gbuffer.comp.spv"));
    StoreProgram(ZEN_NEW() DeferredVoxelGISP(pRenderDevice));
    if (pRenderDevice->GetGPUInfo().supportFragmentStoresAndAtomics)
    {
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ClearSurfaceCaptureSP",
                                             "SceneRenderer/clear_surface_capture.comp.spv"));
        StoreProgram(ZEN_NEW() DeferredDynamicVoxelGISP(pRenderDevice, true));
        StoreProgram(ZEN_NEW() DeferredLightingSP(pRenderDevice, true));
        StoreProgram(ZEN_NEW() DeferredVoxelGISP(pRenderDevice, true));
        StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "ClearLightingCaptureSP",
                                             "SceneRenderer/clear_lighting_capture.comp.spv"));
    }
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "CaptureVoxelReflectanceSP",
                                         "VoxelGI/capture_reflectance.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelClearOwnersAveragedSP",
                                   "VoxelGI/clear_owners_averaged.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelResolveAlbedoAveragedSP",
                                   "VoxelGI/resolve_albedo_averaged.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelResolveSurfaceAveragedSP",
                                   "VoxelGI/resolve_surface_averaged.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW()
                     ComputeFileSP(pRenderDevice, "VoxelInjectRadianceAveragedSP",
                                   "VoxelGI/inject_radiance_averaged.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelizationCompAveragedSP",
                                         "VoxelGI/voxelization_averaged.comp.spv"));

    StoreProgram(ZEN_NEW() LightMarkerSP(pRenderDevice));
    StoreProgram(ZEN_NEW() SceneShadowSP(pRenderDevice));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelFilterAlbedoSP",
                                         "VoxelGI/filter_albedo.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelFilterRadianceSP",
                                         "VoxelGI/filter_radiance.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelSkyIrradianceSP",
                                         "VoxelGI/sky_irradiance.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelClearOwnersSP",
                                         "VoxelGI/clear_owners.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveAlbedoSP",
                                         "VoxelGI/resolve_albedo.comp.spv", volumeConstants));
    StoreProgram(ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelResolveSurfaceSP",
                                         "VoxelGI/resolve_surface.comp.spv", volumeConstants));

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

    if (ResolveVoxelizerMode(platform::ConfigLoader::GetInstance().GetVoxelizerMode(),
                             pRenderDevice->GetGPUInfo()) == platform::VoxelizerMode::eGeometry)
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

    if (ResolveVoxelizerMode(platform::VoxelizerMode::eGeometry, pRenderDevice->GetGPUInfo()) ==
        platform::VoxelizerMode::eGeometry)
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
        ShaderProgram* pShaderProgram =
            ZEN_NEW() ComputeFileSP(pRenderDevice, "VoxelInjectRadianceSP",
                                    "VoxelGI/inject_radiance.comp.spv", volumeConstants);
        StoreProgram(pShaderProgram);
    }
}
} // namespace zen::rc
