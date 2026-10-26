#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelizerBase.h"
#include "Graphics/RenderCore/V2/Renderer/SceneShadowRenderer.h"
#include "Graphics/RenderCore/V2/RenderScene.h"
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include "Graphics/Shared/EnvironmentSampling.h"
#include "Graphics/Shared/VoxelGI.h"
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include <cmath>
#include <bit>
#include <iterator>

namespace zen::rc
{
namespace
{
// light_visibility_cache.glsl: a light count and the cached lights precede the class masks.
constexpr uint64_t kLightCacheHeaderBytes = sizeof(Vec4) + MaxSceneLights * sizeof(GPULight);

// Light-visibility caches are copied into their history, so they are transfer sources and destinations.
RHIBuffer* CreateLightCache(RenderDevice* device, uint64_t bytes, NameID name)
{
    RHIBufferCreateInfo info;
    info.size         = bytes;
    info.tag          = name;
    info.allocateType = RHIBufferAllocateType::eGPU;
    info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eStorageBuffer, RHIBufferUsageFlagBits::eTransferSrcBuffer,
                             RHIBufferUsageFlagBits::eTransferDstBuffer);
    return device->CreateBuffer(info);
}
} // namespace

bool VoxelGISettings::UsesAdaptiveSampling() const
{
    return adaptiveSampling && samples == 4 && referenceSamples == 0;
}

bool VoxelGISettings::UsesRayShadows(bool hardwareReady) const
{
    // Inherited Compute uses maps; only an explicit shadow provider opts into the software BVH.
    return shadows && rtShadows
        && ((hardwareReady && GetFeatureProvider(Feature::Shadows) == RayProvider::Hardware)
            || featureProviders[static_cast<size_t>(Feature::Shadows)] == RayProvider::Voxel);
}

VoxelGISettings::RayProvider VoxelGISettings::GetFeatureProvider(Feature feature) const
{
    const RayProvider selected  = featureProviders[static_cast<size_t>(feature)];
    const RayProvider inherited = selected == RayProvider::Auto ? rayProvider : selected;
    return inherited == RayProvider::Voxel ? RayProvider::Voxel : RayProvider::Hardware;
}

uint32_t VoxelGISettings::GetHardwareFeatureMask() const
{
    uint32_t mask = 0;
    for (uint32_t index = 0; index < static_cast<uint32_t>(Feature::Count); ++index)
    {
        if (GetFeatureProvider(static_cast<Feature>(index)) == RayProvider::Hardware)
        {
            mask |= 1u << index;
        }
    }
    return mask;
}

void VoxelGISettings::SetFeatureProvider(Feature feature, RayProvider provider)
{
    featureProviders[static_cast<size_t>(feature)] = provider;
    // Explicit feature selection supersedes the old algorithm/enable overrides.
    switch (feature)
    {
        case Feature::Bounce: bounceSource = BounceSource::Auto; break;
        case Feature::Reflections:
            reflections       = Reflections::On;
            specularOcclusion = true;
            break;
        case Feature::SpecularOcclusion: specularOcclusion = true; break;
        case Feature::Shadows:
            shadows   = true;
            rtShadows = true;
            break;
        default: break;
    }
}

void VoxelGISettings::SetAllFeatureProviders(RayProvider provider)
{
    rayProvider = provider;
    for (uint32_t index = 0; index < static_cast<uint32_t>(Feature::Count); ++index)
    {
        SetFeatureProvider(static_cast<Feature>(index), provider);
    }
}

VoxelGIRenderer::VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer) :
    m_rayQuery(device), m_softwareRayQuery(device), m_bricks(device), m_device(device), m_voxelizer(voxelizer)
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
    bool providersValid = true;
    for (VoxelGISettings::RayProvider provider : settings.featureProviders)
    {
        providersValid =
            providersValid && static_cast<uint32_t>(provider) <= static_cast<uint32_t>(VoxelGISettings::RayProvider::Hardware);
    }
    return providersValid && std::isfinite(settings.indirectIntensity) && settings.indirectIntensity >= 0
        && settings.indirectIntensity <= 10 && std::isfinite(settings.coneAngleDegrees) && settings.coneAngleDegrees >= 10
        && settings.coneAngleDegrees <= 90 && std::isfinite(settings.stepScale) && settings.stepScale >= 0.25f
        && settings.stepScale <= 2 && std::isfinite(settings.normalBiasVoxels) && settings.normalBiasVoxels >= 0.5f
        && settings.normalBiasVoxels <= 4 && std::isfinite(settings.maxDistanceGridLengths)
        && settings.maxDistanceGridLengths > 0 && settings.maxDistanceGridLengths <= 2
        && (settings.coneCount == 4 || settings.coneCount == 6) && settings.maxSteps >= 8 && settings.maxSteps <= 512
        && (settings.samples == 1 || settings.samples == 2 || settings.samples == 4)
        && (settings.referenceSamples == 0 || settings.referenceSamples == 256 || settings.referenceSamples == 1024
            || settings.referenceSamples == 4096)
        && settings.historyFrames >= 1 && settings.historyFrames <= 256
        && static_cast<uint32_t>(settings.bounceSource) <= static_cast<uint32_t>(VoxelGISettings::BounceSource::Rays)
        && static_cast<uint32_t>(settings.reflections) <= static_cast<uint32_t>(VoxelGISettings::Reflections::On)
        && static_cast<uint32_t>(settings.diffuseResolution) <= static_cast<uint32_t>(VoxelGISettings::DiffuseResolution::Half)
        && static_cast<uint32_t>(settings.computeTraversal) <= static_cast<uint32_t>(VoxelGISettings::ComputeTraversal::Bricks)
        && (settings.brickResolution == 64 || settings.brickResolution == 128 || settings.brickResolution == 256)
        && static_cast<uint32_t>(settings.gather) <= static_cast<uint32_t>(VoxelGISettings::Gather::Probes)
        && (settings.probeSpacing == 8 || settings.probeSpacing == 16)
        && static_cast<uint32_t>(settings.rayProvider) <= static_cast<uint32_t>(VoxelGISettings::RayProvider::Hardware);
}

