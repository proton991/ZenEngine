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

VkPrimitiveTopology ToVkPrimitiveTopology(DrawPrimitiveType type);

VkCullModeFlags ToVkCullModeFlags(PolygonCullMode mode);

VkFrontFace ToVkFrontFace(PolygonFrontFace frontFace);

VkSampleCountFlagBits ToVkSampleCountFlagBits(SampleCount count);

VkCompareOp ToVkCompareOp(CompareOperator op);

VkStencilOp ToVkStencilOp(StencilOperation op);

VkLogicOp ToVkLogicOp(LogicOperation op);

VkBlendOp ToVkBlendOp(BlendOperation op);

VkBlendFactor ToVkBlendFactor(BlendFactor factor);

VkDynamicState ToVkDynamicState(DynamicState state);

VkImageType ToVkImageType(TextureType type);

VkImageUsageFlags ToVkImageUsageFlags(BitField<TextureUsageFlagBits> flags);

VkBufferUsageFlags ToVkBufferUsageFlags(BitField<BufferUsageFlagBits> flags);

VkFormat ToVkFormat(DataFormat format);

VkAttachmentLoadOp ToVkAttachmentLoadOp(RenderTargetLoadOp loadOp);

VkAttachmentStoreOp ToVkAttachmentStoreOp(RenderTargetStoreOp storeOp);

VkImageLayout ToVkImageLayout(TextureLayout layout);

VkFilter ToVkFilter(SamplerFilter filter);

VkSamplerAddressMode ToVkSamplerAddressMode(SamplerRepeatMode mode);

VkBorderColor ToVkBorderColor(SamplerBorderColor color);
} // namespace zen::rhi