#pragma once
#include "Graphics/RHI/RHICommon.h"
#include <algorithm>
#include <limits>

namespace zen::rc
{
struct ComputeDispatchChunk
{
    glm::uvec3 groups{0, 1, 1};
    uint32_t firstItem{0};
    uint32_t itemCount{0};
};

// A caller advances by itemCount until all work is recorded. Within a dispatch:
// group = x + numGroups.x * (y + numGroups.y * z), local = group * groupSize + lane.
// Test local < itemCount BEFORE adding firstItem. Padding never becomes valid work.
inline bool BuildComputeDispatchChunk(uint32_t firstItem,
                                      uint32_t remainingItems,
                                      uint32_t groupSize,
                                      const RHIGPUInfo& gpu,
                                      ComputeDispatchChunk& output)
{
    bool valid = groupSize > 0 && groupSize <= gpu.maxComputeWorkGroupInvocations &&
        groupSize <= gpu.maxComputeWorkGroupSize[0] &&
        uint64_t(firstItem) + remainingItems <= std::numeric_limits<uint32_t>::max();
    ComputeDispatchChunk chunk;
    chunk.firstItem          = firstItem;
    uint64_t groupCapacity   = valid ? std::numeric_limits<uint32_t>::max() / groupSize : 0;
    uint64_t remainingGroups = valid ? (uint64_t(remainingItems) + groupSize - 1) / groupSize : 0;
    for (uint32_t axis = 0; axis < 3 && valid; ++axis)
    {
        const uint64_t limit = std::min(uint64_t(gpu.maxComputeWorkGroupCount[axis]),
                                        uint64_t(std::numeric_limits<int32_t>::max()));
        valid                = limit > 0 && groupCapacity > 0;
        if (valid && remainingItems > 0)
        {
            const uint64_t count =
                std::min(std::max(uint64_t(1), remainingGroups), std::min(limit, groupCapacity));
            chunk.groups[axis] = static_cast<uint32_t>(count);
            remainingGroups    = (remainingGroups + count - 1) / count;
            groupCapacity /= count;
        }
    }
    if (valid)
    {
        const uint64_t capacity =
            uint64_t(chunk.groups.x) * chunk.groups.y * chunk.groups.z * groupSize;
        chunk.itemCount = static_cast<uint32_t>(std::min(uint64_t(remainingItems), capacity));
        output          = chunk;
    }
    return valid;
}
} // namespace zen::rc