bool VoxelGIRenderer::SetSettings(const VoxelGISettings& settings)
{
    const bool valid = ValidateVoxelGISettings(settings);

    if (valid)
    {
        if (settings.coneCount != m_settings.coneCount || settings.normalBiasVoxels != m_settings.normalBiasVoxels
            || settings.environmentLighting != m_settings.environmentLighting
            || settings.coneAngleDegrees != m_settings.coneAngleDegrees
            || settings.GetFeatureProvider(VoxelGISettings::Feature::SkyCache)
                   != m_settings.GetFeatureProvider(VoxelGISettings::Feature::SkyCache))
        {
            m_environmentRevision = 0;
        }

        if (settings.shadows != m_settings.shadows || settings.rtShadows != m_settings.rtShadows
            || settings.UsesRayShadows(m_hardwareQueries) != m_settings.UsesRayShadows(m_hardwareQueries)
            || settings.GetFeatureProvider(VoxelGISettings::Feature::Shadows)
                   != m_settings.GetFeatureProvider(VoxelGISettings::Feature::Shadows)
            || settings.analyticLighting != m_settings.analyticLighting
            || settings.emissiveLighting != m_settings.emissiveLighting
            || settings.normalBiasVoxels != m_settings.normalBiasVoxels)
        {
            m_lightingRevision = 0;
        }

        m_settings = settings;
    }

    return valid;
}

bool LoadVoxelGISettings(const platform::ConfigLoader& config, VoxelGISettings& output, const VoxelGISettings& defaults)
{
    VoxelGISettings settings  = defaults;

    bool valid                = config.ReadNumber("voxel_gi_indirect_intensity", settings.indirectIntensity);

    valid                    &= config.ReadNumber("voxel_gi_cone_angle_degrees", settings.coneAngleDegrees);

    valid                    &= config.ReadNumber("voxel_gi_step_scale", settings.stepScale);

    valid                    &= config.ReadNumber("voxel_gi_normal_bias_voxels", settings.normalBiasVoxels);

    valid                    &= config.ReadNumber("voxel_gi_max_distance_grid_lengths", settings.maxDistanceGridLengths);

    valid                    &= config.ReadNumber("voxel_gi_cone_count", settings.coneCount);

    valid                    &= config.ReadNumber("voxel_gi_max_steps", settings.maxSteps);

    valid                    &= config.ReadBool("voxel_gi_shadow_enabled", settings.shadows);
    valid                    &= config.ReadBool("voxel_gi_rt_shadows", settings.rtShadows);

    valid                    &= config.ReadBool("voxel_gi_analytic_lighting", settings.analyticLighting);

    valid                    &= config.ReadBool("voxel_gi_environment_lighting", settings.environmentLighting);

    valid                    &= config.ReadBool("voxel_gi_emissive_lighting", settings.emissiveLighting);

    valid                    &= config.ReadNumber("voxel_gi_samples", settings.samples);
    valid                    &= config.ReadBool("voxel_gi_adaptive_sampling", settings.adaptiveSampling);
    valid &= config.ReadNumber("voxel_gi_acceleration_structure_budget_mb", settings.accelerationStructureBudgetMB);
    valid &= config.ReadNumber("voxel_gi_reference_samples", settings.referenceSamples);
    valid &= config.ReadNumber("voxel_gi_history_frames", settings.historyFrames);
    valid &= config.ReadBool("voxel_gi_temporal", settings.temporal);
    valid &= config.ReadBool("voxel_gi_previous_frame_radiance", settings.previousFrameRadiance);
    valid &= config.ReadBool("voxel_gi_filter", settings.filter);
    valid &= config.ReadBool("voxel_gi_indirect_diffuse", settings.indirectDiffuse);
    valid &= config.ReadBool("voxel_gi_specular_occlusion", settings.specularOcclusion);
    const char*       defaultReflections  = settings.reflections == VoxelGISettings::Reflections::Off  ? "false"
                                          : settings.reflections == VoxelGISettings::Reflections::Auto ? "auto"
                                                                                                       : "true";
    const char*       defaultResolution   = settings.diffuseResolution == VoxelGISettings::DiffuseResolution::Full ? "full"
                                          : settings.diffuseResolution == VoxelGISettings::DiffuseResolution::Half ? "half"
                                                                                                                   : "auto";
    const std::string resolution          = config.GetString("voxel_gi_diffuse_resolution", defaultResolution);
    valid                                &= resolution == "auto" || resolution == "full" || resolution == "half";
    settings.diffuseResolution            = resolution == "full" ? VoxelGISettings::DiffuseResolution::Full
                                          : resolution == "half" ? VoxelGISettings::DiffuseResolution::Half
                                                                 : VoxelGISettings::DiffuseResolution::Auto;
    const char* defaultTraversal =
        settings.computeTraversal == VoxelGISettings::ComputeTraversal::Bricks ? "bricks" : "triangles";
    const std::string traversal  = config.GetString("voxel_gi_compute_traversal", defaultTraversal);
    valid                       &= traversal == "triangles" || traversal == "bricks";
    settings.computeTraversal =
        traversal == "bricks" ? VoxelGISettings::ComputeTraversal::Bricks : VoxelGISettings::ComputeTraversal::Triangles;
    valid &= config.ReadNumber("voxel_gi_brick_resolution", settings.brickResolution);
    const std::string gather =
        config.GetString("voxel_gi_gather", settings.gather == VoxelGISettings::Gather::Probes ? "probes" : "pixel");
    valid                             &= gather == "pixel" || gather == "probes";
    settings.gather                    = gather == "probes" ? VoxelGISettings::Gather::Probes : VoxelGISettings::Gather::Pixel;
    valid                             &= config.ReadNumber("voxel_gi_probe_spacing", settings.probeSpacing);
    const std::string reflections      = config.GetString("voxel_gi_reflections", defaultReflections);
    valid                             &= reflections == "auto" || reflections == "true" || reflections == "false";
    settings.reflections               = reflections == "true"  ? VoxelGISettings::Reflections::On
                                       : reflections == "false" ? VoxelGISettings::Reflections::Off
                                                                : VoxelGISettings::Reflections::Auto;
    const char*       defaultProvider  = settings.rayProvider == VoxelGISettings::RayProvider::Voxel    ? "voxel"
                                       : settings.rayProvider == VoxelGISettings::RayProvider::Hardware ? "hardware"
                                                                                                        : "auto";
    const std::string provider         = config.GetString("voxel_gi_ray_provider", defaultProvider);
    const std::string bounce           = config.GetString("voxel_gi_bounce_source", "auto");
    valid                             &= bounce == "auto" || bounce == "cone" || bounce == "rays";
    settings.bounceSource              = bounce == "rays" ? VoxelGISettings::BounceSource::Rays
                                       : bounce == "cone" ? VoxelGISettings::BounceSource::Cone
                                                          : VoxelGISettings::BounceSource::Auto;
    valid                             &= provider == "auto" || provider == "voxel" || provider == "hardware";
    settings.rayProvider               = provider == "hardware" ? VoxelGISettings::RayProvider::Hardware
                                       : provider == "voxel"    ? VoxelGISettings::RayProvider::Voxel
                                                                : VoxelGISettings::RayProvider::Auto;
    // Preserve explicit legacy software-shadow requests; inherited preset providers use maps without HW RT.
    if (config.HasKey("voxel_gi_ray_provider") && settings.rayProvider == VoxelGISettings::RayProvider::Voxel
        && settings.rtShadows)
    {
        settings.featureProviders[static_cast<size_t>(VoxelGISettings::Feature::Shadows)] = VoxelGISettings::RayProvider::Voxel;
    }

    const char*       featureKeys[]  = {"voxel_gi_sky_backend",    "voxel_gi_specular_backend",    "voxel_gi_sky_cache_backend",
                                        "voxel_gi_bounce_backend", "voxel_gi_reflections_backend", "voxel_gi_shadows_backend"};
    const std::string backend        = config.GetString("voxel_gi_backend", "");
    valid                           &= backend.empty() || backend == "compute" || backend == "hardware";
    if (!backend.empty())
    {
        settings.SetAllFeatureProviders(backend == "compute" ? VoxelGISettings::RayProvider::Voxel
                                                             : VoxelGISettings::RayProvider::Hardware);
    }
    for (uint32_t index = 0; index < static_cast<uint32_t>(VoxelGISettings::Feature::Count); ++index)
    {
        const std::string value  = config.GetString(featureKeys[index], "");
        valid                   &= value.empty() || value == "compute" || value == "hardware";
        if (!value.empty())
        {
            settings.SetFeatureProvider(static_cast<VoxelGISettings::Feature>(index),
                                        value == "compute" ? VoxelGISettings::RayProvider::Voxel
                                                           : VoxelGISettings::RayProvider::Hardware);
        }
    }
    valid = valid && ValidateVoxelGISettings(settings);

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

        if (m_skyIrradiance != nullptr)
        {
            m_environmentColumns = m_device->CreateStorageBuffer(ZEN_ENVIRONMENT_IMPORTANCE_ROWS
                                                                     * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u) * sizeof(float),
                                                                 nullptr, "environment_columns");
            m_environmentRows = m_device->CreateStorageBuffer((ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 1u) * sizeof(float), nullptr,
                                                              "environment_rows");
            m_environmentHarmonics = m_device->CreateStorageBuffer((ZEN_ENVIRONMENT_HARMONIC_GROUPS + 1u)
                                                                       * ZEN_ENVIRONMENT_HARMONIC_COEFFICIENTS * sizeof(Vec4),
                                                                   nullptr, "environment_harmonics");
            m_environmentLuminance =
                m_device->CreateStorageBuffer(ZEN_ENVIRONMENT_IMPORTANCE_ROWS * ZEN_ENVIRONMENT_IMPORTANCE_SIZE * sizeof(float),
                                              nullptr, "environment_luminance");
            m_environmentTiles =
                m_device->CreateStorageBuffer(ZEN_ENVIRONMENT_TILE_CDF_SIZE * sizeof(float), nullptr, "environment_tiles");
            // Source count, source tiles and the residual tile prefix sums (environment_tiles.comp).
            m_environmentSources = m_device->CreateStorageBuffer((1u + ZEN_ENVIRONMENT_MAX_SOURCES + ZEN_ENVIRONMENT_TILES + 1u)
                                                                     * sizeof(uint32_t),
                                                                 nullptr, "environment_sources");
            m_environmentResidualColumns = m_device->CreateStorageBuffer(
                ZEN_ENVIRONMENT_IMPORTANCE_ROWS * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u) * sizeof(float), nullptr,
                "environment_residual_columns");
            m_environmentResidualRows = m_device->CreateStorageBuffer((ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 1u) * sizeof(float),
                                                                      nullptr, "environment_residual_rows");
        }

        valid = IsInitialized() && PrepareMipViews(m_radiance, m_radianceMips)
             && PrepareMipViews(m_voxelizer->GetVoxelTextures().pAlbedo, m_albedoMips) && m_voxelizer->EnableRadianceInputs();

        if (valid)
        {
            // Hit-radiance bindings need the light-visibility caches before a frame graph sizes them.
            PrepareLightCache(false);

            valid = m_lightCache != nullptr && m_previousLightCache != nullptr && m_hitLightCachePrevious != nullptr;
        }

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
    m_rayQuery.Destroy();
    m_softwareRayQuery.Destroy();
    m_bricks.Destroy();
    m_bricksActive             = false;
    m_hardwareQueries          = false;
    m_hardwareFeatureMask      = 0;
    m_preparingSoftwareQueries = false;
    m_softwareQueriesActive    = false;
    m_scene                    = scene;

    m_geometryRevision         = 0;

    m_lightingRevision         = 0;

    m_environmentRevision      = 0;

    m_radianceValid            = false;

    if (m_settings.previousFrameRadiance)
    {
        m_previousRadianceInitialized = false;
    }
}

