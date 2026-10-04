#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <cmath>
#include <bit>

namespace zen::rc
{
VoxelGIRenderer::VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer) : m_device(device), m_voxelizer(voxelizer)
{
    LoadSettings();
}

VoxelGIVisibilityBounds BuildVoxelGIVisibilityBounds(const sg::AABB& geometryBounds,
                                                     const Vec4&     gridMinimumSize,
                                                     uint32_t        resolution)
{
    const Vec3 size(static_cast<float>(resolution));

    VoxelGIVisibilityBounds result{Vec4(0), Vec4(size, 0)};

    const Vec3 minimum = (geometryBounds.GetMin() - Vec3(gridMinimumSize)) / gridMinimumSize.w;

    const Vec3 maximum = (geometryBounds.GetMax() - Vec3(gridMinimumSize)) / gridMinimumSize.w;

    bool valid         = std::isfinite(gridMinimumSize.w) && gridMinimumSize.w > 0;

    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        valid = valid && std::isfinite(minimum[axis]) && std::isfinite(maximum[axis]) && minimum[axis] <= maximum[axis];
    }

    if (valid)
    {
        // Closed triangle/cell coverage includes both sides of an integer boundary.
        // Keep an extra cell for CPU/GPU transform and grid-conversion rounding.
        result.minimum = Vec4(glm::clamp(glm::floor(minimum) - Vec3(1), Vec3(0), size), 0);

        result.maximum = Vec4(glm::clamp(glm::floor(maximum) + Vec3(2), Vec3(0), size), 0);
    }

    return result;
}

bool ValidateVoxelGISettings(const VoxelGISettings& settings)
{
    return std::isfinite(settings.indirectIntensity) && settings.indirectIntensity >= 0 && settings.indirectIntensity <= 10
        && std::isfinite(settings.coneAngleDegrees) && settings.coneAngleDegrees >= 10 && settings.coneAngleDegrees <= 90
        && std::isfinite(settings.stepScale) && settings.stepScale >= 0.25f && settings.stepScale <= 2
        && std::isfinite(settings.normalBiasVoxels) && settings.normalBiasVoxels >= 0.5f && settings.normalBiasVoxels <= 4
        && std::isfinite(settings.maxDistanceGridLengths) && settings.maxDistanceGridLengths > 0
        && settings.maxDistanceGridLengths <= 2 && (settings.coneCount == 4 || settings.coneCount == 6)
        && settings.maxSteps >= 8 && settings.maxSteps <= 512;
}

bool VoxelGIRenderer::SetSettings(const VoxelGISettings& settings)
{
    const bool valid = ValidateVoxelGISettings(settings);

    if (valid)
    {
        if (settings.coneCount != m_settings.coneCount || settings.normalBiasVoxels != m_settings.normalBiasVoxels
            || settings.environmentLighting != m_settings.environmentLighting)
        {
            m_environmentRevision = 0;
        }

        if (settings.shadows != m_settings.shadows || settings.analyticLighting != m_settings.analyticLighting
            || settings.emissiveLighting != m_settings.emissiveLighting
            || settings.normalBiasVoxels != m_settings.normalBiasVoxels)
        {
            m_lightingRevision = 0;
        }

        m_settings = settings;
    }

    return valid;
}

bool LoadVoxelGISettings(const platform::ConfigLoader& config, VoxelGISettings& output)
{
    VoxelGISettings settings;

    bool valid  = config.ReadNumber("voxel_gi_indirect_intensity", settings.indirectIntensity);

    valid      &= config.ReadNumber("voxel_gi_cone_angle_degrees", settings.coneAngleDegrees);

    valid      &= config.ReadNumber("voxel_gi_step_scale", settings.stepScale);

    valid      &= config.ReadNumber("voxel_gi_normal_bias_voxels", settings.normalBiasVoxels);

    valid      &= config.ReadNumber("voxel_gi_max_distance_grid_lengths", settings.maxDistanceGridLengths);

    valid      &= config.ReadNumber("voxel_gi_cone_count", settings.coneCount);

    valid      &= config.ReadNumber("voxel_gi_max_steps", settings.maxSteps);

    valid      &= config.ReadBool("voxel_gi_shadow_enabled", settings.shadows);

    valid      &= config.ReadBool("voxel_gi_analytic_lighting", settings.analyticLighting);

    valid      &= config.ReadBool("voxel_gi_environment_lighting", settings.environmentLighting);

    valid      &= config.ReadBool("voxel_gi_emissive_lighting", settings.emissiveLighting);

    valid       = valid && ValidateVoxelGISettings(settings);

    if (valid)
    {
        output = settings;
    }

    return valid;
}

void VoxelGIRenderer::LoadSettings()
{
    if (!LoadVoxelGISettings(platform::ConfigLoader::GetInstance(), m_settings))
    {
        LOGW("Invalid voxel_gi settings; using defaults");
    }
}

bool VoxelGIRenderer::PrepareMipViews(RHITexture* texture, HeapVector<RHITextureView*>& views)
{
    bool valid = texture != nullptr;

    if (valid)
    {
        for (uint32_t mip = static_cast<uint32_t>(views.size()); mip < texture->GetNumMipmaps() && valid; ++mip)
        {
            TextureViewFormat format;

            format.dimension     = TextureDimension::e3D;

            format.baseMipLevel  = mip;

            RHITextureView* view = m_device->CreateTextureView(
                texture, format, NameID(fmt::format("{}_mip_{}", texture->GetBaseInfo().tag.CStr(), mip)));

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

        m_radiance       = m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_radiance");

        if (m_radiance != nullptr)
        {
            format.mipmaps  = 1;

            m_skyIrradiance = m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_sky_irradiance");
        }

        valid = IsInitialized() && PrepareMipViews(m_radiance, m_radianceMips)
             && PrepareMipViews(m_voxelizer->GetVoxelTextures().pAlbedo, m_albedoMips) && m_voxelizer->EnableRadianceInputs();

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

void VoxelGIRenderer::BuildMipChain(const HeapVector<RHITextureView*>& views, NameID program, const char* tag)
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

        const glm::uvec3 groups =
            GetVoxelVolumeDispatchGroups(std::max(1u, m_voxelizer->GetVoxelTexResolution() >> mip), m_device->GetGPUInfo());

        graph->AddComputePass(std::move(pass)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(groups.x, groups.y, groups.z);
        });
    }
}

