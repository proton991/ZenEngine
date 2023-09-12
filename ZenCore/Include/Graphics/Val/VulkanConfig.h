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
    uint32_t numUniformTexelBufferDescriptors;
    uint32_t numStorageTexelBufferDescriptors;
    uint32_t numUniformBufferDescriptors;
    uint32_t numStorageBufferDescriptors;
    uint32_t numInputAttachmentDescriptors;
};
} // namespace zen::val