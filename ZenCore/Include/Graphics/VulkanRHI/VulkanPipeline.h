#pragma once
#include "VulkanHeaders.h"
#include "Common/SmallVector.h"

namespace zen::rhi
{
struct VulkanShader
{
    VulkanShader() = default;

    std::vector<VkSpecializationMapEntry> entries{};
    VkSpecializationInfo specializationInfo{};
    VkShaderStageFlags pushConstantStages{0};
    SmallVector<VkPipelineShaderStageCreateInfo> stageCreateInfos;
    SmallVector<VkDescriptorSetLayout> descriptorSetLayouts;
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
};
} // namespace zen::rhi