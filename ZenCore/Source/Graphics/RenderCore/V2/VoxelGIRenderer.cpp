#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <cmath>
#include <bit>

namespace zen::rc
{
VoxelGIRenderer::VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer) :
    m_device(device), m_voxelizer(voxelizer)
{
    LoadSettings();
}

bool VoxelGIRenderer::SetSettings(const VoxelGISettings& settings)
{
    const bool valid = std::isfinite(settings.indirectIntensity) &&
        settings.indirectIntensity >= 0 && settings.indirectIntensity <= 10 &&
        std::isfinite(settings.coneAngleDegrees) && settings.coneAngleDegrees >= 10 &&
        settings.coneAngleDegrees <= 90 && std::isfinite(settings.stepScale) &&
        settings.stepScale >= 0.25f && settings.stepScale <= 2 &&
        std::isfinite(settings.normalBiasVoxels) && settings.normalBiasVoxels >= 0.5f &&
        settings.normalBiasVoxels <= 4 && std::isfinite(settings.maxDistanceGridLengths) &&
        settings.maxDistanceGridLengths > 0 && settings.maxDistanceGridLengths <= 2 &&
        (settings.coneCount == 4 || settings.coneCount == 6) && settings.maxSteps >= 8 &&
        settings.maxSteps <= 512;
    if (valid)
    {
        if (settings.coneCount != m_settings.coneCount ||
            settings.normalBiasVoxels != m_settings.normalBiasVoxels)
        {
            m_environmentRevision = 0;
        }
        if (settings.shadows != m_settings.shadows)
        {
            m_lightingRevision = 0;
        }
        m_settings = settings;
    }
    return valid;
}

void VoxelGIRenderer::LoadSettings()
{
    const platform::ConfigLoader& config = platform::ConfigLoader::GetInstance();
    VoxelGISettings settings;
    bool valid = config.ReadNumber("voxel_gi_indirect_intensity", settings.indirectIntensity);
    valid &= config.ReadNumber("voxel_gi_cone_angle_degrees", settings.coneAngleDegrees);
    valid &= config.ReadNumber("voxel_gi_step_scale", settings.stepScale);
    valid &= config.ReadNumber("voxel_gi_normal_bias_voxels", settings.normalBiasVoxels);
    valid &=
        config.ReadNumber("voxel_gi_max_distance_grid_lengths", settings.maxDistanceGridLengths);
    valid &= config.ReadNumber("voxel_gi_cone_count", settings.coneCount);
    valid &= config.ReadNumber("voxel_gi_max_steps", settings.maxSteps);
    valid &= config.ReadBool("voxel_gi_shadow_enabled", settings.shadows);
    if (!valid || !SetSettings(settings))
    {
        LOGW("Invalid voxel_gi settings; using defaults");
    }
}

bool VoxelGIRenderer::PrepareMipViews(RHITexture* texture, HeapVector<RHITextureView*>& views)
{
    bool valid = texture != nullptr;
    if (valid)
    {
        for (uint32_t mip = static_cast<uint32_t>(views.size());
             mip < texture->GetNumMipmaps() && valid; ++mip)
        {
            TextureViewFormat format;
            format.dimension     = TextureDimension::e3D;
            format.baseMipLevel  = mip;
            RHITextureView* view = m_device->CreateTextureView(
                texture, format,
                NameID(fmt::format("{}_mip_{}", texture->GetBaseInfo().tag.CStr(), mip)));
            valid = view != nullptr;
            if (valid)
            {
                views.push_back(view);
            }
        }
    }
    return valid;
}

bool VoxelGIRenderer::Init()
{
    bool valid = IsInitialized();
    if (!valid && m_voxelizer != nullptr && m_voxelizer->EnsureReady())
    {
        TextureFormat format;
        format.dimension = TextureDimension::e3D;
        format.format    = DataFormat::eR16G16B16A16SFloat;
        format.width     = m_voxelizer->GetVoxelTexResolution();
        format.height    = format.width;
        format.depth     = format.width;
        format.mipmaps   = std::bit_width(format.width);
        m_radiance = m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_radiance");
        if (m_radiance != nullptr)
        {
            format.mipmaps = 1;
            m_skyIrradiance =
                m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_sky_irradiance");
        }
        valid = IsInitialized() && PrepareMipViews(m_radiance, m_radianceMips) &&
            PrepareMipViews(m_voxelizer->GetVoxelTextures().pAlbedo, m_albedoMips) &&
            m_voxelizer->EnableRadianceInputs();
        if (!valid)
        {
            LOGE("Voxel GI resource creation failed");
            Destroy();
        }
    }
    return valid;
}

void VoxelGIRenderer::SetRenderScene(RenderScene* scene)
{
    m_scene               = scene;
    m_geometryRevision    = 0;
    m_lightingRevision    = 0;
    m_environmentRevision = 0;
}

void VoxelGIRenderer::BindFrameData(RDGPassDescBase& pass) const
{
    pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    pass.BindValue("uGISettings", m_uniforms);
}

void VoxelGIRenderer::BuildMipChain(const HeapVector<RHITextureView*>& views,
                                    NameID program,
                                    const char* tag)
{
    RenderGraph* graph = m_device->GetCurrentFrameRDG();
    for (uint32_t mip = 1; mip < views.size(); ++mip)
    {
        RDGComputePassDesc pass;
        pass.SetShaderProgramName(program);
        pass.SetPassTag(NameID(fmt::format("{}_{}", tag, mip)));
        pass.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
        pass.BindStorageImage("sourceVolume", views[mip - 1]);
        pass.BindStorageImage("targetVolume", views[mip], RDGContentGuarantee::eFullWrite);
        const glm::uvec3 groups = GetVoxelVolumeDispatchGroups(
            std::max(1u, m_voxelizer->GetVoxelTexResolution() >> mip), m_device->GetGPUInfo());
        graph->AddComputePass(std::move(pass))
            .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
    }
}

