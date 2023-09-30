#pragma once
#include <cstdint>

namespace zen::val
{
struct VulkanDescriptorPoolSizes
{
    uint32_t maxDescriptorSets;
    uint32_t numSeparateSamplerDescriptors;
    uint32_t numCombinedSamplerDescriptors;
    uint32_t numSampledImageDescriptors;
    uint32_t numStorageImageDescriptors;
    uint32_t numUniformBufferDescriptors;
    uint32_t numStorageBufferDescriptors;
    uint32_t numUniformTexelBufferDescriptors;
    uint32_t numStorageTexelBufferDescriptors;
    uint32_t numInputAttachmentDescriptors;
};

inline VulkanDescriptorPoolSizes MainDescriptorPoolSizes()
{
    return {8192, 1024, 8192, 8192, 1024, 4096, 4096, 1024, 1024, 256};
}
} // namespace zen::val