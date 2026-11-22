#pragma once
#include <volk.h>
#include "ZenRHI/TestSupport/ScopedVulkanCall.h"

namespace zen::test
{
struct SpatialQualificationPipelineFailure
{
    static inline uint32_t calls;

    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice,
                                                 VkPipelineCache,
                                                 uint32_t count,
                                                 const VkComputePipelineCreateInfo*,
                                                 const VkAllocationCallbacks*,
                                                 VkPipeline* pipelines)
    {
        ++calls;
        for (uint32_t index = 0; index < count; ++index)
        {
            pipelines[index] = VK_NULL_HANDLE;
        }
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
};
} // namespace zen::test
