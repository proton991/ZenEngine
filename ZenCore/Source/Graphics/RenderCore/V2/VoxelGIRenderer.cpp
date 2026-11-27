#include "Math/Math.h"
#include "Utils/Errors.h"
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
#include <cstring>
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

bool VoxelGISettings::UsesRayShadows(bool hardwareReady) const
{
    // Direct shadows trace HW rays when HW RT is selected and ready; Compute uses shadow maps.
    return shadows && rtShadows && hardwareReady && rayProvider != RayProvider::Voxel;
}

uint32_t VoxelGISettings::GetHardwareFeatureMask() const
{
    // Existing shader uniforms use six feature bits. A tier always owns all six.
    return rayProvider == RayProvider::Voxel ? 0u : 63u;
}

VoxelGIRenderer::VoxelGIRenderer(RenderDevice* device, VoxelizerBase* voxelizer, const VoxelGISettings* settings) :
    m_rayQuery(device), m_device(device), m_voxelizer(voxelizer)
{
    if (settings != nullptr)
    {
        m_settings = *settings;
    }
    else
    {
        LoadSettings();
    }
}

bool ValidateVoxelGISettings(const VoxelGISettings& settings)
{
    return std::isfinite(settings.indirectIntensity) && settings.indirectIntensity >= 0 && settings.indirectIntensity <= 10
        && std::isfinite(settings.normalBiasVoxels) && settings.normalBiasVoxels >= 0.5f && settings.normalBiasVoxels <= 4
        && settings.worldCacheTileSize >= 6 && settings.worldCacheTileSize <= 16 && settings.worldCacheHistory >= 1
        && settings.worldCacheHistory <= 128 && settings.worldCacheUnusedFrames >= 1 && settings.worldCacheUnusedFrames <= 4096
        && (settings.samples == 1 || settings.samples == 2 || settings.samples == 4)
        && (settings.referenceSamples == 0 || settings.referenceSamples == 64 || settings.referenceSamples == 256
            || settings.referenceSamples == 1024 || settings.referenceSamples == 4096)
        && (settings.referenceTileStride == 1 || (settings.referenceTileStride == 8 && settings.referenceSamples != 0))
        && settings.probeSamplingPattern <= 2u && settings.samplingPhase <= 63u && settings.restirSpatialSamples >= 1
        && settings.restirSpatialSamples <= 4 && settings.historyFrames >= 1 && settings.historyFrames <= 256
        && static_cast<uint32_t>(settings.reflections) <= static_cast<uint32_t>(VoxelGISettings::Reflections::On)
        && (settings.fieldResolution == 32 || settings.fieldResolution == 64)
        && (settings.pixelResolutionDivisor == 0 || settings.pixelResolutionDivisor == 1 || settings.pixelResolutionDivisor == 2
            || settings.pixelResolutionDivisor == 4)
        && static_cast<uint32_t>(settings.rayProvider) <= static_cast<uint32_t>(VoxelGISettings::RayProvider::Hardware);
}

uint32_t ResolvePixelResolutionDivisor(uint32_t requested, uint32_t outputHeight)
{
    constexpr uint32_t minimumLines = 540;
    uint32_t           divisor      = requested;
    if (divisor == 0)
    {
        divisor = outputHeight >= 4 * minimumLines ? 4u : outputHeight >= 2 * minimumLines ? 2u : 1u;
    }
    return divisor;
}

const char* GetHardwareTierUnavailableReason(const VoxelGISettings& settings, const RHIGPUInfo& gpu)
{
    const char* reason = nullptr;
    if (!gpu.rayQuery.IsUsable())
    {
        reason = "ray_query_unavailable";
    }
    else if (settings.rayTracingPipeline && !SupportsHybridRayTracingPipeline(gpu))
    {
        reason = "rt_pipeline_unavailable";
    }
    return reason;
}

bool IsPersistentHardwareTierFailure(const char* reason)
{
    const char* const persistent[] = {"ray_query_features_disabled_or_unavailable", "acceleration_structure_budget",
                                      "scene_build_preparation_failed",
                                      "hit_shading_stream_uncommitted_or_allocation_or_budget"};
    bool              failed       = false;
    for (const char* candidate : persistent)
    {
        failed = failed || (reason != nullptr && std::strcmp(reason, candidate) == 0);
    }
    return failed;
}

void VoxelGIRenderer::SetAutomaticTier(bool automatic)
{
    if (automatic != m_automaticTier)
    {
        m_tierFallback = nullptr;
    }
    m_automaticTier = automatic;
}

