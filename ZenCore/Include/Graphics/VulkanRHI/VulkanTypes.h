#pragma once
#include "VulkanHeaders.h"
#include "Graphics/RHI/RHICommon.h"

#define TO_VK_TEXTURE(tex)           (dynamic_cast<VulkanTexture*>(tex))
#define TO_CVK_TEXTURE(tex)          (dynamic_cast<const VulkanTexture*>(tex))
#define TO_VK_BUFFER(buffer)         (dynamic_cast<VulkanBuffer*>(buffer))
#define TO_VK_PIPELINE(handle)       (dynamic_cast<VulkanPipeline*>(handle))
#define TO_VK_DESCRIPTORSET(handle)  (dynamic_cast<VulkanDescriptorSet*>(handle))
#define TO_CVK_DESCRIPTORSET(handle) (dynamic_cast<const VulkanDescriptorSet*>(handle))
#define TO_VK_FRAMEBUFFER(handle)    reinterpret_cast<VulkanFramebuffer*>((handle).value)
#define TO_VK_RENDER_PASS(handle)    reinterpret_cast<VkRenderPass>((handle).value)
#define TO_VK_SHADER(handle)         (dynamic_cast<VulkanShader*>(handle))
#define TO_CVK_SHADER(handle)        (dynamic_cast<const VulkanShader*>(handle))
#define TO_VK_SAMPLER(sampler)       (dynamic_cast<VulkanSampler*>(sampler))

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

VkClearColorValue ToVkClearColor(const RHIRenderTargetClearValue& clearValue);

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderTargetClearValue& clearValue);

void ToVkClearColor(const Color& color, VkClearColorValue* colorValue);

void ToVkImageSubresourceRange(const RHITextureSubResourceRange& range,
                               VkImageSubresourceRange* vkRange);

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers,
                                VkImageSubresourceLayers* vkLayers);

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* copy);

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* copy);
} // namespace zen