void VoxelGIRenderer::PrepareRadianceHistory(bool update)
{
    const bool rays             = UsesRayBounce() || UsesReflections();

    const uint32_t resolution   = m_voxelizer->GetVoxelTexResolution();

    const uint32_t previousSize = rays || m_settings.previousFrameRadiance ? resolution : 1u;

    const uint32_t previousMips = m_settings.previousFrameRadiance ? m_radiance->GetNumMipmaps() : 1u;

    if (m_previousRadiance != nullptr
        && (m_previousRadiance->GetWidth() != previousSize || m_previousRadiance->GetNumMipmaps() != previousMips))
    {
        m_previousRadianceMips.clear();

        m_device->DestroyTexture(m_previousRadiance);

        m_previousRadiance = nullptr;
    }

    TextureFormat format;

    format.dimension = TextureDimension::e3D;

    format.format    = DataFormat::eR16G16B16A16SFloat;

    format.width     = previousSize;

    format.height    = previousSize;

    format.depth     = previousSize;

    format.mipmaps   = previousMips;

    if (m_previousRadiance == nullptr)
    {
        m_previousRadiance            = m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_previous_radiance");

        m_previousRadianceInitialized = false;
    }

    RenderGraph* graph = m_device->GetCurrentFrameRDG();

    if (m_previousRadiance != nullptr && !m_previousRadianceInitialized)
    {
        graph->AddTransferPass("InitializePreviousRadiance")
            .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
            .ClearTexture(m_previousRadiance, Color(0));

        m_recordedPreviousRadianceInitialization = true;
    }

    const bool needsHitSnapshot = m_settings.previousFrameRadiance && rays;

    if (m_hitRadiancePrevious != nullptr && (!needsHitSnapshot || m_hitRadiancePrevious->GetWidth() != resolution))
    {
        m_device->DestroyTexture(m_hitRadiancePrevious);

        m_hitRadiancePrevious = nullptr;
    }

    bool delayedReady = m_settings.previousFrameRadiance && PrepareMipViews(m_previousRadiance, m_previousRadianceMips);

    if (delayedReady && needsHitSnapshot && m_hitRadiancePrevious == nullptr)
    {
        format.mipmaps        = 1;

        m_hitRadiancePrevious = m_device->CreateTextureStorage(format, {.copyUsage = true}, "voxel_hit_radiance_previous");
    }

    delayedReady = delayedReady && (!needsHitSnapshot || m_hitRadiancePrevious != nullptr);

    if (m_settings.previousFrameRadiance && !delayedReady)
    {
        LOGW("Previous-frame voxel radiance resource creation failed; updating current radiance");
    }

    m_recordedDelayedRadiance = update && m_radianceValid && delayedReady;

    // While compute overwrites the alternate chain, Hybrid GI still needs both
    // older and newer hit samples along the same rays. Preserve only its base level.
    if (update && rays && m_previousRadiance != nullptr && m_radianceValid)
    {
        RHITextureCopyRegion region{};

        region.size = Vec3i(static_cast<int>(resolution));

        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        region.srcSubresources.layerCount = 1;

        region.dstSubresources            = region.srcSubresources;

        RHITexture* source                = m_recordedDelayedRadiance ? m_previousRadiance : m_radiance;

        RHITexture* destination           = m_recordedDelayedRadiance ? m_hitRadiancePrevious : m_previousRadiance;

        graph->AddTransferPass(m_recordedDelayedRadiance ? "VoxelKeepHitRadianceHistory" : "VoxelKeepPreviousRadiance")
            .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
            .CopyTexture(source, destination, MakeVecView(&region, 1));

        if (m_lightCacheActive)
        {
            RHIBuffer* sourceCache      = m_recordedDelayedRadiance ? m_previousLightCache : m_lightCache;

            RHIBuffer* destinationCache = m_recordedDelayedRadiance ? m_hitLightCachePrevious : m_previousLightCache;

            graph->AddTransferPass(m_recordedDelayedRadiance ? "VoxelKeepHitLightCache" : "VoxelKeepPreviousLightCache")
                .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                .CopyBuffer(sourceCache, destinationCache, {0, 0, m_lightCacheBytes});
        }
    }
}

