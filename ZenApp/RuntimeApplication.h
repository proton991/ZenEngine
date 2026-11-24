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
    uint64_t                     readWorkers{4};
    uint64_t                     giEntryFrame{0};
    uint64_t                     captureStart{0};
    HeapVector<uint64_t>         captureFrames; // Sorted and unique; writes <capture>.frame-N.ppm at only these frames.
    std::string                  capture;
    std::string                  pipelineCache;
    std::string                  profile;
    std::string                  logPath;
    bool                         requireValidation{};
    bool                         fixedStep{false};
    bool                         startPaused{false};
    bool                         rhiOverride{false};
    bool                         noUI{false};
    bool                         gpuMarkers{false};
    bool                         deviceLossDiagnostics{};
    std::optional<uint64_t>      stallReportSeconds;
    std::optional<uint64_t>      stallFatalSeconds;
    bool                         pipelineStatistics{false};
    bool                         hybridHitStream{false};
    bool                         hybridRayCounts{false};
    bool                         disableRT{false};
    uint64_t                     deviceMemoryLimitMB{0}; // Zero uses the device's own heaps.
    bool                         gpuCulling{true};
};

RuntimeExit RunApplication(const RuntimeLaunchSettings& setup, const RuntimeOptions& options);
} // namespace zen::runtime
