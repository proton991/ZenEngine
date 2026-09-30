#include "Graphics/RenderCore/V2/VoxelResourcePlanning.h"
#include "Graphics/Shared/VoxelGI.h"
#include <limits>

namespace zen::rc
{
GIResourceStatus ValidateGIStorageBuffer(uint64_t elements,
                                         uint32_t stride,
                                         const RHIGPUInfo& gpu,
                                         uint64_t& bytes)
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
        bytes = elements * stride;

        if (bytes > std::numeric_limits<uint32_t>::max())
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

GIResourceStatus ValidateVoxelReflectanceResources(uint32_t resolution,
                                                   uint64_t triangles,
                                                   uint64_t budgetBytes,
                                                   uint64_t retiringBytes,
                                                   const RHIGPUInfo& gpu,
                                                   uint64_t& peakBytes)
{
    GIResourceStatus status = GIResourceStatus::eInvalidInput;

    peakBytes = 0;

    if ((resolution == 64 || resolution == 128 || resolution == 256) && budgetBytes != 0)
    {
        const uint64_t cells = uint64_t(resolution) * resolution * resolution;

        uint64_t scratchBytes = 0;

        status = ValidateGIStorageBuffer(cells, 16, gpu, scratchBytes);

        if (status == GIResourceStatus::eSuccess)
        {
            const uint64_t currentBytes = GetVoxelReflectanceRequiredBytes(resolution);

            if (triangles > std::numeric_limits<uint32_t>::max() / ZEN_VOXEL_REFLECTANCE_SCALE ||
                retiringBytes > std::numeric_limits<uint64_t>::max() - currentBytes)
            {
                status = GIResourceStatus::eOverflow;
            }
            else
            {
                peakBytes = currentBytes + retiringBytes;

                status = peakBytes <= budgetBytes ? GIResourceStatus::eSuccess :
                                                    GIResourceStatus::eBudget;
            }
        }
    }

    return status;
}
} // namespace zen::rc