void VoxelGIRenderer::PrepareLightCache(bool requested)
{
    // The cache has a mask per class texel of the voxelizer's class owners.
    const RHITexture* classOwner = m_voxelizer->GetVoxelTextures().pClassOwner;

    const uint64_t headerBytes   = kLightCacheHeaderBytes + sizeof(uint32_t);

    uint64_t bytes               = headerBytes;

    bool active                  = requested && classOwner != nullptr;

    if (active)
    {
        const uint64_t resolution = classOwner->GetWidth();

        const uint64_t masks      = uint64_t(ZEN_VOXEL_SURFACE_CLASSES) * resolution * resolution * resolution;

        active                    = ValidateGIStorageBuffer(masks + kLightCacheHeaderBytes / sizeof(uint32_t), sizeof(uint32_t),
                                                            m_device->GetGPUInfo(), bytes)
                                 == GIResourceStatus::eSuccess;

        bytes                     = active ? bytes : headerBytes;
    }

    // Only previous-frame radiance snapshots the older cache before overwriting the alternate one.
    const uint64_t snapshotBytes = active && m_settings.previousFrameRadiance ? bytes : headerBytes;

    if (m_lightCache == nullptr || m_previousLightCache == nullptr || m_hitLightCachePrevious == nullptr
        || bytes != m_lightCacheBytes || snapshotBytes != m_lightCacheSnapshotBytes)
    {
        m_device->DestroyBuffer(m_lightCache);

        m_device->DestroyBuffer(m_previousLightCache);

        m_device->DestroyBuffer(m_hitLightCachePrevious);

        m_lightCache            = CreateLightCache(m_device, bytes, "voxel_light_cache");

        m_previousLightCache    = CreateLightCache(m_device, bytes, "voxel_previous_light_cache");

        m_hitLightCachePrevious = CreateLightCache(m_device, snapshotBytes, "voxel_hit_light_cache_previous");

        // A zero light count makes a lookup of a never-injected cache dark instead of undefined.
        const HeapVector<uint8_t> zeros(kLightCacheHeaderBytes, uint8_t{0});

        for (RHIBuffer* cache : {m_lightCache, m_previousLightCache, m_hitLightCachePrevious})
        {
            if (cache != nullptr)
            {
                m_device->UpdateBuffer(cache, static_cast<uint32_t>(kLightCacheHeaderBytes), zeros.data());
            }
        }

        m_lightCacheBytes              = bytes;

        m_lightCacheSnapshotBytes      = snapshotBytes;

        m_lightCacheHistoryInitialized = false;

        if (requested && !active)
        {
            LOGW("Voxel light-visibility cache does not fit a storage buffer; hits use the isotropic radiance");
        }

        m_lightingRevision = 0;

        m_radianceValid    = false;
    }

    active = active && m_lightCache != nullptr && m_previousLightCache != nullptr && m_hitLightCachePrevious != nullptr;

    if (active != m_lightCacheActive)
    {
        // The radiance changes meaning with the cache: reinject it, without a delayed frame.
        m_lightingRevision = 0;

        m_radianceValid    = false;
    }

    m_lightCacheActive = active;
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

bool VoxelGIRenderer::UsesSoftwareQueries() const
{
    using Feature = VoxelGISettings::Feature;
    // Indexed by Feature: sky, specular occlusion, sky cache, bounce, reflections, shadows.
    const bool enabled[] = {true, m_settings.specularOcclusion, true, UsesRayBounce(), UsesReflections(), UsesRayShadows()};
    static_assert(std::size(enabled) == static_cast<size_t>(Feature::Count));
    bool software = false;
    for (uint32_t feature = 0; feature < static_cast<uint32_t>(Feature::Count); ++feature)
    {
        software |= enabled[feature] && !UsesHardwareQueries(static_cast<Feature>(feature));
    }
    return software;
}

bool VoxelGIRenderer::RequestsBrickTraversal() const
{
    using Feature = VoxelGISettings::Feature;
    // The GI gather features: sky, specular occlusion, bounce and reflections.
    const bool gather = !UsesHardwareQueries(Feature::Sky)
                     || (m_settings.specularOcclusion && !UsesHardwareQueries(Feature::SpecularOcclusion))
                     || (UsesRayBounce() && !UsesHardwareQueries(Feature::Bounce))
                     || (UsesReflections() && !UsesHardwareQueries(Feature::Reflections));
    return m_settings.computeTraversal == VoxelGISettings::ComputeTraversal::Bricks && gather;
}

bool VoxelGIRenderer::BuildRayQueryGraph()
{
    bool ready = IsInitialized() && m_scene != nullptr;
    if (ready)
    {
        const bool     previousHardware = m_hardwareQueries;
        const uint32_t previousMask     = m_hardwareFeatureMask;
        const char*    previousReason   = m_providerReason;
        const uint32_t requestedMask    = m_settings.GetHardwareFeatureMask();
        // Retain a usable HW path until the requested software snapshot has been built
        // and uploaded. Requested settings remain visible to the UI throughout the switch.
        m_hardwareQueries =
            (requestedMask != 0 || previousHardware)
            && m_rayQuery.BuildRenderGraph(*m_scene, uint64_t(m_settings.accelerationStructureBudgetMB) * 1024 * 1024);
        m_hardwareFeatureMask      = m_hardwareQueries ? requestedMask : 0;
        m_preparingSoftwareQueries = false;
        const bool software        = UsesSoftwareQueries();
        if (software)
        {
            // A BVH that the last frame did not trace (a new scene, or a switch from HW RT)
            // can be far behind the scene, so it is built in the background, on HW RT if
            // available and otherwise with PBR. Frames already tracing it rebuild it on each
            // scene change, keeping moving geometry current in Compute and mixed tracing.
            if (!m_softwareQueriesActive || !m_softwareRayQuery.HoldsSceneGeometry())
            {
                const SoftwareSceneRayQuery::PreparationResult prepared = m_softwareRayQuery.PrepareAsync(*m_scene);
                m_preparingSoftwareQueries = prepared == SoftwareSceneRayQuery::PreparationResult::Pending;
                ready                      = prepared == SoftwareSceneRayQuery::PreparationResult::Ready
                                          || (m_preparingSoftwareQueries && m_hardwareQueries);
                if (m_preparingSoftwareQueries && m_hardwareQueries)
                {
                    m_hardwareFeatureMask = (1u << static_cast<uint32_t>(VoxelGISettings::Feature::Count)) - 1u;
                }
            }
            else
            {
                ready = m_softwareRayQuery.Prepare(*m_scene);
            }
        }
        else
        {
            ready = m_softwareRayQuery.PrepareUnused();
        }
        m_softwareQueriesActive = software && ready && !m_preparingSoftwareQueries;
        if (!m_preparingSoftwareQueries && requestedMask == 0 && m_hardwareQueries)
        {
            m_rayQuery.Destroy();
            m_hardwareQueries     = false;
            m_hardwareFeatureMask = 0;
        }
        m_providerReason = m_preparingSoftwareQueries ? "software_bvh_preparing"
                         : requestedMask != 0         ? m_rayQuery.GetReason()
                                                      : "software_bvh_selected";
        // Bricks are built from the software query's triangles. A failed build falls back to them.
        const bool     requested  = ready && RequestsBrickTraversal();
        const bool     were       = m_bricksActive;
        const uint32_t resolution = m_voxelizer->GetVoxelTexResolution();
        m_bricksActive            = requested
                                 && m_bricks.BuildRenderGraph(*m_scene, m_softwareRayQuery, m_voxelizer->GetSceneMinPoint(),
                                                              m_voxelizer->GetVoxelSize() * static_cast<float>(resolution),
                                                              m_settings.brickResolution);
        // Warn once per fallback streak; BrickScene logs its own allocation failures.
        if (requested && !m_bricksActive && !m_brickFallbackLogged)
        {
            LOGW("Brick scene unavailable; Compute GI rays trace triangles");
        }
        m_brickFallbackLogged = requested && !m_bricksActive;
        if (were != m_bricksActive)
        {
            LOGI("Compute GI traversal: {}", m_bricksActive ? "bricks" : "triangles");
        }
        if (previousMask != m_hardwareFeatureMask)
        {
            m_environmentRevision = 0;
            m_lightingRevision    = 0;
        }
        if (previousHardware != m_hardwareQueries || previousReason != m_providerReason)
        {
            LOGI("Hybrid GI selected {}: {}", m_hardwareQueries ? "hardware" : "software", m_providerReason);
        }
    }
    return ready;
}

void VoxelGIRenderer::BuildRenderGraph(SceneShadowRenderer* shadows)
{
    m_recordedRadiance        = false;
    m_recordedDelayedRadiance = false;
    m_rayTotalsCleared        = false;
    m_rayTotalsRecorded       = false;
    m_countedSkyCache         = false;
    m_countedInjection        = false;
    if (IsInitialized() && m_scene != nullptr)
    {
        m_uniforms.gridMinVoxelSize = Vec4(m_voxelizer->GetSceneMinPoint(), m_voxelizer->GetVoxelSize());

        // Scene bounds include committed instance transforms and vertex deformation,
        // even when the user has fixed a smaller voxel grid with SetVoxelBounds.
        m_visibilityBounds =
            BuildVoxelGIVisibilityBounds(m_scene->GetAABB(), m_uniforms.gridMinVoxelSize, m_voxelizer->GetVoxelTexResolution());

        m_uniforms.volume = Vec4(static_cast<float>(m_voxelizer->GetVoxelTexResolution()), m_voxelizer->GetVoxelScale(),
                                 static_cast<float>(m_radianceMips.size()), m_settings.indirectIntensity);

        m_uniforms.cone   = Vec4(std::tan(glm::radians(m_settings.coneAngleDegrees) * 0.5f), m_settings.stepScale,
                                 m_settings.normalBiasVoxels, m_settings.maxDistanceGridLengths);

        // Mesh light visibility needs triangles; ray bounce and reflections then read the light cache.
        const bool meshShadows = (shadows != nullptr || UsesRayShadows()) && m_scene->GetVoxelTriangleCount() != 0;

        PrepareLightCache(meshShadows && UsesRayBounce());

        m_uniforms.limits     = Vec4(static_cast<float>(m_settings.coneCount), static_cast<float>(m_settings.maxSteps),
                                     m_settings.shadows ? 1.0f : 0.0f, m_lightCacheActive ? 1.0f : 0.0f);

        m_uniforms.lighting   = Vec4(m_settings.analyticLighting ? 1.0f : 0.0f, m_settings.environmentLighting ? 1.0f : 0.0f,
                                     m_settings.emissiveLighting ? 1.0f : 0.0f, UsesRayBounce() ? 1.0f : 0.0f);

        m_uniforms.indirect   = Vec4(m_settings.indirectDiffuse ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

        m_recordedGeometry    = m_voxelizer->GetRecordedGeometryRevision();

        m_recordedLighting    = m_scene->GetLightingRevision();

        m_recordedEnvironment = m_scene->GetEnvironmentRevision();

        const bool geometryChanged    = m_geometryRevision != m_recordedGeometry;

        const bool skyChanged         = geometryChanged || m_environmentRevision != m_recordedEnvironment;

        const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();

        RHISampler* sampler           = m_voxelizer->GetVoxelSampler();

        RenderGraph* graph            = m_device->GetCurrentFrameRDG();

        const glm::uvec3 groups = GetVoxelVolumeDispatchGroups(m_voxelizer->GetVoxelTexResolution(), m_device->GetGPUInfo());

        if (CountsRays())
        {
            graph->AddTransferPass("ClearGIRayTotals")
                .CopyBuffer(m_rayTotalsZeros, m_rayTotals, {0, 0, kRayTotalsBytes})
                .NeverCull();
            m_rayTotalsCleared = true;
        }

        if (geometryChanged)
        {
            BuildMipChain(m_albedoMips, "VoxelFilterAlbedoSP", "VoxelOpacityMip");
        }

        if (m_environmentRevision != m_recordedEnvironment)
        {
            BuildEnvironmentDistribution();
        }

        if (skyChanged)
        {
            RDGComputePassDesc sky;

            const bool  surfaceSky     = m_scene->GetVoxelTriangleCount() != 0;
            const bool  hardwareSky    = UsesHardwareQueries(VoxelGISettings::Feature::SkyCache);
            const char* surfaceProgram = CountsRays()
                                           ? (hardwareSky ? "VoxelSkyIrradianceHardwareCountsSP" : "VoxelSkyIrradianceCountsSP")
                                           : (hardwareSky ? "VoxelSkyIrradianceHardwareSP" : "VoxelSkyIrradianceSP");
            sky.SetShaderProgramName(surfaceSky ? surfaceProgram : "VoxelSkyIrradianceLegacySP");
            m_countedSkyCache = CountsRays();
            if (surfaceSky)
            {
                if (CountsRays())
                {
                    BindRayTotals(sky);
                }
                BindEnvironmentSamplingInputs(sky);
                sky.BindStorageImage("voxelOwner", textures.pOwner->GetDefaultView());
                if (hardwareSky)
                {
                    m_rayQuery.BindInputs(sky, *m_scene);
                }
                else
                {
                    m_softwareRayQuery.BindInputs(sky, *m_scene);
                }
                sky.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());
            }

            sky.SetPassTag("VoxelSkyIrradiance");

            sky.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

            BindFrameData(sky);

            sky.BindSampledTexture("voxelAlbedo", sampler, textures.pAlbedoView);

            sky.BindSampledTexture("voxelNormal", sampler, textures.pNormalView);

            const EnvTexture& env = m_scene->GetEnvTexture();
            sky.BindSampledTexture("coneEnvironmentMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());

            sky.BindStorageImage("skyIrradiance", m_skyIrradiance->GetDefaultView(), RDGContentGuarantee::eFullWrite);

            graph->AddComputePass(std::move(sky)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });
        }

        const bool radianceChanged = skyChanged || m_lightingRevision != m_recordedLighting;

        PrepareRadianceHistory(radianceChanged);

        if (radianceChanged)
        {
            m_recordedRadiance = true;

            RDGComputePassDesc inject;

            const bool hardwareShadows = UsesHardwareShadows();

            inject.SetShaderProgramName(meshShadows
                                            ? (m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectMeshRadianceAveragedSP"
                                                                                      : "VoxelInjectMeshRadianceSP")
                                        : m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectRadianceAveragedSP"
                                                                                 : "VoxelInjectRadianceSP");
            const bool averaged = m_voxelizer->UsesAveragedReflectance();
            m_countedInjection  = CountsRays();
            if (hardwareShadows && meshShadows)
            {
                inject.SetShaderProgramName(
                    CountsRays()
                        ? (averaged ? "VoxelInjectHardwareRadianceAveragedCountsSP" : "VoxelInjectHardwareRadianceCountsSP")
                        : (averaged ? "VoxelInjectHardwareRadianceAveragedSP" : "VoxelInjectHardwareRadianceSP"));
            }

            if (UsesRayShadows() && !hardwareShadows && meshShadows)
            {
                inject.SetShaderProgramName(
                    CountsRays()
                        ? (averaged ? "VoxelInjectSoftwareRadianceAveragedCountsSP" : "VoxelInjectSoftwareRadianceCountsSP")
                        : (averaged ? "VoxelInjectSoftwareRadianceAveragedSP" : "VoxelInjectSoftwareRadianceSP"));
            }

            if (CountsRays() && UsesRayShadows() && meshShadows)
            {
                BindRayTotals(inject);
            }

            inject.SetPassTag("VoxelInjectRadiance");

            inject.SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute);

            BindFrameData(inject);

            inject.BindSampledTexture("voxelAlbedo", sampler, textures.pAlbedoView);

            inject.BindSampledTexture("voxelNormal", sampler, textures.pNormalView);

            inject.BindSampledTexture("voxelEmissive", sampler, textures.pEmissiveView);

            if (meshShadows)
            {
                BindShadowInputs(inject, shadows);

                inject.BindStorageImage("voxelOwner", textures.pOwner->GetDefaultView());

                if (!UsesRayShadows())
                {
                    inject.BindStorageBuffer("VertexBuffer", m_scene->GetVertexBuffer());
                    inject.BindStorageBuffer("IndexBuffer", m_scene->GetIndexBuffer());
                    inject.BindStorageBuffer("NodeBuffer", m_scene->GetNodesDataSSBO());
                }

                inject.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());

                inject.BindStorageImage("voxelClassOwner", textures.pClassOwner->GetDefaultView());
            }

            RHIBuffer* lightCache = m_recordedDelayedRadiance ? m_previousLightCache : m_lightCache;

            inject.BindStorageBuffer("LightVisibilityCache", lightCache,
                                     m_lightCacheActive ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);

            if (m_voxelizer->UsesAveragedReflectance())
            {
                inject.BindSampledTexture("voxelReflectance", sampler, textures.pReflectance->GetDefaultView());
            }

            inject.BindSampledTexture("skyIrradiance", sampler, m_skyIrradiance->GetDefaultView());

            const HeapVector<RHITextureView*>& radianceMips =
                m_recordedDelayedRadiance ? m_previousRadianceMips : m_radianceMips;

            inject.BindStorageImage("voxelRadiance", radianceMips[0], RDGContentGuarantee::eFullWrite);

            graph->AddComputePass(std::move(inject)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });

            if (!m_lightCacheHistoryInitialized)
            {
                // Later injections copy the cache into its history; new history buffers start from this one.
                RHIBuffer* other = lightCache == m_lightCache ? m_previousLightCache : m_lightCache;

                graph->AddTransferPass("InitializeLightCacheHistory")
                    .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                    .CopyBuffer(lightCache, other, {0, 0, m_lightCacheBytes})
                    .CopyBuffer(lightCache, m_hitLightCachePrevious, {0, 0, m_lightCacheSnapshotBytes});

                m_recordedLightCacheHistoryInitialization = true;
            }

            BuildMipChain(radianceMips, "VoxelFilterRadianceSP", "VoxelRadianceMip");
        }
    }
}

