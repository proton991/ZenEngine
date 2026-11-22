#include "Math/Math.h"
#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/VoxelGI.h"
#include <algorithm>
#include <limits>
#include <cmath>

namespace zen::rc
{
uint32_t PlanGIRadianceCache(const RHIGPUMemoryStats& memory,
                             const RHIGPUInfo&        gpu,
                             const Vec3&              sceneExtent,
                             uint64_t                 existingBytes)
{
    const uint32_t strides[] = {8, 16, 16};
    return PlanGIRadianceCache(memory, gpu, sceneExtent, existingBytes, strides);
}

uint32_t PlanGIRadianceCache(const RHIGPUMemoryStats&  memory,
                             const RHIGPUInfo&         gpu,
                             const Vec3&               sceneExtent,
                             uint64_t                  existingBytes,
                             ArrayView<const uint32_t> strides,
                             uint64_t                  fixedBytes,
                             uint32_t                  patchesPerAxis,
                             uint32_t                  entriesPerPatch)
{
    uint64_t entryBytes = 0;
    for (uint32_t stride : strides)
    {
        entryBytes += stride;
    }
    uint64_t available = 0;
    for (uint32_t index = 0; memory.budgetAvailable && index < memory.heapCount && index < memory.heaps.size(); ++index)
    {
        const RHIGPUMemoryStats::Heap& heap = memory.heaps[index];
        if (heap.deviceLocal && heap.budgetBytes != 0)
        {
            const uint64_t limit      = heap.budgetBytes - heap.budgetBytes / 10;
            const uint64_t otherUsage = heap.usageBytes - std::min(existingBytes, heap.usageBytes);
            available                 = std::max(available, limit > otherUsage ? (limit - otherUsage) / 4 : 0);
        }
    }
    const double area  = 2.0
                       * (double(sceneExtent.x) * sceneExtent.y + double(sceneExtent.y) * sceneExtent.z
                          + double(sceneExtent.z) * sceneExtent.x);
    const double scale = std::max(double(sceneExtent.x), std::max(double(sceneExtent.y), double(sceneExtent.z)));
    // World coordinates may be normalized by the scene importer. Express density relative
    // to the bounds, with the caller's surface directions per patch.
    const double   patch     = patchesPerAxis != 0 ? scale / double(patchesPerAxis) : 0.0;
    const uint64_t demand    = std::isfinite(area) && area > 0 && patch > 0
                                 ? uint64_t(std::min(area * entriesPerPatch / (patch * patch), double(UINT32_MAX)))
                                 : 0;
    const uint64_t alignment = std::max(uint64_t(1), uint64_t(gpu.storageBufferAlignment));
    const uint64_t padding   = (alignment - fixedBytes % alignment) % alignment;
    available            = available >= fixedBytes && available - fixedBytes >= padding ? available - fixedBytes - padding : 0;
    const uint64_t limit = entryBytes != 0 ? std::min(available / entryBytes, demand) : 0;
    uint32_t       capacity = 0;
    uint64_t       bytes    = 0;
    for (uint64_t entries = 1; entries <= limit && entries <= (uint64_t(1) << 30); entries *= 2)
    {
        uint64_t allocated = 0;
        bool     valid     = true;
        for (uint32_t stride : strides)
        {
            allocated += (entries * stride + alignment - 1) / alignment * alignment;
            valid      = valid && ValidateGIStorageBuffer(entries, stride, gpu, bytes) == GIResourceStatus::eSuccess;
        }
        if (allocated <= available && valid)
        {
            capacity = uint32_t(entries);
        }
    }
    return capacity;
}

GIResourceStatus ValidateGIStorageBuffer(uint64_t elements, uint32_t stride, const RHIGPUInfo& gpu, uint64_t& bytes)
{
    GIResourceStatus status = GIResourceStatus::eSuccess;

    if (stride == 0)
    {
        status = GIResourceStatus::eInvalidInput;
    }
    else if (elements > std::numeric_limits<uint64_t>::max() / stride)
    {
        status = GIResourceStatus::eOverflow;
    }
    else
    {
        bytes                    = elements * stride;

        const uint64_t alignment = std::max(uint64_t(1), uint64_t(gpu.storageBufferAlignment));

        if (alignment > std::numeric_limits<uint32_t>::max() || bytes > std::numeric_limits<uint32_t>::max() - (alignment - 1))
        {
            status = GIResourceStatus::eBufferSize;
        }
        else if (bytes > gpu.maxStorageBufferRange)
        {
            status = GIResourceStatus::eDescriptorRange;
        }
    }

    return status;
}

uint64_t GetVoxelReflectanceRequiredBytes(uint32_t resolution)
{
    const bool valid = resolution == 64 || resolution == 128 || resolution == 256;

    return valid ? uint64_t(resolution) * resolution * resolution * 20 : 0;
}

GIResourceStatus ValidateVoxelReflectanceResources(uint32_t          resolution,
                                                   uint64_t          triangles,
                                                   uint64_t          budgetBytes,
                                                   uint64_t          retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t&         peakBytes)
{
    GIResourceStatus status = GIResourceStatus::eInvalidInput;

    peakBytes               = 0;

    if ((resolution == 64 || resolution == 128 || resolution == 256) && budgetBytes != 0)
    {
        const uint64_t cells  = uint64_t(resolution) * resolution * resolution;

        uint64_t scratchBytes = 0;

        status                = ValidateGIStorageBuffer(cells, 16, gpu, scratchBytes);

        if (status == GIResourceStatus::eSuccess)
        {
            const uint64_t currentBytes = GetVoxelReflectanceRequiredBytes(resolution);

            if (triangles > std::numeric_limits<uint32_t>::max() / ZEN_VOXEL_REFLECTANCE_SCALE
                || retiringBytes > std::numeric_limits<uint64_t>::max() - currentBytes)
            {
                status = GIResourceStatus::eOverflow;
            }
            else
            {
                peakBytes = currentBytes + retiringBytes;

                status    = peakBytes <= budgetBytes ? GIResourceStatus::eSuccess : GIResourceStatus::eBudget;
            }
        }
    }

    return status;
}

void StartFieldThroughputGeneration(FieldThroughputPacing& pacing)
{
    pacing.cycles      = 1;
    pacing.probeFrames = 0;
    pacing.probed      = false;
}

bool PaceFieldThroughput(FieldThroughputPacing& pacing, const FieldThroughputLimits& limits, double intervalUs, bool submitted)
{
    bool record = true;

    if (pacing.probeFrames > 0)
    {
        --pacing.probeFrames;

        record = false;

        if (pacing.probeFrames == 0)
        {
            pacing.renderIntervalUs = intervalUs;
        }
    }
    else if (submitted)
    {
        const double budget = std::max(
            limits.budgetUs, pacing.renderIntervalUs > 0.0 ? pacing.renderIntervalUs * (1.0 + limits.budgetShare) : 0.0);

        if (intervalUs <= budget)
        {
            if (pacing.batchCap < std::min(limits.batchCeiling, limits.triangleCount))
            {
                pacing.batchCap = std::min(limits.batchCeiling, pacing.batchCap * 2u);
            }
            else
            {
                pacing.cycles = std::min(limits.maxCycles, pacing.cycles * 2u);
            }
        }
        else if (!pacing.probed && limits.probeFrames > 0 && limits.budgetShare > 0.0)
        {
            pacing.probed      = true;
            pacing.probeFrames = limits.probeFrames - 1;
            record             = false;

            if (pacing.probeFrames == 0)
            {
                pacing.renderIntervalUs = intervalUs;
            }
        }
        else if (pacing.cycles > 1)
        {
            pacing.cycles /= 2u;
        }
        else
        {
            pacing.batchCap = std::max(1u, pacing.batchCap / 2u);
        }
    }
    else
    {
        pacing.cycles = 1;
    }

    return record;
}
} // namespace zen::rc
