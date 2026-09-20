#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIThread.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Platform/ConfigLoader.h"
#include <string_view>

namespace zen::rc
{
using AsyncComputeMode = platform::AsyncComputeMode;

enum class AsyncComputeStatus
{
    eAvailable,
    eDisabled,
    eComputeUnavailable,
    eSharedGraphicsQueue,
    eDependenciesUnavailable
};

inline AsyncComputeStatus ResolveAsyncComputeStatus(AsyncComputeMode mode,
                                                    const RHIQueueCapabilities& capabilities)
{
    AsyncComputeStatus status = AsyncComputeStatus::eAvailable;
    if (mode == AsyncComputeMode::eDisabled)
    {
        status = AsyncComputeStatus::eDisabled;
    }
    else if (!capabilities.computeSupported)
    {
        status = AsyncComputeStatus::eComputeUnavailable;
    }
    else if (capabilities.AreQueuesShared(RHICommandContextType::eGraphics,
                                          RHICommandContextType::eAsyncCompute))
    {
        status = AsyncComputeStatus::eSharedGraphicsQueue;
    }
    else if (!capabilities.asyncSubmissionDependencies)
    {
        status = AsyncComputeStatus::eDependenciesUnavailable;
    }
    return status;
}

inline const char* GetAsyncComputeStatusReason(AsyncComputeStatus status)
{
    const char* reason = "unknown async compute status";
    switch (status)
    {
        case AsyncComputeStatus::eAvailable: reason = "available"; break;
        case AsyncComputeStatus::eDisabled: reason = "disabled by configuration"; break;
        case AsyncComputeStatus::eComputeUnavailable: reason = "compute queue unavailable"; break;
        case AsyncComputeStatus::eSharedGraphicsQueue:
            reason = "compute shares graphics queue";
            break;
        case AsyncComputeStatus::eDependenciesUnavailable:
            reason = "timeline dependencies unavailable";
            break;
    }
    return reason;
}

inline bool ParseAsyncComputeOverride(std::string_view argument, AsyncComputeMode& mode)
{
    const bool valid = argument == "--async-compute=0" || argument == "--async-compute=1";
    if (valid)
    {
        mode = argument.back() == '1' ? AsyncComputeMode::eAuto : AsyncComputeMode::eDisabled;
    }
    return valid;
}

inline platform::VoxelizerMode ResolveVoxelizerMode(platform::VoxelizerMode requestedMode,
                                                    const RHIGPUInfo& gpuInfo)
{
    if (requestedMode != platform::VoxelizerMode::eCompute && gpuInfo.supportGeometryShader)
    {
        return platform::VoxelizerMode::eGeometry;
    }
    return platform::VoxelizerMode::eCompute;
}

struct RenderConfig
{
    static RenderConfig& GetInstance()
    {
        static RenderConfig instance;
        return instance;
    }
    // Depth bias (and slope) are used to avoid shadowing artifacts
    // Constant depth bias factor (always applied)
    float depthBiasConstant = 1.25f;
    // Slope depth bias factor, applied depending on polygon's slope
    float depthBiasSlope = 1.75f;
    // Size of shadow map
    uint32_t shadowMapSize = 2048;

    uint32_t offScreenFbSize = 2048;

    uint32_t numFrames = 3;

    RHIExecutionMode rhiExecutionMode{RHIExecutionMode::eThreaded};

    AsyncComputeMode asyncComputeMode{platform::ConfigLoader::GetInstance().GetAsyncComputeMode()};

    uint32_t numThreads = 8;

    DataFormat shadowDepthFormat{DataFormat::eD16UNORM};
};
} // namespace zen::rc
