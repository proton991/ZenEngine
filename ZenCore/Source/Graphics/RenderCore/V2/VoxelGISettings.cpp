#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include "Graphics/RenderCore/V2/RenderConfig.h"
#include <spdlog/fmt/fmt.h>
#include <algorithm>
#include <limits>
#include <sstream>

namespace zen::rc
{
namespace
{
constexpr GIMethod   kOptimalMethod   = GIMethod::ScreenProbes;
constexpr GIDenoiser kOptimalDenoiser = GIDenoiser::Spatiotemporal;
constexpr GIShadows  kOptimalShadows  = GIShadows::RayTracedReuse;

const char* const kMethodNames[]      = {"per-pixel", "per-pixel-restir", "per-pixel-cache", "per-pixel-restir-cache",
                                         "screen-probes"};
const char* const kTracingNames[]     = {"auto", "compute", "ray-queries", "rt-pipeline"};
const char* const kDenoiserNames[]    = {"spatiotemporal", "temporal", "spatial", "off"};
const char* const kShadowNames[]      = {"maps", "ray-traced", "ray-traced-reuse"};
const char* const kPresetNames[]      = {"custom", "low", "medium", "high", "ultra", "native", "balanced", "optimal"};

constexpr GIConfigOption kOptions[]   = {
    {"voxel_gi_enabled", "true|false", false},
    {"voxel_gi_preset", "optimal|custom|balanced|native|low|medium|high|ultra", false},
    {"voxel_gi_method", "per-pixel|per-pixel-restir|per-pixel-cache|per-pixel-restir-cache|screen-probes", true},
    {"voxel_gi_tracing", "auto|compute|ray-queries|rt-pipeline", true},
    {"voxel_gi_pixel_resolution_divisor", "auto|1|2|4", true},
    {"voxel_gi_restir_spatial", "true|false", true},
    {"voxel_gi_restir_spatial_samples", "1..4", true},
    {"voxel_gi_field_resolution", "32|64", true},
    {"voxel_resolution", "64|128|256", true},
    {"voxel_gi_denoiser", "spatiotemporal|temporal|spatial|off", true},
    {"voxel_gi_bicubic_history", "true|false", true},
    {"voxel_gi_data_driven_anti_lag", "true|false", true},
    {"voxel_gi_anti_firefly", "true|false", true},
    {"voxel_gi_young_receiver_samples", "true|false", true},
    {"voxel_gi_footprint_receivers", "true|false", true},
    {"voxel_gi_world_cache_seed", "true|false", true},
    {"voxel_gi_world_cache_tile_size", "6..16", true},
    {"voxel_gi_world_cache_history", "1..128", true},
    {"voxel_gi_world_cache_unused_frames", "1..4096", true},
    {"voxel_gi_shadows", "maps|ray-traced|ray-traced-reuse", true},
    {"shadow_map_resolution", "128..2048", true},
    {"voxel_gi_indirect_diffuse", "true|false", false},
    {"voxel_gi_indirect_intensity", "0..10", false},
    {"voxel_gi_analytic_lighting", "true|false", false},
    {"voxel_gi_environment_lighting", "true|false", false},
    {"voxel_gi_emissive_lighting", "true|false", false},
    {"voxel_gi_specular_occlusion", "true|false", false},
    {"voxel_gi_reflections", "true|false|auto", false},
    {"voxel_gi_shadow_enabled", "true|false", false},
};

// Lower-level and older keys that also choose the configuration. Present in a config, they turn the
// optimal configuration off like the table's configuration options. UpdateGIConfigText replaces them.
const char* const kLegacyConfigurationKeys[] = {"voxel_gi_mode",
                                                "voxel_gi_backend",
                                                "voxel_gi_ray_provider",
                                                "voxel_gi_ray_tracing_pipeline",
                                                "voxel_gi_restir",
                                                "voxel_gi_restir_specular",
                                                "voxel_gi_restir_spatial_visibility",
                                                "voxel_gi_screen_probes",
                                                "voxel_gi_world_radiance_cache",
                                                "voxel_gi_world_cache_feedback",
                                                "voxel_gi_temporal",
                                                "voxel_gi_filter",
                                                "voxel_gi_rt_shadows",
                                                "voxel_gi_native_shadow_reuse",
                                                "voxel_gi_shadow_reuse",
                                                "voxel_gi_samples",
                                                "voxel_gi_history_frames"};

template <size_t Count> int32_t FindName(const char* const (&names)[Count], const std::string& name)
{
    int32_t index = -1;
    for (size_t candidate = 0; candidate < Count && index < 0; ++candidate)
    {
        if (name == names[candidate])
        {
            index = static_cast<int32_t>(candidate);
        }
    }
    return index;
}

// Reads an enumerated key. Missing keys leave value unchanged; unknown names are invalid.
template <class Enum, size_t Count>
bool ReadName(const platform::ConfigLoader& config, const char* key, const char* const (&names)[Count], Enum& value)
{
    bool valid = true;
    if (config.HasKey(key))
    {
        const int32_t index = FindName(names, config.GetString(key, ""));
        valid               = index >= 0;
        if (valid)
        {
            value = static_cast<Enum>(index);
        }
    }
    return valid;
}


// The optimal configuration for hardware RT with an automatic provider; content is kept.
void ApplyOptimalDefaults(VoxelGIRuntimeSettings& settings)
{
    VoxelGIRuntimeSettings optimal;
    CopyVoxelGIContent(settings, optimal);
    optimal.optimal    = true;
    optimal.resolution = 128;
    SetGITracing(optimal.cone, GITracing::Automatic);
    SetGIMethod(optimal.cone, kOptimalMethod);
    optimal.cone.pixelResolutionDivisor = 1;
    SetGIDenoiser(optimal.cone, kOptimalDenoiser);
    SetGIShadows(optimal.cone, kOptimalShadows);
    settings = optimal;
}

const char* FormatBool(bool value)
{
    return value ? "true" : "false";
}

std::string GetGIOptionValue(std::string_view key, const VoxelGIRuntimeSettings& settings, bool enabled)
{
    const VoxelGISettings& cone          = settings.cone;
    const char* const      reflections[] = {"auto", "false", "true"};
    std::string            value;
    if (key == "voxel_gi_enabled")
    {
        value = FormatBool(enabled);
    }
    else if (key == "voxel_gi_preset")
    {
        value = IsOptimalVoxelGIConfiguration(settings) ? "optimal" : "custom";
    }
    else if (key == "voxel_gi_method")
    {
        value = GetGIMethodName(GetGIMethod(cone));
    }
    else if (key == "voxel_gi_tracing")
    {
        value = GetGITracingName(GetGITracing(cone));
    }
    else if (key == "voxel_gi_pixel_resolution_divisor")
    {
        value = cone.pixelResolutionDivisor == 0 ? "auto" : std::to_string(cone.pixelResolutionDivisor);
    }
    else if (key == "voxel_gi_restir_spatial")
    {
        value = FormatBool(cone.restirSpatial);
    }
    else if (key == "voxel_gi_restir_spatial_samples")
    {
        value = std::to_string(cone.restirSpatialSamples);
    }
    else if (key == "voxel_gi_field_resolution")
    {
        value = std::to_string(cone.fieldResolution);
    }
    else if (key == "voxel_resolution")
    {
        value = std::to_string(settings.resolution);
    }
    else if (key == "voxel_gi_denoiser")
    {
        value = GetGIDenoiserName(GetGIDenoiser(cone));
    }
    else if (key == "voxel_gi_bicubic_history")
    {
        value = FormatBool(cone.bicubicHistory);
    }
    else if (key == "voxel_gi_data_driven_anti_lag")
    {
        value = FormatBool(cone.dataDrivenAntiLag);
    }
    else if (key == "voxel_gi_anti_firefly")
    {
        value = FormatBool(cone.antiFirefly);
    }
    else if (key == "voxel_gi_young_receiver_samples")
    {
        value = FormatBool(cone.youngReceiverSamples);
    }
    else if (key == "voxel_gi_footprint_receivers")
    {
        value = FormatBool(cone.footprintReceivers);
    }
    else if (key == "voxel_gi_world_cache_tile_size")
    {
        value = std::to_string(cone.worldCacheTileSize);
    }
    else if (key == "voxel_gi_world_cache_history")
    {
        value = std::to_string(cone.worldCacheHistory);
    }
    else if (key == "voxel_gi_world_cache_unused_frames")
    {
        value = std::to_string(cone.worldCacheUnusedFrames);
    }
    else if (key == "voxel_gi_world_cache_seed")
    {
        value = FormatBool(cone.worldCacheSeed);
    }
    else if (key == "voxel_gi_shadows")
    {
        value = GetGIShadowsName(GetGIShadows(cone));
    }
    else if (key == "shadow_map_resolution")
    {
        value = std::to_string(settings.shadowMapResolution);
    }
    else if (key == "voxel_gi_indirect_diffuse")
    {
        value = FormatBool(cone.indirectDiffuse);
    }
    else if (key == "voxel_gi_indirect_intensity")
    {
        value = fmt::format("{}", cone.indirectIntensity);
    }
    else if (key == "voxel_gi_analytic_lighting")
    {
        value = FormatBool(cone.analyticLighting);
    }
    else if (key == "voxel_gi_environment_lighting")
    {
        value = FormatBool(cone.environmentLighting);
    }
    else if (key == "voxel_gi_emissive_lighting")
    {
        value = FormatBool(cone.emissiveLighting);
    }
    else if (key == "voxel_gi_specular_occlusion")
    {
        value = FormatBool(cone.specularOcclusion);
    }
    else if (key == "voxel_gi_reflections")
    {
        value = reflections[static_cast<uint32_t>(cone.reflections)];
    }
    else if (key == "voxel_gi_shadow_enabled")
    {
        value = FormatBool(cone.shadows);
    }
    return value;
}

// Options written for these settings: the optimal configuration omits what it chooses itself.
bool WritesGIOption(const GIConfigOption& option, const VoxelGIRuntimeSettings& settings)
{
    return !IsOptimalVoxelGIConfiguration(settings) || !option.configuration;
}

bool IsGIConfigKey(std::string_view key)
{
    bool found = std::find(std::begin(kLegacyConfigurationKeys), std::end(kLegacyConfigurationKeys), key)
              != std::end(kLegacyConfigurationKeys);
    for (const GIConfigOption& option : kOptions)
    {
        found = found || key == option.key;
    }
    return found;
}

bool HasConfigurationKey(const platform::ConfigLoader& config)
{
    bool found = false;
    for (const char* key : kLegacyConfigurationKeys)
    {
        found = found || config.HasKey(key);
    }
    for (const GIConfigOption& option : kOptions)
    {
        found = found || (option.configuration && config.HasKey(option.key));
    }
    return found;
}

// The key of a "key=value" line, or empty for comments and other lines.
std::string_view GetLineKey(std::string_view line)
{
    const size_t     first = line.find_first_not_of(" \t");
    const size_t     equal = line.find('=');
    std::string_view key;
    if (first != std::string_view::npos && line[first] != '#' && equal != std::string_view::npos && equal > first)
    {
        key = line.substr(first, equal - first);
        key = key.substr(0, key.find_last_not_of(" \t") + 1);
    }
    return key;
}
} // namespace

void CopyVoxelGIContent(const VoxelGIRuntimeSettings& source, VoxelGIRuntimeSettings& target)
{
    target.cone.indirectDiffuse     = source.cone.indirectDiffuse;
    target.cone.indirectIntensity   = source.cone.indirectIntensity;
    target.cone.analyticLighting    = source.cone.analyticLighting;
    target.cone.environmentLighting = source.cone.environmentLighting;
    target.cone.emissiveLighting    = source.cone.emissiveLighting;
    target.cone.specularOcclusion   = source.cone.specularOcclusion;
    target.cone.reflections         = source.cone.reflections;
    target.cone.shadows             = source.cone.shadows;
    target.asyncCompute             = source.asyncCompute;
    target.averagedReflectance      = source.averagedReflectance;
    target.reflectanceBudgetBytes   = source.reflectanceBudgetBytes;
}

GIMethod GetGIMethod(const VoxelGISettings& settings)
{
    GIMethod method = settings.restirGI ? GIMethod::PixelReSTIR : GIMethod::Pixel;
    if (settings.screenProbeGI)
    {
        method = GIMethod::ScreenProbes;
    }
    else if (settings.worldRadianceCache)
    {
        method = settings.restirGI ? GIMethod::PixelReSTIRCache : GIMethod::PixelCache;
    }
    return method;
}

void SetGIMethod(VoxelGISettings& settings, GIMethod method)
{
    settings.screenProbeGI = method == GIMethod::ScreenProbes;
    settings.restirGI      = method == GIMethod::PixelReSTIR || method == GIMethod::PixelReSTIRCache;
    settings.worldRadianceCache =
        method == GIMethod::PixelCache || method == GIMethod::PixelReSTIRCache || method == GIMethod::ScreenProbes;
}

const char* GetGIMethodName(GIMethod method)
{
    return kMethodNames[std::min(static_cast<uint32_t>(method), static_cast<uint32_t>(GIMethod::Count) - 1)];
}

GITracing GetGITracing(const VoxelGISettings& settings)
{
    GITracing tracing = GITracing::Automatic;
    if (settings.rayProvider == VoxelGISettings::RayProvider::Voxel)
    {
        tracing = GITracing::Compute;
    }
    else if (settings.rayProvider == VoxelGISettings::RayProvider::Hardware)
    {
        tracing = settings.rayTracingPipeline ? GITracing::RayTracingPipeline : GITracing::RayQueries;
    }
    return tracing;
}

void SetGITracing(VoxelGISettings& settings, GITracing tracing)
{
    using Provider              = VoxelGISettings::RayProvider;
    settings.rayProvider        = tracing == GITracing::Automatic ? Provider::Auto
                                : tracing == GITracing::Compute   ? Provider::Voxel
                                                                  : Provider::Hardware;
    settings.rayTracingPipeline = tracing != GITracing::RayQueries;
}

const char* GetGITracingName(GITracing tracing)
{
    return kTracingNames[std::min(static_cast<uint32_t>(tracing), static_cast<uint32_t>(GITracing::Count) - 1)];
}

GIDenoiser GetGIDenoiser(const VoxelGISettings& settings)
{
    return settings.temporal ? (settings.filter ? GIDenoiser::Spatiotemporal : GIDenoiser::Temporal)
                             : (settings.filter ? GIDenoiser::Spatial : GIDenoiser::Off);
}

void SetGIDenoiser(VoxelGISettings& settings, GIDenoiser denoiser)
{
    settings.temporal = denoiser == GIDenoiser::Spatiotemporal || denoiser == GIDenoiser::Temporal;
    settings.filter   = denoiser == GIDenoiser::Spatiotemporal || denoiser == GIDenoiser::Spatial;
}

const char* GetGIDenoiserName(GIDenoiser denoiser)
{
    return kDenoiserNames[std::min(static_cast<uint32_t>(denoiser), static_cast<uint32_t>(GIDenoiser::Count) - 1)];
}

GIShadows GetGIShadows(const VoxelGISettings& settings)
{
    return !settings.rtShadows        ? GIShadows::ShadowMaps
         : settings.UsesShadowReuse() ? GIShadows::RayTracedReuse
                                      : GIShadows::RayTraced;
}

void SetGIShadows(VoxelGISettings& settings, GIShadows shadows)
{
    settings.rtShadows         = shadows != GIShadows::ShadowMaps;
    settings.nativeShadowReuse = shadows == GIShadows::RayTracedReuse;
    settings.shadowReuse       = settings.nativeShadowReuse;
}

const char* GetGIShadowsName(GIShadows shadows)
{
    return kShadowNames[std::min(static_cast<uint32_t>(shadows), static_cast<uint32_t>(GIShadows::Count) - 1)];
}

const char* GetGIMethodUnavailableReason(GIMethod method, GITracing tracing)
{
    const bool restir = method == GIMethod::PixelReSTIR || method == GIMethod::PixelReSTIRCache;
    const bool cache =
        method == GIMethod::PixelCache || method == GIMethod::PixelReSTIRCache || method == GIMethod::ScreenProbes;
    const char* reason = nullptr;
    if (restir && tracing == GITracing::Compute)
    {
        reason = "ReSTIR reuse needs hardware RT";
    }
    else if (cache && tracing == GITracing::RayQueries)
    {
        reason = "The world cache needs the RT pipeline or Compute";
    }
    return reason;
}

const char* GetGITracingUnavailableReason(GITracing tracing, const RHIGPUInfo& gpu)
{
    const char* reason = nullptr;
    if (tracing == GITracing::RayQueries || tracing == GITracing::RayTracingPipeline)
    {
        VoxelGISettings settings;
        SetGITracing(settings, tracing);
        reason = GetHardwareTierUnavailableReason(settings, gpu);
    }
    return reason;
}

void ApplyOptimalVoxelGIConfiguration(VoxelGIRuntimeSettings& settings, const RHIGPUInfo& gpu)
{
    // An explicitly forced provider (ZenApp --backend) stays; the rest follows its tier.
    const VoxelGISettings::RayProvider provider = settings.cone.rayProvider;
    ApplyOptimalDefaults(settings);
    settings.cone.rayProvider = provider;
    // Automatic falls back to field tracing for the same gather. A hardware-only request
    // on a query-only device uses the per-pixel method supported by that tier.
    if (provider == VoxelGISettings::RayProvider::Hardware && gpu.rayQuery.IsUsable()
        && GetGITracingUnavailableReason(GITracing::RayTracingPipeline, gpu) != nullptr)
    {
        SetGITracing(settings.cone, GITracing::RayQueries);
        SetGIMethod(settings.cone, GetOptimalGIMethod(GITracing::RayQueries));
        settings.cone.pixelResolutionDivisor = 0;
    }
}

bool IsOptimalVoxelGIConfiguration(const VoxelGIRuntimeSettings& settings)
{
    VoxelGIRuntimeSettings optimal = settings;
    ApplyOptimalDefaults(optimal);
    optimal.cone.rayProvider = settings.cone.rayProvider;
    return settings.optimal && optimal == settings;
}

GITracing GetOptimalGITracing(bool rayQueries, bool rayTracingPipeline)
{
    return rayQueries && rayTracingPipeline ? GITracing::RayTracingPipeline : GITracing::Compute;
}

GIMethod GetOptimalGIMethod(GITracing tracing)
{
    return tracing == GITracing::RayQueries ? GIMethod::PixelReSTIR : kOptimalMethod;
}

const char* GetVoxelGIPresetName(VoxelGIPreset preset)
{
    return kPresetNames[preset <= VoxelGIPreset::Optimal ? static_cast<uint32_t>(preset) : 0];
}

bool ApplyVoxelGIPreset(VoxelGIPreset preset, VoxelGIRuntimeSettings& settings)
{
    const bool valid = preset >= VoxelGIPreset::Low && preset <= VoxelGIPreset::Optimal;
    if (preset == VoxelGIPreset::Optimal)
    {
        ApplyOptimalDefaults(settings);
    }
    else if (preset == VoxelGIPreset::Balanced)
    {
        settings            = {};
        settings.resolution = 128;
        SetGITracing(settings.cone, GITracing::Automatic);
        SetGIMethod(settings.cone, GIMethod::PixelReSTIR);
        settings.cone.pixelResolutionDivisor = 4;
        settings.cone.restirSpatialSamples   = 2;
    }
    else if (valid)
    {
        settings.optimal                     = false;
        settings.resolution                  = preset >= VoxelGIPreset::Ultra ? 256 : preset == VoxelGIPreset::High ? 128 : 64;
        settings.cone.screenProbeGI          = preset == VoxelGIPreset::Native;
        settings.cone.restirGI               = false;
        settings.cone.rayTracingPipeline     = settings.cone.screenProbeGI;
        settings.cone.worldRadianceCache     = settings.cone.screenProbeGI;
        settings.cone.worldCacheFeedback     = true;
        settings.cone.nativeShadowReuse      = true;
        settings.cone.shadowReuse            = false;
        settings.cone.nativeProbeBenchmark   = false;
        settings.cone.radianceCache          = false;
        settings.cone.pixelResolutionDivisor = settings.cone.screenProbeGI ? 1u : 2u;
        settings.cone.shadows                = true;
        settings.cone.rtShadows              = true;
        settings.cone.rayProvider            = preset == VoxelGIPreset::Low    ? VoxelGISettings::RayProvider::Voxel
                                             : preset == VoxelGIPreset::Medium ? VoxelGISettings::RayProvider::Auto
                                                                               : VoxelGISettings::RayProvider::Hardware;
        // GI without HW RT traces the distance field, including the sky cache; direct shadows use maps.
        settings.cone.fieldResolution     = 64;
        settings.cone.samples             = 4;
        settings.cone.referenceSamples    = 0;
        settings.cone.referenceTileStride = 1;
        settings.cone.historyFrames       = 32;
        settings.cone.temporal            = true;
        settings.cone.filter              = true;
        // A pass the GI tracing panel switched off comes back on.
        settings.cone.indirectDiffuse   = true;
        settings.cone.specularOcclusion = true;
        settings.cone.reflections =
            preset == VoxelGIPreset::Low ? VoxelGISettings::Reflections::Off : VoxelGISettings::Reflections::On;
        settings.shadowMapResolution = 1024;
    }
    return valid;
}

VoxelGIPreset GetVoxelGIPreset(const VoxelGIRuntimeSettings& settings)
{
    const VoxelGISettings& cone   = settings.cone;
    VoxelGIPreset          preset = VoxelGIPreset::Custom;
    VoxelGIRuntimeSettings balanced;
    ApplyVoxelGIPreset(VoxelGIPreset::Balanced, balanced);
    balanced.cone.rayProvider = cone.rayProvider;
    if (IsOptimalVoxelGIConfiguration(settings))
    {
        preset = VoxelGIPreset::Optimal;
    }
    else if (settings == balanced)
    {
        preset = VoxelGIPreset::Balanced;
    }
    else if ((cone.screenProbeGI || (cone.pixelResolutionDivisor == 2 && !cone.restirGI)) && cone.samples == 4
             && cone.referenceSamples == 0 && cone.historyFrames == 32 && cone.temporal && cone.filter && cone.indirectDiffuse
             && cone.specularOcclusion && cone.shadows && cone.rtShadows && settings.shadowMapResolution == 1024
             && cone.fieldResolution == 64 && !cone.shadowReuse)
    {
        if (cone.screenProbeGI)
        {
            if (settings.resolution == 256 && cone.rayTracingPipeline && cone.worldCacheFeedback && cone.worldRadianceCache
                && cone.reflections == VoxelGISettings::Reflections::On)
            {
                preset = VoxelGIPreset::Native;
            }
        }
        else if (settings.resolution == 64 && cone.reflections == VoxelGISettings::Reflections::Off)
        {
            preset = VoxelGIPreset::Low;
        }
        else if (cone.reflections == VoxelGISettings::Reflections::On)
        {
            preset = settings.resolution == 64  ? VoxelGIPreset::Medium
                   : settings.resolution == 128 ? VoxelGIPreset::High
                   : settings.resolution == 256 ? VoxelGIPreset::Ultra
                                                : VoxelGIPreset::Custom;
        }
    }
    return preset;
}

const char* ResolveVoxelGIAutomaticProvider(VoxelGIRuntimeSettings& settings, const RHIGPUInfo& gpu)
{
    const char* reason = "explicit_provider";
    if (IsOptimalVoxelGIConfiguration(settings))
    {
        ApplyOptimalVoxelGIConfiguration(settings, gpu);
    }
    else
    {
        // An edited configuration field wins over the optimal choice.
        settings.optimal = false;
    }
    if (settings.cone.rayProvider == VoxelGISettings::RayProvider::Auto)
    {
        // Capability decides, never device or driver identity. Usable ray queries imply the
        // acceleration-structure, device-address and build features the hardware tier needs; the
        // RT-pipeline tier also needs its pipelines. A scene that cannot build its structures
        // moves the automatic tier to Compute at runtime (VoxelGIRenderer::SetAutomaticTier).
        const char* unavailable = GetHardwareTierUnavailableReason(settings.cone, gpu);
        settings.cone.rayProvider =
            unavailable == nullptr ? VoxelGISettings::RayProvider::Hardware : VoxelGISettings::RayProvider::Voxel;
        reason = unavailable == nullptr ? "ray_query_available" : unavailable;
    }
    return reason;
}

bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings)
{
    const bool async = settings.asyncCompute == platform::AsyncComputeMode::eDisabled
                    || settings.asyncCompute == platform::AsyncComputeMode::eAuto;

    return (settings.resolution == 64 || settings.resolution == 128 || settings.resolution == 256)
        && ValidateVoxelGISettings(settings.cone) && async
        && (!settings.averagedReflectance || settings.reflectanceBudgetBytes != 0) && settings.shadowMapResolution >= 128
        && settings.shadowMapResolution <= 2048;
}

bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config,
                                VoxelGIRuntimeSettings&       output,
                                const VoxelGIRuntimeSettings* inherited)
{
    VoxelGIRuntimeSettings settings = inherited != nullptr ? *inherited : VoxelGIRuntimeSettings{};

    // "custom" applies no preset: keys absent from the configuration keep the built-in or inherited values.
    // Any other name must be a preset, so a typo is rejected rather than read as custom.
    const std::string presetName  = config.GetString("voxel_gi_preset", inherited != nullptr ? "custom" : "optimal");
    bool              presetValid = presetName == GetVoxelGIPresetName(VoxelGIPreset::Custom);
    if (presetValid && config.HasKey("voxel_gi_preset"))
    {
        settings.optimal = false;
    }
    for (uint32_t index = 1; index <= static_cast<uint32_t>(VoxelGIPreset::Optimal); ++index)
    {
        if (presetName == GetVoxelGIPresetName(static_cast<VoxelGIPreset>(index)))
        {
            presetValid = ApplyVoxelGIPreset(static_cast<VoxelGIPreset>(index), settings);
        }
    }

    if (config.HasKey("voxel_gi_mode"))
    {
        // Older name of voxel_gi_method, whose screen probes also selected the RT pipeline.
        const std::string mode             = config.GetString("voxel_gi_mode", "");
        const bool        legacyPixelMode  = mode == "pixel" || mode == "restir";
        presetValid                       &= mode == "per-pixel" || mode == "screen-probes" || legacyPixelMode;
        SetGIMethod(settings.cone, mode == "screen-probes" ? GIMethod::ScreenProbes
                                   : mode == "restir"      ? GIMethod::PixelReSTIR
                                                           : GIMethod::Pixel);
        settings.cone.rayTracingPipeline = true;
    }

    const std::string async =
        config.GetString("async_compute", settings.asyncCompute == platform::AsyncComputeMode::eAuto ? "auto" : "off");

    const std::string reflectance =
        config.GetString("voxel_reflectance_policy", settings.averagedReflectance ? "averaged" : "owner");

    uint64_t budgetMiB = 0;

    bool valid         = presetValid && config.ReadNumber("voxel_resolution", settings.resolution)
                      && LoadVoxelGISettings(config, settings.cone, settings.cone) && (async == "off" || async == "auto")
                      && (reflectance == "owner" || reflectance == "averaged")
                      && config.ReadNumber("voxel_reflectance_budget_mb", budgetMiB)
                      && budgetMiB <= std::numeric_limits<uint64_t>::max() / (1024 * 1024)
                      && config.ReadNumber("shadow_map_resolution", settings.shadowMapResolution);

    // The combined keys follow the lower-level keys they set, so they decide when both are present.
    GIMethod   method   = GetGIMethod(settings.cone);
    GITracing  tracing  = GetGITracing(settings.cone);
    GIDenoiser denoiser = GetGIDenoiser(settings.cone);
    GIShadows  shadows  = GetGIShadows(settings.cone);
    valid               = valid && ReadName(config, "voxel_gi_method", kMethodNames, method)
                       && ReadName(config, "voxel_gi_tracing", kTracingNames, tracing)
                       && ReadName(config, "voxel_gi_denoiser", kDenoiserNames, denoiser)
                       && ReadName(config, "voxel_gi_shadows", kShadowNames, shadows);

    if (valid)
    {
        if (config.HasKey("voxel_gi_method"))
        {
            SetGIMethod(settings.cone, method);
        }
        if (config.HasKey("voxel_gi_tracing"))
        {
            SetGITracing(settings.cone, tracing);
        }
        if (config.HasKey("voxel_gi_denoiser"))
        {
            SetGIDenoiser(settings.cone, denoiser);
        }
        if (config.HasKey("voxel_gi_shadows"))
        {
            SetGIShadows(settings.cone, shadows);
        }
        settings.optimal      = settings.optimal && !HasConfigurationKey(config);

        settings.asyncCompute = async == "auto" ? platform::AsyncComputeMode::eAuto : platform::AsyncComputeMode::eDisabled;

        settings.averagedReflectance = reflectance == "averaged";

        if (config.HasKey("voxel_reflectance_budget_mb"))
        {
            settings.reflectanceBudgetBytes = budgetMiB * 1024 * 1024;
        }

        valid = ValidateVoxelGIRuntimeSettings(settings);
    }

    if (valid)
    {
        output = settings;
    }

    return valid;
}

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous, const VoxelGIRuntimeSettings& next)
{
    return previous.averagedReflectance != next.averagedReflectance
        || ((previous.averagedReflectance || next.averagedReflectance)
            && previous.reflectanceBudgetBytes != next.reflectanceBudgetBytes)
        || previous.resolution != next.resolution;
}

