#pragma once
#include "VulkanHeaders.h"
#include "Common/SmallVector.h"

namespace zen::rhi
{
struct VulkanShader
{
    VulkanShader() = default;

    struct VertexInputInfo
    {
        SmallVector<VkVertexInputBindingDescription> vkBindings;
        SmallVector<VkVertexInputAttributeDescription> vkAttributes;
        VkPipelineVertexInputStateCreateInfo stateCI{};
    } vertexInputInfo;
    std::vector<VkSpecializationMapEntry> entries{};
    VkSpecializationInfo specializationInfo{};
    SmallVector<VkPipelineShaderStageCreateInfo> stageCreateInfos;
    SmallVector<VkDescriptorSetLayout> descriptorSetLayouts;
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
};
} // namespace zen::rhi