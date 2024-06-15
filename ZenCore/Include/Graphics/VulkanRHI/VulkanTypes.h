#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommon.h"

namespace zen::rhi
{
/**
 * Convert RHI ShaderResourceType to VkDescriptorType
 * @param shaderResourceType ShaderResourceType
 * @return Corresponding VkDescriptorType
 */
VkDescriptorType ShaderResourceTypeToVkDescriptorType(ShaderResourceType shaderResourceType);

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(ShaderStage stage);

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(
    BitField<ShaderStageFlagBits> stageFlags);
} // namespace zen::rhi