bool VoxelGIRenderer::SetSettings(const VoxelGISettings& settings)
{
    const bool valid = ValidateVoxelGISettings(settings);

    if (valid)
    {
        // A changed tier request retries the HW tier.
        if (settings.rayProvider != m_settings.rayProvider || settings.rayTracingPipeline != m_settings.rayTracingPipeline
            || settings.accelerationStructureBudgetMB != m_settings.accelerationStructureBudgetMB)
        {
            m_tierFallback = nullptr;
        }

        if (settings.normalBiasVoxels != m_settings.normalBiasVoxels
            || settings.environmentLighting != m_settings.environmentLighting || settings.rayProvider != m_settings.rayProvider)
        {
            m_distributionTextureRevision = 0;
            m_environmentRevision         = 0;
        }

        if (settings.shadows != m_settings.shadows || settings.rtShadows != m_settings.rtShadows
            || settings.UsesRayShadows(m_hardwareQueries) != m_settings.UsesRayShadows(m_hardwareQueries)
            || settings.rayProvider != m_settings.rayProvider || settings.analyticLighting != m_settings.analyticLighting
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

    valid                    &= config.ReadNumber("voxel_gi_normal_bias_voxels", settings.normalBiasVoxels);

    valid                    &= config.ReadBool("voxel_gi_shadow_enabled", settings.shadows);
    valid                    &= config.ReadBool("voxel_gi_rt_shadows", settings.rtShadows);

    valid                    &= config.ReadBool("voxel_gi_analytic_lighting", settings.analyticLighting);

    valid                    &= config.ReadBool("voxel_gi_environment_lighting", settings.environmentLighting);

    valid                    &= config.ReadBool("voxel_gi_emissive_lighting", settings.emissiveLighting);

    valid                    &= config.ReadNumber("voxel_gi_samples", settings.samples);
    valid &= config.ReadNumber("voxel_gi_acceleration_structure_budget_mb", settings.accelerationStructureBudgetMB);
    valid &= config.ReadNumber("voxel_gi_reference_samples", settings.referenceSamples);
    valid &= config.ReadNumber("voxel_gi_reference_tile_stride", settings.referenceTileStride);
    valid &= config.ReadNumber("voxel_gi_sampling_phase", settings.samplingPhase);
    valid &= config.ReadNumber("voxel_gi_history_frames", settings.historyFrames);
    valid &= config.ReadBool("voxel_gi_temporal", settings.temporal);
    if (config.GetString("voxel_gi_pixel_resolution_divisor", "") == "auto")
    {
        settings.pixelResolutionDivisor = 0;
    }
    else
    {
        // Automatic is spelled "auto"; a numeric zero is rejected like other invalid divisors.
        valid &= config.ReadNumber("voxel_gi_pixel_resolution_divisor", settings.pixelResolutionDivisor)
              && (!config.HasKey("voxel_gi_pixel_resolution_divisor") || settings.pixelResolutionDivisor != 0);
    }
    valid &= config.ReadBool("voxel_gi_restir", settings.restirGI);
    valid &= config.ReadBool("voxel_gi_restir_spatial", settings.restirSpatial);
    valid &= config.ReadNumber("voxel_gi_restir_spatial_samples", settings.restirSpatialSamples);
    valid &= config.ReadBool("voxel_gi_restir_spatial_visibility", settings.restirSpatialVisibility);
    valid &= config.ReadBool("voxel_gi_restir_specular", settings.restirSpecular);
    valid &= config.ReadBool("voxel_gi_filter", settings.filter);
    valid &= config.ReadBool("voxel_gi_data_driven_anti_lag", settings.dataDrivenAntiLag);
    valid &= config.ReadBool("voxel_gi_young_receiver_samples", settings.youngReceiverSamples);
    valid &= config.ReadBool("voxel_gi_radiance_cache", settings.radianceCache);
    valid &= config.ReadBool("voxel_gi_native_probe_benchmark", settings.nativeProbeBenchmark);
    valid &= config.ReadBool("voxel_gi_world_radiance_cache", settings.worldRadianceCache);
    valid &= config.ReadBool("voxel_gi_world_cache_feedback", settings.worldCacheFeedback);
    const std::string response =
        config.GetString("voxel_gi_history_response", settings.localHistoryResponse ? "local" : "global");
    valid                         &= response == "local" || response == "global";
    settings.localHistoryResponse  = response == "local";
    valid                         &= config.ReadBool("voxel_gi_world_cache_seed", settings.worldCacheSeed);
    valid                         &= config.ReadNumber("voxel_gi_world_cache_tile_size", settings.worldCacheTileSize);
    valid                         &= config.ReadNumber("voxel_gi_world_cache_history", settings.worldCacheHistory);
    valid                         &= config.ReadNumber("voxel_gi_world_cache_unused_frames", settings.worldCacheUnusedFrames);
    valid                         &= config.ReadBool("voxel_gi_screen_probes", settings.screenProbeGI);
    valid                         &= config.ReadBool("voxel_gi_native_shadow_reuse", settings.nativeShadowReuse);
    valid                         &= config.ReadBool("voxel_gi_shadow_reuse", settings.shadowReuse);
    valid                         &= config.ReadBool("voxel_gi_bicubic_history", settings.bicubicHistory);
    valid                         &= config.ReadBool("voxel_gi_anti_firefly", settings.antiFirefly);
    valid                         &= config.ReadBool("voxel_gi_footprint_receivers", settings.footprintReceivers);
    valid                         &= config.ReadBool("voxel_gi_denoiser_validation", settings.denoiserValidation);
    if (!config.GetString("voxel_gi_relax_denoiser", "").empty())
    {
        LOGI("voxel_gi_relax_denoiser ignored; the GI denoiser no longer has an alternative");
    }
    valid                          &= config.ReadBool("voxel_gi_ray_tracing_pipeline", settings.rayTracingPipeline);
    valid                          &= config.ReadBool("voxel_gi_indirect_diffuse", settings.indirectDiffuse);
    valid                          &= config.ReadBool("voxel_gi_specular_occlusion", settings.specularOcclusion);
    const char* defaultReflections  = settings.reflections == VoxelGISettings::Reflections::Off  ? "false"
                                    : settings.reflections == VoxelGISettings::Reflections::Auto ? "auto"
                                                                                                 : "true";
    // Compute GI always traces the distance field. Older configs named the software BVH or occupancy
    // bricks; both load as the field, and an occupancy resolution does not translate into a field one.
    const std::string traversal  = config.GetString("voxel_gi_compute_traversal", "field");
    valid                       &= traversal == "triangles" || traversal == "bricks" || traversal == "field";
    if (traversal != "field")
    {
        LOGI("voxel_gi_compute_traversal={} migrated to the distance field", traversal);
    }
    if (!config.GetString("voxel_gi_brick_resolution", "").empty())
    {
        LOGI("voxel_gi_brick_resolution ignored; voxel_gi_field_resolution sets the distance field");
    }
    valid                             &= config.ReadNumber("voxel_gi_field_resolution", settings.fieldResolution);
    const std::string reflections      = config.GetString("voxel_gi_reflections", defaultReflections);
    valid                             &= reflections == "auto" || reflections == "true" || reflections == "false";
    settings.reflections               = reflections == "true"  ? VoxelGISettings::Reflections::On
                                       : reflections == "false" ? VoxelGISettings::Reflections::Off
                                                                : VoxelGISettings::Reflections::Auto;
    const char*       defaultProvider  = settings.rayProvider == VoxelGISettings::RayProvider::Voxel    ? "voxel"
                                       : settings.rayProvider == VoxelGISettings::RayProvider::Hardware ? "hardware"
                                                                                                        : "auto";
    const std::string provider         = config.GetString("voxel_gi_ray_provider", defaultProvider);
    valid                             &= provider == "auto" || provider == "voxel" || provider == "hardware";
    settings.rayProvider               = provider == "hardware" ? VoxelGISettings::RayProvider::Hardware
                                       : provider == "voxel"    ? VoxelGISettings::RayProvider::Voxel
                                                                : VoxelGISettings::RayProvider::Auto;
    const std::string backend          = config.GetString("voxel_gi_backend", "");
    valid                             &= backend.empty() || backend == "auto" || backend == "compute" || backend == "hardware";
    if (!backend.empty())
    {
        settings.rayProvider = backend == "auto"    ? VoxelGISettings::RayProvider::Auto
                             : backend == "compute" ? VoxelGISettings::RayProvider::Voxel
                                                    : VoxelGISettings::RayProvider::Hardware;
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
                texture, format, NameID(fmt::format("{}_mip_{}", texture->GetBaseInfo().tag.c_str(), mip)));

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

        if (m_skyIrradiance != nullptr && m_unshadowedMap == nullptr)
        {
            // Field gathers without scene shadow maps read zeroed shadow data: every light unshadowed.
            TextureFormat unshadowed;
            unshadowed.format      = DataFormat::eR32SFloat;
            unshadowed.dimension   = TextureDimension::e2D;
            unshadowed.width       = 1;
            unshadowed.height      = 1;
            unshadowed.depth       = 1;
            unshadowed.arrayLayers = 1;
            m_unshadowedMap        = m_device->CreateTextureSampled(unshadowed, {}, "voxel_unshadowed_map");
            if (m_unshadowedMap != nullptr)
            {
                TextureViewFormat view;
                view.dimension      = TextureDimension::e2D;
                view.arrayView      = true;
                m_unshadowedMapView = m_device->CreateTextureView(m_unshadowedMap, view, "voxel_unshadowed_array");
            }
            m_unshadowedSampler = m_device->CreateSampler({});
        }

        if (m_skyIrradiance != nullptr)
        {
            format.width = format.height = format.depth = format.mipmaps = 1;
            m_emptyLighting      = m_device->CreateTextureStorage(format, {.copyUsage = true}, "unused_gi_lighting");
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
            // Count, tile indices, residual CDF and three scalar RGB energy components per source.
            m_environmentSources = m_device->CreateStorageBuffer((2u + 4u * ZEN_ENVIRONMENT_MAX_SOURCES + ZEN_ENVIRONMENT_TILES)
                                                                     * sizeof(uint32_t),
                                                                 nullptr, "environment_sources");
            m_environmentResidualColumns = m_device->CreateStorageBuffer(
                ZEN_ENVIRONMENT_IMPORTANCE_ROWS * (ZEN_ENVIRONMENT_IMPORTANCE_SIZE + 1u) * sizeof(float), nullptr,
                "environment_residual_columns");
            m_environmentResidualRows = m_device->CreateStorageBuffer((ZEN_ENVIRONMENT_IMPORTANCE_ROWS + 1u) * sizeof(float),
                                                                      nullptr, "environment_residual_rows");
        }

        valid = IsInitialized() && m_emptyLighting != nullptr && PrepareMipViews(m_radiance, m_radianceMips)
             && PrepareMipViews(m_voxelizer->GetVoxelTextures().pAlbedo, m_albedoMips) && m_voxelizer->EnableRadianceInputs();

        if (valid)
        {
            // Hit-radiance bindings need the light-visibility caches before a frame graph sizes them.
            PrepareLightCache(false);

            valid = m_lightCache != nullptr && m_previousLightCache != nullptr;
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
    m_nativeDynamicGeometry.Destroy(*m_device);
    m_nativeDynamicRevision = 0;
    m_nativeDynamicCount    = 0;
    // Another scene may build on the HW tier.
    m_requestedHardware = false;
    m_tierFallback      = nullptr;
    if (m_field)
    {
        m_field->Destroy();
    }
    ++m_fieldSerial;
    m_fieldActive                 = false;
    m_preparingField              = false;
    m_skyVisibility               = 0;
    m_hardwareQueries             = false;
    m_hardwareFeatureMask         = 0;
    m_scene                       = scene;

    m_geometryRevision            = 0;

    m_lightingRevision            = 0;

    m_distributionTextureRevision = 0;
    m_environmentRevision         = 0;

    m_radianceValid               = false;
}

void VoxelGIRenderer::PrepareRadianceHistory(bool update)
{
    const uint32_t resolution = m_voxelizer->GetVoxelTexResolution();
    if (m_previousRadiance != nullptr && m_previousRadiance->GetWidth() != resolution)
    {
        m_device->DestroyTexture(m_previousRadiance);
        m_previousRadiance = nullptr;
    }
    if (m_previousRadiance == nullptr)
    {
        TextureFormat format;
        format.dimension = TextureDimension::e3D;
        format.format    = DataFormat::eR16G16B16A16SFloat;
        format.width = format.height = format.depth = resolution;
        format.mipmaps                              = 1;
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
    // The current denoiser samples the same rays against the cache before and after injection.
    if (update && m_previousRadiance != nullptr && m_radianceValid)
    {
        RHITextureCopyRegion region{};
        region.size = RHIOffset3D(static_cast<int>(resolution));
        region.srcSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        region.srcSubresources.layerCount = 1;
        region.dstSubresources            = region.srcSubresources;
        graph->AddTransferPass("VoxelKeepPreviousRadiance")
            .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
            .CopyTexture(m_radiance, m_previousRadiance, MakeVecView(&region, 1));
        if (m_lightCacheActive)
        {
            graph->AddTransferPass("VoxelKeepPreviousLightCache")
                .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                .CopyBuffer(m_lightCache, m_previousLightCache, {0, 0, m_lightCacheBytes});
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

    if (m_lightCache == nullptr || m_previousLightCache == nullptr || bytes != m_lightCacheBytes)
    {
        m_device->DestroyBuffer(m_lightCache);

        m_device->DestroyBuffer(m_previousLightCache);


        m_lightCache         = CreateLightCache(m_device, bytes, "voxel_light_cache");

        m_previousLightCache = CreateLightCache(m_device, bytes, "voxel_previous_light_cache");


        // A zero light count makes a lookup of a never-injected cache dark instead of undefined.
        const HeapVector<uint8_t> zeros(kLightCacheHeaderBytes, uint8_t{0});

        for (RHIBuffer* cache : {m_lightCache, m_previousLightCache})
        {
            if (cache != nullptr)
            {
                m_device->UpdateBuffer(cache, static_cast<uint32_t>(kLightCacheHeaderBytes), zeros.data());
            }
        }

        m_lightCacheBytes              = bytes;


        m_lightCacheHistoryInitialized = false;

        if (requested && !active)
        {
            LOGW("Voxel light-visibility cache does not fit a storage buffer; hits use the isotropic radiance");
        }

        m_lightingRevision = 0;

        m_radianceValid    = false;
    }

    active = active && m_lightCache != nullptr && m_previousLightCache != nullptr;

    if (active != m_lightCacheActive)
    {
        // The radiance changes meaning with the cache: reinject it immediately.
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

bool VoxelGIRenderer::UsesComputeQueries() const
{
    return m_fieldActive;
}

bool VoxelGIRenderer::IsRefiningField() const
{
    return m_fieldActive && m_field && m_scene != nullptr && (m_field->LocalWorkRemains() || !m_field->IsCurrent(*m_scene));
}

HeapVector<AABB> VoxelGIRenderer::BuildLocalFieldVolumes() const
{
    // Lamps sit in fixtures whose centimetre openings the scene field closes, trapping their light.
    // A finer volume around each point and spot light lets bounce rays leave and enter the fixture
    // and resolves the brightly lit surfaces around it. Three voxels each way passed the New Sponza
    // lamp gates; one and two voxels left 32% and 24% of the floor bounce missing
    // (Doc/ComputeGI/BrixelizerGIExecution.md, milestone 6).
    HeapVector<AABB> volumes;
    const Vec3       extent(3.0f * m_voxelizer->GetVoxelSize());
    for (const LightEntry& entry : m_scene->GetLights().GetEntries())
    {
        const SceneLight& light = entry.light;
        if (light.enabled && light.type != SceneLightType::eDirectional && light.intensity > 0.0f
            && volumes.size() < ZEN_FIELD_LOCAL_VOLUMES)
        {
            volumes.push_back(AABB(light.position - extent, light.position + extent));
        }
    }
    return volumes;
}

DistanceFieldSceneSettings VoxelGIRenderer::BuildFieldSettings() const
{
    // Construction shares the GPU with interactive rendering. Bound both temporary memory and
    // graph size; the adaptive triangle quota targets a 16 ms frame instead of throughput stalls.
    // When rendering alone exceeds that (Compute GI at 4K), construction may instead lengthen the
    // measured render-only frame by a quarter, so local volumes still publish.
    // Capacity changes only batching, never the final field resolution or coverage quality.
    DistanceFieldSceneSettings settings;
    settings.cellsPerAxis                      = m_settings.fieldResolution;
    settings.atlasTexelsPerAxis                = settings.cellsPerAxis * 8;
    settings.triangleBatchSize                 = 65536;
    settings.maxCyclesPerFrame                 = 4;
    settings.throughputFrameBudgetMicroseconds = 16000.0;
    settings.throughputFrameBudgetShare        = 0.25;
    settings.referencesPerCell                 = ZEN_FIELD_MAX_CELL_REFS;
    settings.deriveCapacities                  = true;
    // Two full 64^3 fields plus all 32 local slots and one temporary local build need about
    // 398 MiB. Keep the published field available during refresh; this cap does not preallocate.
    settings.persistentBudgetBytes = 512ull * 1024 * 1024;
    settings.scratchBudgetBytes    = 1024ull * 1024 * 1024;
    return settings;
}

bool VoxelGIRenderer::BuildRayQueryGraph()
{
    bool ready = IsInitialized() && m_scene != nullptr;
    if (ready)
    {
        const bool     previousHardware = m_hardwareQueries;
        const uint32_t previousMask     = m_hardwareFeatureMask;
        const char*    previousReason   = m_providerReason;
        const bool     previousField    = m_fieldActive;
        uint32_t       requestedMask    = m_settings.GetHardwareFeatureMask();
        // Auto resolves the complete tier by device capability: RT pipeline (or ray-query) HW tier, then
        // Compute, then PBR. An automatic HW tier that persistently fails to build this scene also
        // selects Compute; an explicit HW tier stays unavailable and the frame falls back to PBR.
        const bool automatic = m_settings.rayProvider == VoxelGISettings::RayProvider::Auto || m_automaticTier;
        if (requestedMask != 0 && automatic
            && (m_tierFallback != nullptr || GetHardwareTierUnavailableReason(m_settings, m_device->GetGPUInfo()) != nullptr))
        {
            requestedMask = 0;
        }
        // Only the selected HW tier may build or use acceleration structures.
        // Compute preparation never borrows the previously selected HW backend, including a
        // pending or failed build from the frame that requested it.
        if (requestedMask == 0 && (previousHardware || m_requestedHardware))
        {
            m_rayQuery.Destroy();
        }
        m_requestedHardware = requestedMask != 0;
        m_hardwareQueries =
            requestedMask != 0
            && m_rayQuery.BuildRenderGraph(*m_scene, uint64_t(m_settings.accelerationStructureBudgetMB) * 1024 * 1024);
        if (requestedMask != 0 && !m_hardwareQueries && automatic && IsPersistentHardwareTierFailure(m_rayQuery.GetReason()))
        {
            // Released next frame, after this frame's graph no longer records the failed build.
            m_tierFallback = m_rayQuery.GetReason();
            requestedMask  = 0;
            LOGW("Hybrid GI hardware tier unavailable ({}); using the Compute tier", m_tierFallback);
        }
        m_hardwareFeatureMask             = m_hardwareQueries ? requestedMask : 0;
        const bool                compute = requestedMask == 0;
        DistanceFieldScene::State state   = DistanceFieldScene::State::Pending;
        const bool                nativeGeometryReady =
            !compute || (!m_settings.screenProbeGI && !m_settings.worldRadianceCache) || PrepareNativeDynamicGeometry();
        if (compute && nativeGeometryReady)
        {
            if (!m_field || m_fieldResolution != m_settings.fieldResolution)
            {
                if (m_field)
                {
                    m_field->Destroy();
                }
                m_field           = MakeUnique<DistanceFieldScene>(m_device, BuildFieldSettings());
                m_fieldResolution = m_settings.fieldResolution;
                ++m_fieldSerial;
            }
            m_field->SetLocalVolumes(BuildLocalFieldVolumes());
            state           = m_field->BuildRenderGraph(*m_scene);
            m_fieldRecorded = true;
        }
        else if (m_field)
        {
            // An unfinished build would otherwise hold its construction scratch while unused.
            m_field->Suspend();
        }
        // Hardware-only frames do not build an unused field. Retain an existing published field
        // for a warm switch; the Compute path refreshes it against committed edits when selected.
        const bool hardwareReady = requestedMask == 0 || m_hardwareQueries;
        m_fieldActive            = compute && state == DistanceFieldScene::State::Ready;
        // Cold Compute preparation displays PBR until a field is published.
        m_preparingField = hardwareReady && compute && nativeGeometryReady && state == DistanceFieldScene::State::Pending;
        // A failed backend is unavailable, not permission to trace with the other backend.
        ready             = hardwareReady && nativeGeometryReady && (!compute || m_fieldActive);
        const bool failed = compute && state == DistanceFieldScene::State::Failed;
        m_providerReason  = m_preparingField     ? "field_preparing"
                          : failed               ? "field_failed"
                          : !nativeGeometryReady ? "native_dynamic_geometry_unavailable"
                          : requestedMask != 0   ? m_rayQuery.GetReason()
                                                 : "field_selected";
        if (failed && previousReason != m_providerReason)
        {
            LOGW("Distance field unavailable for Compute GI: {}", m_field->GetFailureReason());
        }
        if (previousField != m_fieldActive)
        {
            LOGI("Compute GI distance field {}", m_fieldActive ? "active" : "inactive");
        }
        if (previousMask != m_hardwareFeatureMask)
        {
            m_distributionTextureRevision = 0;
            m_environmentRevision         = 0;
            m_lightingRevision            = 0;
        }
        if (previousHardware != m_hardwareQueries || previousReason != m_providerReason)
        {
            LOGI("Hybrid GI selected {}: {}", m_hardwareQueries ? "hardware" : "compute", m_providerReason);
        }
    }
    return ready;
}

void VoxelGIRenderer::BuildRenderGraph(SceneShadowRenderer* shadows)
{
    m_shadowMaps        = shadows;
    m_recordedRadiance  = false;
    m_rayTotalsCleared  = false;
    m_rayTotalsRecorded = false;
    m_countedSkyCache   = false;
    m_countedInjection  = false;
    if (IsInitialized() && m_scene != nullptr)
    {
        m_uniforms.gridMinVoxelSize = Vec4(m_voxelizer->GetSceneMinPoint(), m_voxelizer->GetVoxelSize());

        m_uniforms.volume = Vec4(static_cast<float>(m_voxelizer->GetVoxelTexResolution()), m_voxelizer->GetVoxelScale(),
                                 static_cast<float>(m_radianceMips.size()), m_settings.indirectIntensity);

        // Only the normal-bias lane is used; the remaining lanes preserve the uniform layout.
        m_uniforms.cone          = Vec4(0.0f, 0.0f, m_settings.normalBiasVoxels, 0.0f);

        const bool voxelLighting = UsesVoxelLighting();
        // Only Compute consumers need a secondary lighting cache.
        const bool meshShadows = (shadows != nullptr || UsesRayShadows()) && m_scene->GetVoxelTriangleCount() != 0;

        PrepareLightCache(voxelLighting && meshShadows);

        m_uniforms.limits      = Vec4(0.0f, 0.0f, m_settings.shadows ? 1.0f : 0.0f, m_lightCacheActive ? 1.0f : 0.0f);

        m_uniforms.lighting    = Vec4(m_settings.analyticLighting ? 1.0f : 0.0f, m_settings.environmentLighting ? 1.0f : 0.0f,
                                      m_settings.emissiveLighting ? 1.0f : 0.0f, 1.0f);

        uint32_t lightingMask  = m_hardwareFeatureMask;
        lightingMask          &= m_settings.specularOcclusion ? ~0u : ~18u;
        lightingMask          &= UsesReflections() ? ~0u : ~16u;
        m_uniforms.indirect    = Vec4(m_settings.indirectDiffuse ? 1.0f : 0.0f, static_cast<float>(lightingMask),
                                      static_cast<float>(m_settings.samples), std::bit_cast<float>(m_frame));

        m_recordedGeometry     = m_voxelizer->GetRecordedGeometryRevision();

        m_recordedLighting     = m_scene->GetLightingRevision();

        m_recordedEnvironment  = m_scene->GetEnvironmentRevision();

        const bool geometryChanged = m_geometryRevision != m_recordedGeometry;

        // The Compute sky cache follows the published field generation.
        m_recordedSkyVisibility = GetVisibilityGeneration();

        const bool skyChanged =
            geometryChanged || m_environmentRevision != m_recordedEnvironment || m_skyVisibility != m_recordedSkyVisibility;

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

        if (UsesHardwareShadows() || (UsesRayTracingPipeline() && (m_settings.screenProbeGI || m_settings.worldRadianceCache)))
        {
            BuildLightOccluders();
        }

        if (!voxelLighting && !m_emptyLightingInitialized)
        {
            graph->AddTransferPass("InitializeUnusedGIInputs").ClearTexture(m_emptyLighting, Color(0));
            m_recordedEmptyLighting = true;
        }

        if (voxelLighting && geometryChanged)
        {
            BuildMipChain(m_albedoMips, "VoxelFilterAlbedoSP", "VoxelOpacityMip");
        }

        m_recordedTextureRevision = m_scene->GetEnvironmentTextureRevision();
        if (m_distributionTextureRevision != m_recordedTextureRevision)
        {
            BuildEnvironmentDistribution();
            m_recordedEnvironmentDistribution = true;
        }

        if (voxelLighting && skyChanged && m_scene->GetVoxelTriangleCount() != 0 && m_fieldActive)
        {
            RDGComputePassDesc sky;

            sky.SetShaderProgramName(CountsRays() ? "VoxelSkyIrradianceFieldCountsSP" : "VoxelSkyIrradianceFieldSP");
            m_countedSkyCache = CountsRays();

            if (CountsRays())
            {
                BindRayTotals(sky);
            }
            BindEnvironmentSamplingInputs(sky);
            sky.BindStorageImage("voxelOwner", textures.pOwner->GetDefaultView());
            // Owner receivers read scene geometry directly; the field holds no triangles.
            m_field->BindCompositeInputs(sky);
            sky.BindStorageBuffer("VertexBuffer", m_scene->GetVertexBuffer());
            sky.BindStorageBuffer("IndexBuffer", m_scene->GetIndexBuffer());
            sky.BindStorageBuffer("NodeBuffer", m_scene->GetNodesDataSSBO());
            sky.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());


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
        else if (voxelLighting && skyChanged)
        {
            // Empty scenes have no cache receivers. Define the output without tracing a substitute.
            graph->AddTransferPass("VoxelSkyIrradiance")
                .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                .ClearTexture(m_skyIrradiance, Color(0));
        }

        const bool radianceChanged = skyChanged || m_lightingRevision != m_recordedLighting;

        if (voxelLighting)
        {
            PrepareRadianceHistory(radianceChanged);
        }

        if (voxelLighting && radianceChanged)
        {
            m_recordedRadiance = true;

            RDGComputePassDesc inject;

            inject.SetShaderProgramName(meshShadows
                                            ? (m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectMeshRadianceAveragedSP"
                                                                                      : "VoxelInjectMeshRadianceSP")
                                        : m_voxelizer->UsesAveragedReflectance() ? "VoxelInjectRadianceAveragedSP"
                                                                                 : "VoxelInjectRadianceSP");
            m_countedInjection = CountsRays();

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

                inject.BindStorageBuffer("VertexBuffer", m_scene->GetVertexBuffer());
                inject.BindStorageBuffer("IndexBuffer", m_scene->GetIndexBuffer());
                inject.BindStorageBuffer("NodeBuffer", m_scene->GetNodesDataSSBO());

                inject.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());

                inject.BindStorageImage("voxelClassOwner", textures.pClassOwner->GetDefaultView());
            }



            inject.BindStorageBuffer("LightVisibilityCache", m_lightCache,
                                     m_lightCacheActive ? RDGContentGuarantee::eFullWrite : RDGContentGuarantee::eNone);

            if (m_voxelizer->UsesAveragedReflectance())
            {
                inject.BindSampledTexture("voxelReflectance", sampler, textures.pReflectance->GetDefaultView());
            }

            inject.BindSampledTexture("skyIrradiance", sampler, m_skyIrradiance->GetDefaultView());

            inject.BindStorageImage("voxelRadiance", m_radianceMips[0], RDGContentGuarantee::eFullWrite);

            graph->AddComputePass(std::move(inject)).RecordPassCommands([groups](RDGPassCmdEncoder& encoder) {
                encoder.Dispatch(groups.x, groups.y, groups.z);
            });

            if (!m_lightCacheHistoryInitialized)
            {
                // Later injections copy the cache into its history; new history buffers start from this one.


                graph->AddTransferPass("InitializeLightCacheHistory")
                    .SetQueuePreference(RDGQueuePreference::ePreferAsyncCompute)
                    .CopyBuffer(m_lightCache, m_previousLightCache, {0, 0, m_lightCacheBytes});

                m_recordedLightCacheHistoryInitialization = true;
            }

            BuildMipChain(m_radianceMips, "VoxelFilterRadianceSP", "VoxelRadianceMip");
        }
    }
}

void VoxelGIRenderer::BindLightingInputs(RDGPassDescBase& pass, bool environment) const
{
    pass.BindValue("uGISettings", m_uniforms);
    if (environment)
    {
        const EnvTexture& env = m_scene->GetEnvTexture();
        pass.BindSampledTexture("coneEnvironmentMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());
    }
}

void VoxelGIRenderer::BindSecondaryBRDF(RDGPassDescBase& pass) const
{
    const EnvTexture& env = m_scene->GetEnvTexture();
    pass.BindSampledTexture("secondaryBRDFMap", env.pLutBRDFSampler, env.pLutBRDF->GetDefaultView());
}

bool VoxelGIRenderer::UsesRayTracingPipeline() const
{
    return m_hardwareQueries && m_settings.rayTracingPipeline && SupportsHybridRayTracingPipeline(m_device->GetGPUInfo());
}

void VoxelGIRenderer::BindRayInputs(RDGPassDescBase& pass) const
{
    BindLightingInputs(pass, true);
    pass.BindSampledTexture(
        "voxelOpacity", m_voxelizer->GetVoxelSampler(),
        (UsesVoxelLighting() ? m_voxelizer->GetVoxelTextures().pAlbedo : m_emptyLighting)->GetDefaultView());
}

void VoxelGIRenderer::BindHardwareRayInputs(RDGPassDescBase& pass, bool environment, bool occluders) const
{
    if (environment)
    {
        BindRayInputs(pass);
    }
    else
    {
        BindLightingInputs(pass);
    }
    m_rayQuery.BindInputs(pass, *m_scene);
    if (occluders)
    {
        pass.BindStorageBuffer("LightOccluders", m_lightOccluders);
    }
}

void VoxelGIRenderer::BindFieldInputs(RDGPassDescBase& pass) const
{
    // Gather passes read near-light visibility from the frame's shadow maps (hit_radiance.glsl). A frame
    // without them, which the renderer never composes with field GI, sees every light unshadowed.
    VERIFY_EXPR(m_fieldActive);
    m_field->BindCompositeInputs(pass);
    if (m_shadowMaps != nullptr)
    {
        m_shadowMaps->BindLightingInputs(pass);
    }
    else
    {
        static const SceneShadowUniformData unshadowed{};
        pass.BindValue("uSceneShadows", unshadowed);
        pass.BindSampledTexture("sceneShadowMaps", m_unshadowedSampler, m_unshadowedMapView);
        pass.BindSampledTexture("sceneShadowMaxDepth", m_unshadowedSampler, m_unshadowedMapView);
    }
}

void VoxelGIRenderer::BindNativeFieldInputs(RDGPassDescBase& pass, bool environment) const
{
    VERIFY_EXPR(m_fieldActive && UsesVoxelLighting());
    m_field->BindCompositeInputs(pass);
    // Native Compute traces committed dynamic triangles while the static scene and
    // light-detail regions use the distance field. A continuously moving mesh must
    // not make every ray from its still-building field region unavailable.
    DistanceFieldLocalsUniform locals = m_field->GetLocalsUniform();
    for (uint32_t index = 0; index < locals.header.x; ++index)
    {
        if (locals.volumes[index].slot.w != 0u)
        {
            locals.volumes[index].slot.z = 0u;
        }
    }
    pass.BindValue("uDistanceFieldLocals", locals);
    BindSceneRayGeometry(pass, *m_device, *m_scene, false);
    pass.BindStorageBuffer("TriangleRecords", m_scene->GetVoxelTriangleBuffer());
    pass.BindStorageBuffer("NativeDynamicGeometry", m_nativeDynamicGeometry.GetBuffer());
    BindLightingInputs(pass, environment);
    const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();
    pass.BindSampledTexture("nativeReflectance", m_voxelizer->GetVoxelSampler(),
                            m_voxelizer->UsesAveragedReflectance() ? textures.pReflectance->GetDefaultView()
                                                                   : textures.pAlbedoView);
    pass.BindSampledTexture("nativeNormal", m_voxelizer->GetVoxelSampler(), textures.pNormal->GetDefaultView());
    pass.BindSampledTexture("nativeEmission", m_voxelizer->GetVoxelSampler(), textures.pEmissiveView);
    pass.BindValue("uNativeFieldData",
                   glm::uvec4(m_voxelizer->UsesAveragedReflectance() ? 1u : 0u, m_nativeDynamicCount, 0u, 0u));
}

bool VoxelGIRenderer::PrepareNativeDynamicGeometry()
{
    bool           ready    = true;
    const uint64_t revision = m_scene->GetGeometryRevision();
    if (revision != m_nativeDynamicRevision || m_nativeDynamicGeometry.GetBuffer() == nullptr)
    {
        struct Record
        {
            Vec4       minimum;
            Vec4       maximum;
            glm::uvec4 triangles;
        };
        static_assert(sizeof(Record) == 48);
        HeapVector<Record>                                    records;
        const HeapVector<RenderScene::InstanceTriangleRange>& ranges  = m_scene->GetInstanceTriangleRanges();
        const HeapVector<RenderScene::ShadowCaster>&          casters = m_scene->GetShadowCasters();
        ready                                                         = ranges.size() == casters.size();
        if (!ready)
        {
            LOGE("Native dynamic geometry has {} instance ranges and {} shadow casters", ranges.size(), casters.size());
        }
        for (size_t index = 0; ready && index < ranges.size(); ++index)
        {
            const RenderScene::InstanceTriangleRange& range = ranges[index];
            if ((range.committedMask & GI_DYNAMIC) != 0u && range.triangleCount != 0u)
            {
                const Vec3 minimum = casters[index].bounds.GetMin();
                const Vec3 maximum = casters[index].bounds.GetMax();
                const Vec3 padding = glm::max(glm::max(glm::abs(minimum), glm::abs(maximum)), Vec3(1)) * 1e-6f;
                records.push_back({Vec4(minimum - padding, 0), Vec4(maximum + padding, 0),
                                   glm::uvec4(range.firstTriangle, range.triangleCount, 0, 0)});
            }
        }
        const uint32_t count = static_cast<uint32_t>(records.size());
        if (records.empty())
        {
            records.push_back(Record{});
        }
        const uint64_t bytes = uint64_t(records.size()) * sizeof(Record);
        ready                = ready && bytes <= UINT32_MAX && bytes <= m_device->GetGPUInfo().maxStorageBufferRange;
        if (ready)
        {
            m_nativeDynamicGeometry.MarkDirty(0, static_cast<uint32_t>(bytes));
            ready = m_nativeDynamicGeometry.Update(*m_device, records.data(), static_cast<uint32_t>(bytes),
                                                   "NativeDynamicGeometry");
        }
        if (ready)
        {
            m_nativeDynamicRevision = revision;
            m_nativeDynamicCount    = count;
        }
        else
        {
            LOGE("Unable to upload native Compute dynamic geometry");
        }
    }
    return ready;
}

void VoxelGIRenderer::BuildLightOccluders()
{
    RenderGraph*  graph = m_device->GetCurrentFrameRDG();
    RDGBufferDesc desc;
    desc.name = "light_occluder_triangles";
    desc.size = uint64_t(MaxSceneLights) * 6u * 16u * 16u * 3u * sizeof(Vec4);
    desc.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
    m_lightOccluders               = graph->GetResourceManager()->CreateBuffer(desc);
    const bool            pipeline = UsesRayTracingPipeline();
    RDGComputePassDesc    compute;
    RDGRayTracingPassDesc rays;
    RDGDispatchPassDesc& pass = pipeline ? static_cast<RDGDispatchPassDesc&>(rays) : static_cast<RDGDispatchPassDesc&>(compute);
    pass.SetPassTag("LightOccluders");
    pass.SetShaderProgramName(pipeline ? (CountsRays() ? "LightOccludersPipelineCountsSP" : "LightOccludersPipelineSP")
                                       : (CountsRays() ? "LightOccludersCountsSP" : "LightOccludersSP"));
    if (CountsRays())
    {
        BindRayTotals(pass);
    }
    pass.BindValue("uGISettings", m_uniforms);
    pass.BindValue("uSceneData", m_scene->GetSceneUniformData(), sizeof(SceneUniformData));
    m_rayQuery.BindInputs(pass, *m_scene);
    pass.BindStorageBuffer("LightOccluders", m_lightOccluders, RDGContentGuarantee::eFullWrite);
    // Rebuild from this frame's published geometry and lights. No stale cache survives scene edits or declined frames.
    if (pipeline)
    {
        rays.SetShaderGroups({{RHIRayTracingShaderGroupType::eRayGeneration, 0},
                              {RHIRayTracingShaderGroupType::eMiss, 1},
                              {RHIRayTracingShaderGroupType::eHit, kUnusedRayTracingShader, 2, 3}});
        graph->AddRayTracingPass(std::move(rays)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.TraceRays(16, 16, MaxSceneLights * 6);
        });
    }
    else
    {
        graph->AddComputePass(std::move(compute)).RecordPassCommands([](RDGPassCmdEncoder& encoder) {
            encoder.Dispatch(2, 2, MaxSceneLights * 6);
        });
    }
}

void VoxelGIRenderer::BindShadowInputs(RDGPassDescBase& pass, SceneShadowRenderer* shadows, bool forwardMaterialBound) const
{
    if (UsesHardwareShadows())
    {
        m_rayQuery.BindInputs(pass, *m_scene, forwardMaterialBound);
        pass.BindStorageBuffer("LightOccluders", m_lightOccluders);
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
    // Only the Compute tier reads cached hit lighting.
    if (!m_hardwareQueries)
    {
        RHITexture* previous = m_previousRadiance != nullptr ? m_previousRadiance : m_radiance;

        pass.BindSampledTexture("hitRadiance", m_voxelizer->GetVoxelSampler(), m_radiance->GetDefaultView());
        pass.BindSampledTexture("hitRadiancePrevious", m_voxelizer->GetVoxelSampler(), previous->GetDefaultView());
        pass.BindSampledTexture("hitNormal", m_voxelizer->GetVoxelSampler(),
                                m_voxelizer->GetVoxelTextures().pNormal->GetDefaultView());

        pass.BindStorageBuffer("HitLightCache", m_lightCache);

        pass.BindStorageBuffer("HitLightCachePrevious", m_previousLightCache);

        const VoxelTextures& textures = m_voxelizer->GetVoxelTextures();

        pass.BindSampledTexture("hitReflectance", m_voxelizer->GetVoxelSampler(),
                                m_voxelizer->UsesAveragedReflectance() ? textures.pReflectance->GetDefaultView()
                                                                       : textures.pAlbedoView);
    }
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
    tiles.BindSampledTexture("coneEnvironmentMap", env.pPrefilteredSampler, env.pPrefiltered->GetDefaultView());
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

    if (succeeded)
    {
        ++m_frame;
        m_emptyLightingInitialized |= m_recordedEmptyLighting;
        if (m_recordedEnvironmentDistribution)
        {
            m_distributionTextureRevision = m_recordedTextureRevision;
        }
    }
    m_recordedEmptyLighting           = false;
    m_recordedEnvironmentDistribution = false;

    if (succeeded && m_recordedPreviousRadianceInitialization)
    {
        m_previousRadianceInitialized = true;
    }

    m_recordedPreviousRadianceInitialization   = false;

    m_lightCacheHistoryInitialized            |= succeeded && m_recordedLightCacheHistoryInitialization;

    m_recordedLightCacheHistoryInitialization  = false;

    if (succeeded && m_recordedRadiance)
    {
        m_radianceValid = true;

        ++m_radianceGeneration;
        m_skyVisibility       = m_recordedSkyVisibility;
        m_geometryRevision    = m_recordedGeometry;
        m_lightingRevision    = m_recordedLighting;
        m_environmentRevision = m_recordedEnvironment;
    }
    else if (result == RenderFrameResult::eFailed || result == RenderFrameResult::eBlocked)
    {
        m_geometryRevision            = 0;
        m_lightingRevision            = 0;
        m_distributionTextureRevision = 0;
        m_environmentRevision         = 0;
        m_skyVisibility               = UINT64_MAX;

        m_radianceValid               = false;
    }
    // Query preparation may execute in a frame which falls back before injection.
    // Publish cache revisions only when their radiance was actually recorded.
    m_recordedRadiance = false;
}

void VoxelGIRenderer::OnVisibilityGraphExecuted(RenderFrameResult result)
{
    m_rayQuery.OnRenderGraphExecuted(result);
    if (m_fieldRecorded && m_field)
    {
        m_field->OnRenderGraphExecuted(result);
    }
    m_fieldRecorded = false;
}

void VoxelGIRenderer::Destroy()
{
    m_rayQuery.Destroy();
    m_nativeDynamicGeometry.Destroy(*m_device);
    m_nativeDynamicRevision = 0;
    m_nativeDynamicCount    = 0;
    if (m_field)
    {
        m_field->Destroy();
    }
    m_field.Reset();
    m_fieldResolution     = 0;
    m_fieldActive         = false;
    m_fieldRecorded       = false;
    m_preparingField      = false;
    m_skyVisibility       = 0;
    m_hardwareQueries     = false;
    m_hardwareFeatureMask = 0;
    m_requestedHardware   = false;
    m_tierFallback        = nullptr;
    m_device->DestroyTexture(m_radiance);
    m_device->DestroyTexture(m_emptyLighting);
    m_emptyLighting                   = nullptr;
    m_emptyLightingInitialized        = false;
    m_recordedEmptyLighting           = false;
    m_recordedEnvironmentDistribution = false;
    m_frame                           = 0;

    m_device->DestroyTexture(m_previousRadiance);
    m_previousRadiance = nullptr;




    m_device->DestroyBuffer(m_lightCache);

    m_device->DestroyBuffer(m_previousLightCache);


    m_lightCache                              = nullptr;

    m_previousLightCache                      = nullptr;


    m_lightCacheBytes                         = 0;


    m_lightCacheActive                        = false;

    m_lightCacheHistoryInitialized            = false;

    m_recordedLightCacheHistoryInitialization = false;

    m_previousRadianceInitialized             = false;

    m_recordedPreviousRadianceInitialization  = false;

    m_radianceValid                           = false;

    m_recordedRadiance                        = false;


    m_device->DestroyTexture(m_skyIrradiance);

    m_device->DestroyTexture(m_unshadowedMap);

    m_unshadowedMap     = nullptr;
    m_unshadowedMapView = nullptr;

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
    m_geometryRevision            = 0;

    m_lightingRevision            = 0;

    m_distributionTextureRevision = 0;
    m_environmentRevision         = 0;
}
} // namespace zen::rc
