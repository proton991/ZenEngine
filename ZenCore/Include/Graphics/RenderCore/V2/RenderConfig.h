#pragma once
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/RHI/RHIThread.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/Shared/VoxelGI.h"
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

inline glm::uvec3 ResolveVoxelVolumeWorkgroupSize(const RHIGPUInfo& gpuInfo)
{
    // A bounded capability-based default, not a substitute for per-pass GPU profiling.
    glm::uvec3 size(ZEN_VOXEL_VOLUME_GROUP_SIZE);
    const glm::uvec3 candidates[] = {{8, 8, 8}, {8, 8, 4}, {8, 4, 4}};
    for (const glm::uvec3& candidate : candidates)
    {
        if (candidate.x <= gpuInfo.maxComputeWorkGroupSize[0] &&
            candidate.y <= gpuInfo.maxComputeWorkGroupSize[1] &&
            candidate.z <= gpuInfo.maxComputeWorkGroupSize[2] &&
            candidate.x * candidate.y * candidate.z <= gpuInfo.maxComputeWorkGroupInvocations)
        {
            size = candidate;
            break;
        }
    }
    return size;
}

inline glm::uvec3 GetVoxelVolumeDispatchGroups(uint32_t dimension, const RHIGPUInfo& gpuInfo)
{
    const glm::uvec3 size = ResolveVoxelVolumeWorkgroupSize(gpuInfo);
    return (glm::uvec3(dimension) + size - 1u) / size;
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
