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

enum class VoxelGIPreset : uint32_t
{
    Custom,
    Low,
    Medium,
    High,
    Ultra
};

const char*   GetVoxelGIPresetName(VoxelGIPreset preset);
VoxelGIPreset GetVoxelGIPreset(const VoxelGIRuntimeSettings& settings);
bool          ApplyVoxelGIPreset(VoxelGIPreset preset, VoxelGIRuntimeSettings& settings);
// Resolve only inherited Auto requests from device capability: HW RT where ray queries are
// usable, Compute otherwise. Explicit per-feature requests remain authoritative.
const char* ResolveVoxelGIAutomaticProvider(VoxelGIRuntimeSettings& settings, const RHIGPUInfo& gpu);

bool ValidateVoxelGIRuntimeSettings(const VoxelGIRuntimeSettings& settings);

// Parsing is transactional: a rejected configuration leaves output untouched.
bool LoadVoxelGIRuntimeSettings(const platform::ConfigLoader& config, VoxelGIRuntimeSettings& output);

bool RequiresVoxelGIRebuild(const VoxelGIRuntimeSettings& previous, const VoxelGIRuntimeSettings& next);

} // namespace zen::rc
