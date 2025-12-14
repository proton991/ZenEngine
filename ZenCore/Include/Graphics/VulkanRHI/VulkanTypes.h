#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommon.h"

namespace zen
{
/**
 * Convert RHI RHIShaderResourceType to VkDescriptorType
 * @param shaderResourceType RHIShaderResourceType
 * @return Corresponding VkDescriptorType
 */
VkDescriptorType ShaderResourceTypeToVkDescriptorType(RHIShaderResourceType shaderResourceType);

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(RHIShaderStage stage);

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(
    BitField<RHIShaderStageFlagBits> stageFlags);

VkPrimitiveTopology ToVkPrimitiveTopology(RHIDrawPrimitiveType type);

VkCullModeFlags ToVkCullModeFlags(RHIPolygonCullMode mode);

VkFrontFace ToVkFrontFace(RHIPolygonFrontFace frontFace);

VkSampleCountFlagBits ToVkSampleCountFlagBits(SampleCount count);

VkCompareOp ToVkCompareOp(RHIDepthCompareOperator op);

VkStencilOp ToVkStencilOp(RHIStencilOp op);

VkLogicOp ToVkLogicOp(RHIBlendLogicOp op);

VkBlendOp ToVkBlendOp(RHIBlendOp op);

VkBlendFactor ToVkBlendFactor(RHIBlendFactor factor);

VkDynamicState ToVkDynamicState(RHIDynamicState state);

VkImageType ToVkImageType(RHITextureType type);

VkImageViewType ToVkImageViewType(RHITextureType type);

VkImageUsageFlags ToVkImageUsageFlags(BitField<RHITextureUsageFlagBits> flags);

VkBufferUsageFlags ToVkBufferUsageFlags(BitField<RHIBufferUsageFlagBits> flags);

VkFormat ToVkFormat(DataFormat format);

VkAttachmentLoadOp ToVkAttachmentLoadOp(RHIRenderTargetLoadOp loadOp);

VkAttachmentStoreOp ToVkAttachmentStoreOp(RHIRenderTargetStoreOp storeOp);

VkImageLayout ToVkImageLayout(RHITextureLayout layout);

VkAccessFlags ToVkAccessFlags(BitField<RHIAccessFlagBits> access);

VkImageAspectFlags ToVkAspectFlags(BitField<RHITextureAspectFlagBits> aspect);

VkFilter ToVkFilter(RHISamplerFilter filter);

VkSamplerAddressMode ToVkSamplerAddressMode(RHISamplerRepeatMode mode);

VkBorderColor ToVkBorderColor(RHISamplerBorderColor color);

VkClearColorValue ToVkClearColor(const RHIRenderPassClearValue& clearValue);

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderPassClearValue& clearValue);

void ToVkClearColor(const Color& color, VkClearColorValue* colorValue);

void ToVkImageSubresourceRange(const RHITextureSubResourceRange& range,
                               VkImageSubresourceRange* vkRange);

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers,
                                VkImageSubresourceLayers* vkLayers);

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* copy);

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* copy);
} // namespace zen