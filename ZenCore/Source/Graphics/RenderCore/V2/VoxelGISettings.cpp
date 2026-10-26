#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include <limits>

namespace zen::rc
{
const char* GetVoxelGIPresetName(VoxelGIPreset preset)
{
    const char* names[] = {"custom", "low", "medium", "high", "ultra"};
    return names[preset <= VoxelGIPreset::Ultra ? static_cast<uint32_t>(preset) : 0];
}

bool ApplyVoxelGIPreset(VoxelGIPreset preset, VoxelGIRuntimeSettings& settings)
{
    const bool valid = preset >= VoxelGIPreset::Low && preset <= VoxelGIPreset::Ultra;
    if (valid)
    {
        settings.resolution = preset == VoxelGIPreset::Ultra ? 256 : preset == VoxelGIPreset::High ? 128 : 64;
        settings.cone.SetAllFeatureProviders(VoxelGISettings::RayProvider::Auto);
        settings.cone.rayProvider = preset == VoxelGIPreset::Low    ? VoxelGISettings::RayProvider::Voxel
                                  : preset == VoxelGIPreset::Medium ? VoxelGISettings::RayProvider::Auto
                                                                    : VoxelGISettings::RayProvider::Hardware;
        // GI without HW RT traces bricks, the sky cache traces triangles, and direct shadows use maps.
        settings.cone.computeTraversal = VoxelGISettings::ComputeTraversal::Bricks;
        settings.cone.brickResolution  = 128;
        settings.cone.gather           = VoxelGISettings::Gather::Probes;
        settings.cone.probeSpacing     = 16;
        settings.cone.samples          = 4;
        settings.cone.adaptiveSampling = true;
        settings.cone.referenceSamples = 0;
        settings.cone.historyFrames    = 32;
        settings.cone.temporal         = true;
        settings.cone.filter           = true;
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
    if (cone.samples == 4 && cone.adaptiveSampling && cone.referenceSamples == 0 && cone.historyFrames == 32 && cone.temporal
        && cone.filter && cone.indirectDiffuse && cone.specularOcclusion && cone.shadows && cone.rtShadows
        && cone.bounceSource == VoxelGISettings::BounceSource::Auto && settings.shadowMapResolution == 1024
        && cone.computeTraversal == VoxelGISettings::ComputeTraversal::Bricks && cone.brickResolution == 128
        && cone.gather == VoxelGISettings::Gather::Probes && cone.probeSpacing == 16)
    {
        if (settings.resolution == 64 && cone.reflections == VoxelGISettings::Reflections::Off)
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
    if (settings.cone.rayProvider == VoxelGISettings::RayProvider::Auto)
    {
        // Capability decides, never device or driver identity. Usable ray queries imply the
        // acceleration-structure, device-address and build features the hardware tier needs;
        // a scene that cannot build its structures still falls back visibly at runtime.
        const bool hardware       = gpu.rayQuery.IsUsable();
        settings.cone.rayProvider = hardware ? VoxelGISettings::RayProvider::Hardware : VoxelGISettings::RayProvider::Voxel;
        reason                    = hardware ? "ray_query_available" : "ray_query_unavailable";
    }
    return reason;
}

bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings)
{
    const bool voxelizer = settings.voxelizer == platform::VoxelizerMode::eAuto
                        || settings.voxelizer == platform::VoxelizerMode::eCompute
                        || settings.voxelizer == platform::VoxelizerMode::eGeometry;

    const bool async     = settings.asyncCompute == platform::AsyncComputeMode::eDisabled
                        || settings.asyncCompute == platform::AsyncComputeMode::eAuto;

    return (settings.resolution == 64 || settings.resolution == 128 || settings.resolution == 256)
        && ValidateVoxelGISettings(settings.cone) && voxelizer && async
        && (!settings.averagedReflectance || settings.reflectanceBudgetBytes != 0) && settings.shadowMapResolution >= 128
        && settings.shadowMapResolution <= 2048;
}

bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config, VoxelGIRuntimeSettings& output)
{
    VoxelGIRuntimeSettings settings;

    // "custom" applies no preset: keys absent from the configuration keep the built-in defaults.
    // Any other name must be a preset, so a typo is rejected rather than read as custom.
    const std::string presetName  = config.GetString("voxel_gi_preset", "medium");
    bool              presetValid = presetName == GetVoxelGIPresetName(VoxelGIPreset::Custom);
    for (uint32_t index = 1; index <= static_cast<uint32_t>(VoxelGIPreset::Ultra); ++index)
    {
        if (presetName == GetVoxelGIPresetName(static_cast<VoxelGIPreset>(index)))
        {
            presetValid = ApplyVoxelGIPreset(static_cast<VoxelGIPreset>(index), settings);
        }
    }

    const std::string voxelizer   = config.GetString("voxelizer", "auto");

    const std::string async       = config.GetString("async_compute", "off");

    const std::string reflectance = config.GetString("voxel_reflectance_policy", "owner");

    uint64_t budgetMiB            = 0;

    bool valid                    = presetValid && config.ReadNumber("voxel_resolution", settings.resolution)
                                 && LoadVoxelGISettings(config, settings.cone, settings.cone)
                                 && (voxelizer == "auto" || voxelizer == "comp" || voxelizer == "geom") && (async == "off" || async == "auto")
                                 && (reflectance == "owner" || reflectance == "averaged")
                                 && config.ReadNumber("voxel_reflectance_budget_mb", budgetMiB)
                                 && budgetMiB <= std::numeric_limits<uint64_t>::max() / (1024 * 1024)
                                 && config.ReadNumber("shadow_map_resolution", settings.shadowMapResolution);

    if (valid)
    {
        settings.voxelizer              = config.GetVoxelizerMode();

        settings.asyncCompute           = config.GetAsyncComputeMode();

        settings.averagedReflectance    = reflectance == "averaged";

        settings.reflectanceBudgetBytes = budgetMiB * 1024 * 1024;

        valid                           = ValidateVoxelGIRuntimeSettings(settings);
    }

    if (valid)
    {
        output = settings;
    }

    return valid;
}

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous, const VoxelGIRuntimeSettings& next)
{
    return previous.voxelizer != next.voxelizer || previous.averagedReflectance != next.averagedReflectance
        || ((previous.averagedReflectance || next.averagedReflectance)
            && previous.reflectanceBudgetBytes != next.reflectanceBudgetBytes)
        || previous.resolution != next.resolution;
}
} // namespace zen::rc
