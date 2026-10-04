#pragma once
#include "Platform/ConfigLoader.h"
#include "Graphics/RenderCore/V2/Renderer/VoxelGIRenderer.h"

namespace zen::rc
{
// Apply on the render/main thread between frame recordings. Resource changes may stall.
struct VoxelGIRuntimeSettings
{
    uint32_t                   resolution{256};
    VoxelGISettings            cone;
    platform::VoxelizerMode    voxelizer{platform::VoxelizerMode::eAuto};
    platform::AsyncComputeMode asyncCompute{platform::AsyncComputeMode::eDisabled};
    bool                       averagedReflectance{false};
    uint64_t                   reflectanceBudgetBytes{0};
    uint32_t                   shadowMapResolution{1024};

    bool operator==(const VoxelGIRuntimeSettings&) const = default;
};

bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings);

// Parsing is transactional: a rejected configuration leaves output untouched.
bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config, VoxelGIRuntimeSettings& output);

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous, const VoxelGIRuntimeSettings& next);

} // namespace zen::rc