void VoxelGIRenderer::BindLightingInputs(RDGPassDescBase& pass) const
{
    BindRayInputs(pass);
    pass.BindValue("uGIVisibilityBounds", m_visibilityBounds);

    pass.BindSampledTexture("voxelRadiance", m_voxelizer->GetVoxelSampler(), m_radiance->GetDefaultView());
}

void VoxelGIRenderer::BindRayInputs(RDGPassDescBase& pass) const
{
    pass.BindValue("uGISettings", m_uniforms);

    pass.BindSampledTexture("voxelOpacity", m_voxelizer->GetVoxelSampler(),
                            m_voxelizer->GetVoxelTextures().pAlbedo->GetDefaultView());

    const EnvTexture& env = m_scene->GetEnvTexture();
    pass.BindSampledTexture("coneEnvironmentMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());
}

void VoxelGIRenderer::BindSoftwareRayInputs(RDGPassDescBase& pass, bool forwardMaterialBound) const
{
    m_softwareRayQuery.BindInputs(pass, *m_scene, forwardMaterialBound);
}

void VoxelGIRenderer::BindHardwareRayInputs(RDGPassDescBase& pass) const
{
    BindRayInputs(pass);
    m_rayQuery.BindInputs(pass, *m_scene);
    m_softwareRayQuery.BindTraversalInputs(pass);
}

void VoxelGIRenderer::BindShadowInputs(RDGPassDescBase& pass, SceneShadowRenderer* shadows, bool forwardMaterialBound) const
{
    if (UsesHardwareShadows())
    {
        m_rayQuery.BindInputs(pass, *m_scene, forwardMaterialBound);
    }
    else if (UsesRayShadows())
    {
        m_softwareRayQuery.BindInputs(pass, *m_scene, forwardMaterialBound);
    }
    else
    {
        VERIFY_EXPR(shadows != nullptr);
        shadows->BindLightingInputs(pass);
    }
}

void VoxelGIRenderer::BindRayTotals(RDGPassDescBase& pass) const
{
    VERIFY_EXPR(m_rayTotals != nullptr);
    pass.BindStorageBuffer("GIRayTotals", m_rayTotals);
}

void VoxelGIRenderer::ReadRayTotals()
{
    // Totals are meaningful only in the frame that zeroed them.
    if (m_rayTotalsCleared && m_rayTotalsReadback != nullptr)
    {
        m_device->GetCurrentFrameRDG()
            ->AddTransferPass("ReadGIRayTotals")
            .CopyBuffer(m_rayTotals, m_rayTotalsReadback, {0, 0, kRayTotalsBytes})
            .NeverCull();
        m_rayTotalsRecorded = true;
    }
}

void VoxelGIRenderer::BindHitRadianceInputs(RDGPassDescBase& pass) const
{
    RHITexture* previous = m_previousRadiance != nullptr ? m_previousRadiance : m_radiance;

    if (m_recordedDelayedRadiance)
    {
        // Unused hit bindings must not introduce a dependency on the chain being written.
        previous = UsesRayBounce() || UsesReflections() ? m_hitRadiancePrevious : m_radiance;
    }

    pass.BindSampledTexture("hitRadiance", m_voxelizer->GetVoxelSampler(), m_radiance->GetDefaultView());
    pass.BindSampledTexture("hitRadiancePrevious", m_voxelizer->GetVoxelSampler(), previous->GetDefaultView());
    pass.BindSampledTexture("hitNormal", m_voxelizer->GetVoxelSampler(),
                            m_voxelizer->GetVoxelTextures().pNormal->GetDefaultView());

    pass.BindStorageBuffer("HitLightCache", m_lightCache);

    pass.BindStorageBuffer("HitLightCachePrevious", m_recordedDelayedRadiance ? m_hitLightCachePrevious : m_previousLightCache);

    const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();

    pass.BindSampledTexture("hitReflectance", m_voxelizer->GetVoxelSampler(),
                            m_voxelizer->UsesAveragedReflectance() ? textures.pReflectance->GetDefaultView()
                                                                   : textures.pAlbedoView);
}

void VoxelGIRenderer::BindEnvironmentSamplingInputs(RDGPassDescBase& pass) const
{
    pass.BindStorageBuffer("EnvironmentColumns", m_environmentColumns);
    pass.BindStorageBuffer("EnvironmentRows", m_environmentRows);
    pass.BindStorageBuffer("EnvironmentLuminance", m_environmentLuminance);
    pass.BindStorageBuffer("EnvironmentTiles", m_environmentTiles);
    pass.BindStorageBuffer("EnvironmentSources", m_environmentSources);
    pass.BindStorageBuffer("EnvironmentResidualColumns", m_environmentResidualColumns);
    pass.BindStorageBuffer("EnvironmentResidualRows", m_environmentResidualRows);
}

void VoxelGIRenderer::BindEnvironmentHarmonics(RDGPassDescBase& pass) const
{
    pass.BindStorageBuffer("EnvironmentHarmonics", m_environmentHarmonics);
}

void VoxelGIRenderer::BuildEnvironmentDistribution()
{
    RenderGraph*      graph = m_device->GetCurrentFrameRDG();
    const EnvTexture& env   = m_scene->GetEnvTexture();

    RDGComputePassDesc columns;
    columns.SetShaderProgramName("EnvironmentColumnsSP");
    columns.SetPassTag("EnvironmentColumns");
    columns.BindSampledTexture("coneEnvironmentMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());
    columns.BindStorageBuffer("EnvironmentColumns", m_environmentColumns, RDGContentGuarantee::eFullWrite);
    columns.BindStorageBuffer("EnvironmentHarmonics", m_environmentHarmonics, RDGContentGuarantee::eFullWrite);
    columns.BindStorageBuffer("EnvironmentLuminance", m_environmentLuminance, RDGContentGuarantee::eFullWrite);
    graph->AddComputePass(std::move(columns)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
        encoder.Dispatch((ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 63u) / 64u, 1, 1);
    });

    RDGComputePassDesc rows;
    rows.SetShaderProgramName("EnvironmentRowsSP");
    rows.SetPassTag("EnvironmentRows");
    rows.BindStorageBuffer("EnvironmentColumns", m_environmentColumns);
    rows.BindStorageBuffer("EnvironmentRows", m_environmentRows, RDGContentGuarantee::eFullWrite);
    rows.BindStorageBuffer("EnvironmentHarmonics", m_environmentHarmonics);
    graph->AddComputePass(std::move(rows)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });

    RDGComputePassDesc tiles;
    tiles.SetShaderProgramName("EnvironmentTilesSP");
    tiles.SetPassTag("EnvironmentTiles");
    tiles.BindStorageBuffer("EnvironmentColumns", m_environmentColumns);
    tiles.BindStorageBuffer("EnvironmentRows", m_environmentRows);
    tiles.BindStorageBuffer("EnvironmentLuminance", m_environmentLuminance);
    tiles.BindStorageBuffer("EnvironmentTiles", m_environmentTiles, RDGContentGuarantee::eFullWrite);
    tiles.BindStorageBuffer("EnvironmentSources", m_environmentSources, RDGContentGuarantee::eFullWrite);
    tiles.BindStorageBuffer("EnvironmentResidualColumns", m_environmentResidualColumns, RDGContentGuarantee::eFullWrite);
    tiles.BindStorageBuffer("EnvironmentResidualRows", m_environmentResidualRows, RDGContentGuarantee::eFullWrite);
    graph->AddComputePass(std::move(tiles)).RecordPassCommands([](RDGPassCmdEncoder& encoder) { encoder.Dispatch(1, 1, 1); });
}

