#pragma once
#include "Graphics/RenderCore/V2/SceneLighting.h"
#include "Graphics/RenderCore/V2/VoxelGISettings.h"
#include <string>

namespace zen::rc
{
// Values only: shared by the editor preview and the future player setup file.
enum class RenderAlgorithm : uint32_t
{
    ePBR,
    eVoxelGI
};

enum class DebugOutput : uint32_t
{
    eFinal,
    eDepth,
    eAlbedo,
    eNormal,
    eShadow,
    eVoxels,
    eVoxelSlice
};

enum class LightOrigin : uint32_t
{
    eGLTF,
    eManual,
    eSides,
    eCorners
};

struct RenderingLight
{
    uint64_t    id{0};
    LightOrigin origin{LightOrigin::eManual};
    SceneLight  light;
};

struct EnvironmentSettings
{
    std::string texturePath;
    float       intensity{1.0f};
    float       rotationDegrees{0.0f};
    bool        lighting{true};
    bool        skybox{true};
};

struct DebugSelection
{
    DebugOutput output{DebugOutput::eFinal};
    bool        linearDepth{true};
    float       minimum{0.0f};
    float       maximum{10.0f};
    uint64_t    lightId{0}; // Configuration identity, resolved to a runtime LightId by the adapter.
    uint32_t    face{0};
    uint32_t    axis{2};
    uint32_t    slice{0};
    uint32_t    mip{0};
};

struct RenderingSettings
{
    std::string                scenePath;
    Vec3                       normalizationCenter{0.0f};
    float                      normalizationScale{1.0f};
    RenderAlgorithm            algorithm{RenderAlgorithm::ePBR};
    HeapVector<RenderingLight> lights;
    CameraLightSettings        cameraLight;
    EnvironmentSettings        environment;
    VoxelGIRuntimeSettings     gi;
    DebugSelection             debug;
    bool                       lightMarkers{false};
    float                      lightMarkerSize{0.02f};
};

bool ValidateRenderingSettings(const RenderingSettings& settings, std::string& error);

bool RequiresRenderingResourceApply(const RenderingSettings& previous, const RenderingSettings& next);

// Keeps the values that require Apply at their applied state, so other edits can preview
// first; selections that depend on those values are clamped to them.
void HoldRenderingResources(const RenderingSettings& applied, RenderingSettings& settings);
} // namespace zen::rc
