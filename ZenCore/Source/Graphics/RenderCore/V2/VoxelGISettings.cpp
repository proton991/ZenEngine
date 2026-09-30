#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include <limits>

namespace zen::rc
{
bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings)
{
    const bool voxelizer = settings.voxelizer == platform::VoxelizerMode::eAuto ||
        settings.voxelizer == platform::VoxelizerMode::eCompute ||
        settings.voxelizer == platform::VoxelizerMode::eGeometry;

    const bool async = settings.asyncCompute == platform::AsyncComputeMode::eDisabled ||
        settings.asyncCompute == platform::AsyncComputeMode::eAuto;

    return ValidateDynamicVoxelGISettings(settings.dynamic) &&
        ValidateVoxelGISettings(settings.cone) && voxelizer && async &&
        (!settings.averagedReflectance || settings.reflectanceBudgetBytes != 0) &&
        settings.shadowMapResolution >= 128 && settings.shadowMapResolution <= 2048;
}

bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config,
                                VoxelGIRuntimeSettings& output)
{
    VoxelGIRuntimeSettings settings;

    const std::string voxelizer = config.GetString("voxelizer", "auto");

    const std::string async = config.GetString("async_compute", "off");

    const std::string reflectance = config.GetString("voxel_reflectance_policy", "owner");

    uint64_t budgetMiB = 0;

    bool valid = LoadDynamicVoxelGISettings(config, settings.dynamic) &&
        LoadVoxelGISettings(config, settings.cone) &&
        (voxelizer == "auto" || voxelizer == "comp" || voxelizer == "geom") &&
        (async == "off" || async == "auto") &&
        (reflectance == "owner" || reflectance == "averaged") &&
        config.ReadNumber("voxel_reflectance_budget_mb", budgetMiB) &&
        budgetMiB <= std::numeric_limits<uint64_t>::max() / (1024 * 1024) &&
        config.ReadNumber("shadow_map_resolution", settings.shadowMapResolution);

    if (valid)
    {
        settings.voxelizer = config.GetVoxelizerMode();

        settings.asyncCompute = config.GetAsyncComputeMode();

        settings.averagedReflectance = reflectance == "averaged";

        settings.reflectanceBudgetBytes = budgetMiB * 1024 * 1024;

        valid = ValidateVoxelGIRuntimeSettings(settings);
    }

    if (valid)
    {
        output = settings;
    }

    return valid;
}

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous,
                            const VoxelGIRuntimeSettings& next)
{
    return previous.voxelizer != next.voxelizer ||
        previous.averagedReflectance != next.averagedReflectance ||
        previous.reflectanceBudgetBytes != next.reflectanceBudgetBytes ||
        previous.dynamic.resolution != next.dynamic.resolution ||
        previous.dynamic.raysPerFace != next.dynamic.raysPerFace ||
        previous.dynamic.neighborRadius != next.dynamic.neighborRadius ||
        previous.dynamic.backend != next.dynamic.backend ||
        previous.dynamic.compactCache != next.dynamic.compactCache ||
        previous.dynamic.memoryBudgetBytes != next.dynamic.memoryBudgetBytes;
}
} // namespace zen::rc