void VoxelGIRenderer::BuildRenderGraph(SceneShadowRenderer* shadows)
{
    if (IsInitialized() && m_scene != nullptr)
    {
        m_uniforms.gridMinVoxelSize = Vec4(m_voxelizer->GetSceneMinPoint(), m_voxelizer->GetVoxelSize());

        // Scene bounds include committed instance transforms and vertex deformation,
        // even when the user has fixed a smaller voxel grid with SetVoxelBounds.
        m_visibilityBounds =
            BuildVoxelGIVisibilityBounds(m_scene->GetAABB(), m_uniforms.gridMinVoxelSize, m_voxelizer->GetVoxelTexResolution());

        m_uniforms.volume   = Vec4(static_cast<float>(m_voxelizer->GetVoxelTexResolution()), m_voxelizer->GetVoxelScale(),
                                   static_cast<float>(m_radianceMips.size()), m_settings.indirectIntensity);

        m_uniforms.cone     = Vec4(std::tan(glm::radians(m_settings.coneAngleDegrees) * 0.5f), m_settings.stepScale,
                                   m_settings.normalBiasVoxels, m_settings.maxDistanceGridLengths);

        m_uniforms.limits   = Vec4(static_cast<float>(m_settings.coneCount), static_cast<float>(m_settings.maxSteps),
                                 m_settings.shadows ? 1.0f : 0.0f, 0.0f);

        m_uniforms.lighting = Vec4(m_settings.analyticLighting ? 1.0f : 0.0f, m_settings.environmentLighting ? 1.0f : 0.0f,
                                   m_settings.emissiveLighting ? 1.0f : 0.0f, 0.0f);

        m_recordedGeometry            = m_voxelizer->GetRecordedGeometryRevision();

        m_recordedLighting            = m_scene->GetLightingRevision();

        m_recordedEnvironment         = m_scene->GetEnvironmentRevision();

        const bool geometryChanged    = m_geometryRevision != m_recordedGeometry;

        const bool skyChanged         = geometryChanged || m_environmentRevision != m_recordedEnvironment;

        const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();

        RHISampler* sampler           = m_voxelizer->GetVoxelSampler();

        RenderGraph* graph            = m_device->GetCurrentFrameRDG();

        const glm::uvec3 groups = GetVoxelVolumeDispatchGroups(m_voxelizer->GetVoxelTexResolution(), m_device->GetGPUInfo());

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

            sky.BindSampledTexture("skyboxMap", env.pPrefilteredSampler, env.pSkybox->GetDefaultView());

            sky.BindStorageImage("skyIrradiance", m_skyIrradiance->GetDefaultView(), RDGContentGuarantee::eFullWrite);

            graph->AddComputePass(std::move(sky)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
        }

        if (skyChanged || m_lightingRevision != m_recordedLighting)
        {
            RDGComputePassDesc inject;

            const bool meshShadows = shadows != nullptr && m_scene->GetVoxelTriangleCount() != 0;

            inject.SetShaderProgramName(meshShadows
                                            ? (m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectMeshRadianceAveragedSP"
                                                                                      : "VoxelInjectMeshRadianceSP")
                                        : m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectRadianceAveragedSP"
                                                                                 : "VoxelInjectRadianceSP");

            inject.SetPassTag("VoxelInjectRadiance");

            inject.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

            BindFrameData(inject);

            inject.BindSampledTexture("voxelAlbedo", sampler, textures.pAlbedoView);

            inject.BindSampledTexture("voxelNormal", sampler, textures.pNormalView);

            inject.BindSampledTexture("voxelEmissive", sampler, textures.pEmissiveView);

            if (meshShadows)
            {
                shadows->BindLightingInputs(inject);

                inject.BindStorageImage("voxelOwner", textures.pOwner->GetDefaultView());

                inject.BindStorageBuffer("VertexBuffer", m_scene->GetVertexBuffer());

                inject.BindStorageBuffer("IndexBuffer", m_scene->GetIndexBuffer());

                inject.BindStorageBuffer("NodeBuffer", m_scene->GetNodesDataSSBO());

                inject.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());
            }

            if (m_voxelizer->UsesAveragedReflectance())
            {
                inject.BindSampledTexture("voxelReflectance", sampler, textures.pReflectance->GetDefaultView());
            }

            inject.BindSampledTexture("skyIrradiance", sampler, m_skyIrradiance->GetDefaultView());

            inject.BindStorageImage("voxelRadiance", m_radianceMips[0], RDGContentGuarantee::eFullWrite);

            graph->AddComputePass(std::move(inject)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });

            BuildMipChain(m_radianceMips, "VoxelFilterRadianceSP", "VoxelRadianceMip");
        }
    }
}

void VoxelGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uGISettings", m_uniforms);

    pass.BindValue("uGIVisibilityBounds", m_visibilityBounds);

    pass.BindSampledTexture("voxelRadiance", m_voxelizer->GetVoxelSampler(), m_radiance->GetDefaultView());

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