void VoxelGIRenderer::OnRenderGraphExecuted(RenderFrameResult result)
{
    const bool succeeded = result == RenderFrameResult::eSubmitted;

    if (succeeded && m_recordedPreviousRadianceInitialization)
    {
        m_previousRadianceInitialized = true;
    }

    m_recordedPreviousRadianceInitialization   = false;

    m_lightCacheHistoryInitialized            |= succeeded && m_recordedLightCacheHistoryInitialization;

    m_recordedLightCacheHistoryInitialization  = false;

    if (succeeded && m_recordedRadiance)
    {
        if (m_recordedDelayedRadiance)
        {
            std::swap(m_radiance, m_previousRadiance);

            std::swap(m_radianceMips, m_previousRadianceMips);

            std::swap(m_lightCache, m_previousLightCache);
        }

        m_radianceValid = true;

        ++m_radianceGeneration;
        m_geometryRevision    = m_recordedGeometry;
        m_lightingRevision    = m_recordedLighting;
        m_environmentRevision = m_recordedEnvironment;
    }
    else if (result == RenderFrameResult::eFailed || result == RenderFrameResult::eBlocked)
    {
        m_geometryRevision    = 0;
        m_lightingRevision    = 0;
        m_environmentRevision = 0;

        m_radianceValid       = false;
    }
    // Query preparation may execute in a frame which falls back before injection.
    // Publish cache revisions only when their radiance was actually recorded.
    m_recordedRadiance        = false;

    m_recordedDelayedRadiance = false;
    m_rayQuery.OnRenderGraphExecuted(result);
    m_softwareRayQuery.OnRenderGraphExecuted(result);
    m_bricks.OnRenderGraphExecuted(result);
}

