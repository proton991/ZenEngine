#pragma once
#include "Runtime/RuntimeLaunchSettings.h"

namespace zen::runtime
{
enum class RuntimeExit : int
{
    Success  = 0,
    Setup    = 2,
    Package  = 3,
    Renderer = 4
};

struct RuntimeOptions
{
    uint64_t    frames{0};
    std::string capture;
    std::string profile;
    std::string logPath;
    bool        fixedStep{false};
    bool        startPaused{false};
    bool        rhiOverride{false};
};

RuntimeExit RunApplication(const RuntimeLaunchSettings& setup, const RuntimeOptions& options);
} // namespace zen::runtime
