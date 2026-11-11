#pragma once
#include "Runtime/RuntimeLaunchSettings.h"
#include "GroundTruthBatch.h"
#include "CameraPath.h"
#include "LightPath.h"

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
    std::string                  groundTruth;
    std::string                  output;
    HeapVector<GroundTruthFrame> batch;
    std::string                  cameraPath;
    HeapVector<CameraPathKey>    cameraKeys;
    std::string                  lightPath;
    LightPath                    lightKeys;
    uint64_t                     frames{0};
    uint64_t                     captureStart{0};
    std::string                  capture;
    std::string                  profile;
    std::string                  logPath;
    bool                         fixedStep{false};
    bool                         startPaused{false};
    bool                         rhiOverride{false};
    bool                         noUI{false};
    bool                         gpuMarkers{false};
    bool                         disableRT{false};
    bool                         gpuCulling{true};
};

RuntimeExit RunApplication(const RuntimeLaunchSettings& setup, const RuntimeOptions& options);
} // namespace zen::runtime