void VoxelGIRenderer::BuildRenderGraph()
{
    if (IsInitialized() && m_scene != nullptr)
    {
        m_uniforms.gridMinVoxelSize =
            Vec4(m_voxelizer->GetSceneMinPoint(), m_voxelizer->GetVoxelSize());
        m_uniforms.volume = Vec4(
            static_cast<float>(m_voxelizer->GetVoxelTexResolution()), m_voxelizer->GetVoxelScale(),
            static_cast<float>(m_radianceMips.size()), m_settings.indirectIntensity);
        m_uniforms.cone =
            Vec4(std::tan(glm::radians(m_settings.coneAngleDegrees) * 0.5f), m_settings.stepScale,
                 m_settings.normalBiasVoxels, m_settings.maxDistanceGridLengths);
        m_uniforms.limits =
            Vec4(static_cast<float>(m_settings.coneCount), static_cast<float>(m_settings.maxSteps),
                 m_settings.shadows ? 1.0f : 0.0f, 0.0f);
        m_recordedGeometry         = m_voxelizer->GetRecordedGeometryRevision();
        m_recordedLighting         = m_scene->GetLights().GetRevision();
        m_recordedEnvironment      = m_scene->GetEnvironmentRevision();
        const bool geometryChanged = m_geometryRevision != m_recordedGeometry;
        const bool skyChanged = geometryChanged || m_environmentRevision != m_recordedEnvironment;
        const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();
        RHISampler* sampler           = m_voxelizer->GetVoxelSampler();
        RenderGraph* graph            = m_device->GetCurrentFrameRDG();
        const glm::uvec3 groups = GetVoxelVolumeDispatchGroups(m_voxelizer->GetVoxelTexResolution(),
                                                               m_device->GetGPUInfo());
        if (geometryChanged)
        {
            BuildMipChain(m_albedoMips, "VoxelFilterAlbedoSP", "VoxelOpacityMip");
        }
        if (skyChanged)
        {
            RDGComputePassDesc sky;
            sky.SetShaderProgramName("VoxelSkyIrradianceSP");
            sky.SetPassTag("VoxelSkyIrradiance");
            sky.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            BindFrameData(sky);
            sky.BindSampledTexture("voxelAlbedo", sampler, textures.pAlbedoView);
            sky.BindSampledTexture("voxelNormal", sampler, textures.pNormalView);
            const EnvTexture& env = m_scene->GetEnvTexture();
            sky.BindSampledTexture("skyboxMap", env.pPrefilteredSampler,
                                   env.pSkybox->GetDefaultView());
            sky.BindStorageImage("skyIrradiance", m_skyIrradiance->GetDefaultView(),
                                 RDGContentGuarantee::eFullWrite);
            graph->AddComputePass(std::move(sky))
                .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
        }
        if (skyChanged || m_lightingRevision != m_recordedLighting)
        {
            RDGComputePassDesc inject;
            inject.SetShaderProgramName(m_voxelizer->UsesAveragedReflectance() ?
                                            "VoxelInjectRadianceAveragedSP" :
                                            "VoxelInjectRadianceSP");
            inject.SetPassTag("VoxelInjectRadiance");
            inject.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);
            BindFrameData(inject);
            inject.BindSampledTexture("voxelAlbedo", sampler, textures.pAlbedoView);
            inject.BindSampledTexture("voxelNormal", sampler, textures.pNormalView);
            inject.BindSampledTexture("voxelEmissive", sampler, textures.pEmissiveView);
            if (m_voxelizer->UsesAveragedReflectance())
            {
                inject.BindSampledTexture("voxelReflectance", sampler,
                                          textures.pReflectance->GetDefaultView());
            }
            inject.BindSampledTexture("skyIrradiance", sampler, m_skyIrradiance->GetDefaultView());
            inject.BindStorageImage("voxelRadiance", m_radianceMips[0],
                                    RDGContentGuarantee::eFullWrite);
            graph->AddComputePass(std::move(inject))
                .RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                    encoder.Dispatch(groups.x, groups.y, groups.z);
                });
            BuildMipChain(m_radianceMips, "VoxelFilterRadianceSP", "VoxelRadianceMip");
        }
    }
}

void VoxelGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uGISettings", m_uniforms);
    pass.BindSampledTexture("voxelRadiance", m_voxelizer->GetVoxelSampler(),
                            m_radiance->GetDefaultView());
    pass.BindSampledTexture("voxelOpacity", m_voxelizer->GetVoxelSampler(),
                            m_voxelizer->GetVoxelTextures().pAlbedo->GetDefaultView());
    const EnvTexture& env = m_scene->GetEnvTexture();
    pass.BindSampledTexture("skyboxMap", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());
}

void VoxelGIRenderer::OnRenderGraphExecuted(bool succeeded)
{
    m_geometryRevision    = succeeded ? m_recordedGeometry : 0;
    m_lightingRevision    = succeeded ? m_recordedLighting : 0;
    m_environmentRevision = succeeded ? m_recordedEnvironment : 0;
}

void VoxelGIRenderer::Destroy()
{
    m_device->DestroyTexture(m_radiance);
    m_device->DestroyTexture(m_skyIrradiance);
    m_radiance      = nullptr;
    m_skyIrradiance = nullptr;
    m_radianceMips.clear();
    // These views belong to the voxelizer's surviving albedo texture. Reuse even a partial
    // list after failed GI initialization instead of accumulating owned views on each retry.
    m_geometryRevision    = 0;
    m_lightingRevision    = 0;
    m_environmentRevision = 0;
}
} // namespace zen::rc
