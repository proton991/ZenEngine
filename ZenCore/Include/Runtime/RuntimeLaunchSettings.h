#pragma once
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/RHI/RHIThread.h"
#include "Runtime/NavigationCamera.h"
#include <filesystem>
#include <string_view>

namespace zen::runtime
{
inline constexpr uint32_t RuntimeSetupVersion  = 1;
inline constexpr size_t   MaxRuntimeSetupBytes = 1024 * 1024;

struct RuntimeRunSettings
{
    uint32_t         windowWidth{1440};
    uint32_t         windowHeight{900};
    uint32_t         renderWidth{1440};
    uint32_t         renderHeight{900};
    bool             followWindow{true};
    bool             vsync{true};
    RHIPresentMode   presentMode{RHIPresentMode::eDefault};
    RHIExecutionMode rhiMode{RHIExecutionMode::eThreaded};
    bool             validation{false};
    bool             gpuTimings{false};
    float            cameraMoveSpeed{kDefaultNavigationCameraMoveSpeed};
    uint32_t         framesInFlight{3};
    uint32_t         workerThreads{8};
    float            shadowDepthBias{1.25f};
    float            shadowSlopeBias{1.75f};
};

struct RuntimeLaunchSettings
{
    uint32_t    version{RuntimeSetupVersion};
    std::string packagePath;
    std::string sourcePath;
    std::string processingProfile;
    // Resolved source configuration for renderer options outside RenderingSettings.
    // It is installed before any runtime singleton/device construction.
    std::string           engineConfiguration;
    std::string           configurationDirectory;
    std::string           defaultEnvironment;
    rc::RenderingSettings rendering;
    NavigationCameraState camera;
    RuntimeRunSettings    run;
    bool                  animationEnabled{true};
    uint32_t              animationClip{0};
    bool                  animationLoop{true};
};

// No file access: validates the standalone setup before renderer initialization.
bool ValidateRuntimeLaunchSettings(const RuntimeLaunchSettings& settings, std::string& error);
bool EncodeRuntimeLaunchSettings(const RuntimeLaunchSettings& settings, std::string& json, std::string& error);
bool DecodeRuntimeLaunchSettings(std::string_view json, RuntimeLaunchSettings& settings, std::string& error);
// Transactional reads and atomic replacement. A failed write preserves the old file.
bool ReadRuntimeLaunchSettings(const std::filesystem::path& path, RuntimeLaunchSettings& settings, std::string& error);
bool WriteRuntimeLaunchSettings(const std::filesystem::path& path, const RuntimeLaunchSettings& settings, std::string& error);
} // namespace zen::runtime