std::span<const GIConfigOption> GetGIConfigOptions()
{
    return kOptions;
}

std::string GetGIConfigArgumentName(const GIConfigOption& option)
{
    std::string_view key = option.key;
    if (key.starts_with("voxel_gi_"))
    {
        key.remove_prefix(9);
    }
    std::string name(key);
    std::replace(name.begin(), name.end(), '_', '-');
    return name;
}

const GIConfigOption* FindGIConfigArgument(std::string_view name)
{
    const GIConfigOption* found = nullptr;
    for (const GIConfigOption& option : kOptions)
    {
        if (found == nullptr && GetGIConfigArgumentName(option) == name)
        {
            found = &option;
        }
    }
    return found;
}

std::string FormatGIConfig(const VoxelGIRuntimeSettings& settings, bool enabled)
{
    std::string text;
    for (const GIConfigOption& option : kOptions)
    {
        if (WritesGIOption(option, settings))
        {
            text += fmt::format("{}={}\n", option.key, GetGIOptionValue(option.key, settings, enabled));
        }
    }
    return text;
}

std::string FormatGIArguments(const VoxelGIRuntimeSettings& settings, bool enabled)
{
    std::string text;
    for (const GIConfigOption& option : kOptions)
    {
        if (WritesGIOption(option, settings))
        {
            text += fmt::format("{}--gi-{}={}", text.empty() ? "" : " ", GetGIConfigArgumentName(option),
                                GetGIOptionValue(option.key, settings, enabled));
        }
    }
    return text;
}

std::string UpdateGIConfigText(std::string_view text, const VoxelGIRuntimeSettings& settings, bool enabled)
{
    std::string        output;
    bool               inserted = false;
    std::istringstream lines{std::string(text)};
    std::string        line;
    while (std::getline(lines, line))
    {
        if (!IsGIConfigKey(GetLineKey(line)))
        {
            output += line + "\n";
        }
        else if (!inserted)
        {
            output   += FormatGIConfig(settings, enabled);
            inserted  = true;
        }
    }
    if (!inserted)
    {
        output += (output.empty() || output.ends_with("\n\n") ? "" : "\n") + FormatGIConfig(settings, enabled);
    }
    return output;
}

} // namespace zen::rc
