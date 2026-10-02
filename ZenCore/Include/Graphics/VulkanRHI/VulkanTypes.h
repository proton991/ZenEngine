#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommon.h"

#include "VulkanResourceCast.h"

namespace zen
{
/**
 * Convert RHI RHIShaderResourceType to VkDescriptorType
 * @param shaderResourceType RHIShaderResourceType
 * @return Corresponding VkDescriptorType
 */
VkDescriptorType ShaderResourceTypeToVkDescriptorType(RHIShaderResourceType shaderResourceType);

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(RHIShaderStage stage);

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(BitField<RHIShaderStageFlagBits> stageFlags);

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

VkImageViewCreateInfo MakeVkImageViewCreateInfo(RHITextureType                    type,
                                                DataFormat                        format,
                                                VkImage                           image,
                                                const RHITextureSubResourceRange& range);

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

VkClearColorValue ToVkClearColor(const RHIRenderTargetClearValue& clearValue);

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderTargetClearValue& clearValue);

void ToVkClearColor(const Color& color, VkClearColorValue* pColorValue);

void ToVkImageSubresourceRange(const RHITextureSubResourceRange& range, VkImageSubresourceRange* pVkRange);

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers, VkImageSubresourceLayers* pVkLayers);

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* pCopy);

void ToVkImageBlit(const RHITextureBlitRegion& region, VkImageBlit* pBlit);

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* pCopy);
} // namespace zen