void VoxelGIRenderer::Destroy()
{
    m_rayQuery.Destroy();
    m_softwareRayQuery.Destroy();
    m_bricks.Destroy();
    m_bricksActive             = false;
    m_hardwareQueries          = false;
    m_hardwareFeatureMask      = 0;
    m_preparingSoftwareQueries = false;
    m_softwareQueriesActive    = false;
    m_device->DestroyTexture(m_radiance);

    m_device->DestroyTexture(m_previousRadiance);
    m_previousRadiance = nullptr;

    m_previousRadianceMips.clear();

    m_device->DestroyTexture(m_hitRadiancePrevious);

    m_hitRadiancePrevious = nullptr;

    m_device->DestroyBuffer(m_lightCache);

    m_device->DestroyBuffer(m_previousLightCache);

    m_device->DestroyBuffer(m_hitLightCachePrevious);

    m_lightCache                              = nullptr;

    m_previousLightCache                      = nullptr;

    m_hitLightCachePrevious                   = nullptr;

    m_lightCacheBytes                         = 0;

    m_lightCacheSnapshotBytes                 = 0;

    m_lightCacheActive                        = false;

    m_lightCacheHistoryInitialized            = false;

    m_recordedLightCacheHistoryInitialization = false;

    m_previousRadianceInitialized             = false;

    m_recordedPreviousRadianceInitialization  = false;

    m_radianceValid                           = false;

    m_recordedRadiance                        = false;

    m_recordedDelayedRadiance                 = false;

    m_device->DestroyTexture(m_skyIrradiance);

    m_device->DestroyBuffer(m_environmentColumns);
    m_device->DestroyBuffer(m_environmentRows);
    m_device->DestroyBuffer(m_environmentHarmonics);
    m_device->DestroyBuffer(m_environmentLuminance);
    m_device->DestroyBuffer(m_environmentTiles);
    m_device->DestroyBuffer(m_environmentSources);
    m_device->DestroyBuffer(m_environmentResidualColumns);
    m_device->DestroyBuffer(m_environmentResidualRows);
    m_environmentColumns         = nullptr;
    m_environmentRows            = nullptr;
    m_environmentHarmonics       = nullptr;
    m_environmentLuminance       = nullptr;
    m_environmentTiles           = nullptr;
    m_environmentSources         = nullptr;
    m_environmentResidualColumns = nullptr;
    m_environmentResidualRows    = nullptr;

    m_radiance                   = nullptr;

    m_skyIrradiance              = nullptr;

    m_radianceMips.clear();

    // These views belong to the voxelizer's surviving albedo texture. Reuse even a partial
    // list after failed GI initialization instead of accumulating owned views on each retry.
    m_geometryRevision    = 0;

    m_lightingRevision    = 0;

    m_environmentRevision = 0;
}
} // namespace zen::rc
