#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include <algorithm>
#include <cmath>

namespace zen::rc
{
bool ValidateRenderingSettings(const RenderingSettings& settings, std::string& error)
{
    error.clear();

    if (settings.algorithm > RenderAlgorithm::eVoxelGI || !ValidateVoxelGIRuntimeSettings(settings.gi))
    {
        error = "Invalid algorithm or GI settings. Grid sizes are 64, 128 or 256; shadow maps are 128 to 2048.";
    }
    else if (!std::isfinite(settings.environment.intensity) || settings.environment.intensity < 0.0f
             || !std::isfinite(settings.environment.rotationDegrees))
    {
        error = "Environment intensity must be finite and non-negative; rotation must be finite.";
    }
    else if (!ValidateCameraLight(settings.cameraLight))
    {
        error = "Camera light color, intensity and range must be finite and non-negative.";
    }
    else if (settings.debug.output > DebugOutput::eVoxelSlice || !std::isfinite(settings.debug.minimum)
             || !std::isfinite(settings.debug.maximum) || settings.debug.maximum <= settings.debug.minimum
             || settings.debug.face >= 6 || settings.debug.axis >= 3 || settings.debug.mip != 0
             || (settings.debug.output == DebugOutput::eVoxelSlice && settings.debug.slice >= settings.gi.resolution))
    {
        error = "Invalid debug range or subresource. Surface voxels currently expose mip zero only.";
    }
    else if (!std::isfinite(settings.lightMarkerSize) || settings.lightMarkerSize <= 0.0f
             || !std::isfinite(settings.normalizationScale) || settings.normalizationScale <= 0.0f
             || !std::isfinite(settings.normalizationCenter.x) || !std::isfinite(settings.normalizationCenter.y)
             || !std::isfinite(settings.normalizationCenter.z) || settings.lights.size() > MaxSceneLights)
    {
        error = "Invalid normalization, marker size or light capacity (maximum 32 lights).";
    }

    for (size_t index = 0; error.empty() && index < settings.lights.size(); ++index)
    {
        const RenderingLight& entry = settings.lights[index];

        if (entry.id == 0 || entry.origin > LightOrigin::eCorners || !SceneLights::Validate(entry.light))
        {
            error = "Invalid light: check direction, non-negative values and spot angles (inner < outer <= 90).";
        }

        for (size_t previous = 0; previous < index; ++previous)
        {
            if (settings.lights[previous].id == entry.id)
            {
                error = "Light configuration IDs must be unique.";
            }
        }
    }

    return error.empty();
}

bool RequiresRenderingResourceApply(const RenderingSettings& previous, const RenderingSettings& next)
{
    return RequiresVoxelGIRebuild(previous.gi, next.gi) || previous.gi.shadowMapResolution != next.gi.shadowMapResolution;
}

void HoldRenderingResources(const RenderingSettings& applied, RenderingSettings& settings)
{
    // Matches RequiresVoxelGIRebuild: the budget is a resource only with averaged reflectance.
    if (applied.gi.averagedReflectance || settings.gi.averagedReflectance)
    {
        settings.gi.reflectanceBudgetBytes = applied.gi.reflectanceBudgetBytes;
    }

    settings.gi.averagedReflectance = applied.gi.averagedReflectance;

    settings.gi.resolution          = applied.gi.resolution;

    settings.gi.voxelizer           = applied.gi.voxelizer;

    settings.gi.shadowMapResolution = applied.gi.shadowMapResolution;

    settings.debug.slice            = std::min(settings.debug.slice, settings.gi.resolution - 1);
}
} // namespace zen::rc
