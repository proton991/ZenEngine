#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Utils/Errors.h"

namespace zen
{
VkDescriptorType ShaderResourceTypeToVkDescriptorType(RHIShaderResourceType shaderResourceType)
{
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    switch (shaderResourceType)
    {
        case RHIShaderResourceType::eSampler:
            //
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case RHIShaderResourceType::eTexture:
            //
            type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case RHIShaderResourceType::eSamplerWithTexture:
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            break;
        case RHIShaderResourceType::eImage:
            //
            type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        case RHIShaderResourceType::eTextureBuffer:
        case RHIShaderResourceType::eSamplerWithTextureBuffer:
            type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            break;
        case RHIShaderResourceType::eImageBuffer:
            type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            break;
        case RHIShaderResourceType::eUniformBuffer:
            //
            type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            break;
        case RHIShaderResourceType::eStorageBuffer:
            //
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            break;
        case RHIShaderResourceType::eInputAttachment:
            //
            type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            break;
        default: LOGE("Invalid ShaderResource Type"); break;
    }
    return type;
}

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(RHIShaderStage stage)
{
    switch (stage)
    {
        case RHIShaderStage::eVertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case RHIShaderStage::eTesselationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case RHIShaderStage::eTesselationEvaluation:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case RHIShaderStage::eGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case RHIShaderStage::eFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case RHIShaderStage::eCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: return VK_SHADER_STAGE_ALL;
    }
}

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(
    BitField<RHIShaderStageFlagBits> stageFlags)
{
    VkShaderStageFlags flags{};
    for (uint32_t k = 0; k < ToUnderlying(RHIShaderStage::eMax); k++)
    {
        RHIShaderStage stage = static_cast<RHIShaderStage>(k);
        if (stageFlags.HasFlag(RHIShaderStageToFlagBits(stage)))
        {
            flags |= ShaderStageToVkShaderStageFlagBits(stage);
        }
    }
    return flags;
}

VkPrimitiveTopology ToVkPrimitiveTopology(RHIDrawPrimitiveType type)
{
    return static_cast<VkPrimitiveTopology>(type);
}

VkCullModeFlags ToVkCullModeFlags(RHIPolygonCullMode mode)
{
    return static_cast<VkCullModeFlags>(mode);
}

VkFrontFace ToVkFrontFace(RHIPolygonFrontFace frontFace)
{
    return static_cast<VkFrontFace>(frontFace);
}

VkSampleCountFlagBits ToVkSampleCountFlagBits(SampleCount count)
{
    switch (count)
    {
        case SampleCount::e1: return VK_SAMPLE_COUNT_1_BIT;
        case SampleCount::e2: return VK_SAMPLE_COUNT_2_BIT;
        case SampleCount::e4: return VK_SAMPLE_COUNT_4_BIT;
        case SampleCount::e8: return VK_SAMPLE_COUNT_8_BIT;
        case SampleCount::e16: return VK_SAMPLE_COUNT_16_BIT;
        case SampleCount::e32: return VK_SAMPLE_COUNT_32_BIT;
        case SampleCount::e64: return VK_SAMPLE_COUNT_64_BIT;
        default:
        case SampleCount::eMax: return VK_SAMPLE_COUNT_FLAG_BITS_MAX_ENUM;
    }
}

VkCompareOp ToVkCompareOp(RHIDepthCompareOperator op)
{
    return static_cast<VkCompareOp>(op);
}

VkStencilOp ToVkStencilOp(RHIStencilOp op)
{
    return static_cast<VkStencilOp>(op);
}

VkLogicOp ToVkLogicOp(RHIBlendLogicOp op)
{
    return static_cast<VkLogicOp>(op);
}

VkBlendOp ToVkBlendOp(RHIBlendOp op)
{
    return static_cast<VkBlendOp>(op);
}

VkBlendFactor ToVkBlendFactor(RHIBlendFactor factor)
{
    return static_cast<VkBlendFactor>(factor);
}

VkDynamicState ToVkDynamicState(RHIDynamicState state)
{
    return static_cast<VkDynamicState>(state);
}

VkImageType ToVkImageType(RHITextureType type)
{
    return static_cast<VkImageType>(type);
}

VkImageViewType ToVkImageViewType(RHITextureType type)
{
    switch (type)
    {
        case RHITextureType::e1D: return VK_IMAGE_VIEW_TYPE_1D;
        case RHITextureType::e2D: return VK_IMAGE_VIEW_TYPE_2D;
        case RHITextureType::e3D: return VK_IMAGE_VIEW_TYPE_3D;
        case RHITextureType::eCube: return VK_IMAGE_VIEW_TYPE_CUBE;
        case RHITextureType::eMax: return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    }
}

VkImageUsageFlags ToVkImageUsageFlags(BitField<RHITextureUsageFlagBits> flagBits)
{
    VkImageUsageFlags flags{};
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransferSrc))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransferDst))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eSampled))
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eStorage))
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eColorAttachment))
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eDepthStencilAttachment))
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eTransientAttachment))
    {
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(RHITextureUsageFlagBits::eInputAttachment))
    {
        flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    return flags;
}

VkBufferUsageFlags ToVkBufferUsageFlags(BitField<RHIBufferUsageFlagBits> flags)
{
    VkBufferUsageFlags vkFlags{};
    if (flags.HasFlag(RHIBufferUsageFlagBits::eTransferSrcBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eTransferDstBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eTextureBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eImageBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eUniformBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eStorageBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eIndexBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eVertexBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (flags.HasFlag(RHIBufferUsageFlagBits::eIndirectBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    return vkFlags;
}

VkFormat ToVkFormat(DataFormat format)
{
    return static_cast<VkFormat>(format);
}

VkAttachmentLoadOp ToVkAttachmentLoadOp(RHIRenderTargetLoadOp loadOp)
{
    return static_cast<VkAttachmentLoadOp>(loadOp);
}

VkAttachmentStoreOp ToVkAttachmentStoreOp(RHIRenderTargetStoreOp storeOp)
{
    return static_cast<VkAttachmentStoreOp>(storeOp);
}

VkImageLayout ToVkImageLayout(RHITextureLayout layout)
{
    switch (layout)
    {
        case RHITextureLayout::eUndefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case RHITextureLayout::eGeneral: return VK_IMAGE_LAYOUT_GENERAL;
        case RHITextureLayout::eColorTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case RHITextureLayout::eDepthStencilTarget:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case RHITextureLayout::eShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case RHITextureLayout::eTransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case RHITextureLayout::eTransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VkAccessFlags ToVkAccessFlags(BitField<RHIAccessFlagBits> access)
{
    return static_cast<VkAccessFlags>(access);
}

VkImageAspectFlags ToVkAspectFlags(BitField<RHITextureAspectFlagBits> aspect)
{
    return static_cast<VkImageAspectFlags>(aspect);
}

VkFilter ToVkFilter(RHISamplerFilter filter)
{
    return static_cast<VkFilter>(filter);
}

VkSamplerAddressMode ToVkSamplerAddressMode(RHISamplerRepeatMode mode)
{
    return static_cast<VkSamplerAddressMode>(mode);
}


VkBorderColor ToVkBorderColor(RHISamplerBorderColor color)
{
    return static_cast<VkBorderColor>(color);
}

VkClearColorValue ToVkClearColor(const RHIRenderPassClearValue& clearValue)
{

    VkClearColorValue colorValue{};
    colorValue.float32[0] = clearValue.color.r;
    colorValue.float32[1] = clearValue.color.g;
    colorValue.float32[2] = clearValue.color.b;
    colorValue.float32[3] = clearValue.color.a;

    return colorValue;
}

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderPassClearValue& clearValue)
{
    VkClearDepthStencilValue depthStencilValue{};
    depthStencilValue.depth   = clearValue.depth;
    depthStencilValue.stencil = clearValue.stencil;

    return depthStencilValue;
}

VkClearColorValue ToVkClearColor(const RHIRenderTargetClearValue& clearValue)
{
    VkClearColorValue colorValue{};
    colorValue.float32[0] = clearValue.color.r;
    colorValue.float32[1] = clearValue.color.g;
    colorValue.float32[2] = clearValue.color.b;
    colorValue.float32[3] = clearValue.color.a;

    return colorValue;
}

VkClearDepthStencilValue ToVkClearDepthStencil(const RHIRenderTargetClearValue& clearValue)
{
    VkClearDepthStencilValue depthStencilValue{};
    depthStencilValue.depth   = clearValue.depth;
    depthStencilValue.stencil = clearValue.stencil;

    return depthStencilValue;
}

void ToVkClearColor(const Color& color, VkClearColorValue* colorValue)
{
    *colorValue            = {};
    colorValue->float32[0] = color.r;
    colorValue->float32[1] = color.g;
    colorValue->float32[2] = color.b;
    colorValue->float32[3] = color.a;
}

void ToVkImageSubresourceRange(const RHITextureSubResourceRange& range,
                               VkImageSubresourceRange* vkRange)
{
    *vkRange                = {};
    vkRange->aspectMask     = ToVkAspectFlags(range.aspect);
    vkRange->layerCount     = range.layerCount;
    vkRange->levelCount     = range.levelCount;
    vkRange->baseArrayLayer = range.baseArrayLayer;
    vkRange->baseMipLevel   = range.baseMipLevel;
}

void ToVkImageSubresourceLayers(const RHITextureSubresourceLayers& layers,
                                VkImageSubresourceLayers* vkLayers)
{
    *vkLayers                = {};
    vkLayers->aspectMask     = ToVkAspectFlags(layers.aspect);
    vkLayers->layerCount     = layers.layerCount;
    vkLayers->mipLevel       = layers.mipmap;
    vkLayers->baseArrayLayer = layers.baseArrayLayer;
}

void ToVkImageCopy(const RHITextureCopyRegion& region, VkImageCopy* copy)
{
    *copy = {};
    ToVkImageSubresourceLayers(region.srcSubresources, &copy->srcSubresource);
    ToVkImageSubresourceLayers(region.dstSubresources, &copy->dstSubresource);
    copy->srcOffset.x   = region.srcOffset.x;
    copy->srcOffset.y   = region.srcOffset.y;
    copy->srcOffset.z   = region.srcOffset.z;
    copy->dstOffset.x   = region.dstOffset.x;
    copy->dstOffset.y   = region.dstOffset.y;
    copy->dstOffset.z   = region.dstOffset.z;
    copy->extent.width  = region.size.x;
    copy->extent.height = region.size.y;
    copy->extent.depth  = region.size.z;
}

void ToVkBufferImageCopy(const RHIBufferTextureCopyRegion& region, VkBufferImageCopy* copy)
{
    *copy = {};
    ToVkImageSubresourceLayers(region.textureSubresources, &copy->imageSubresource);
    copy->bufferOffset       = region.bufferOffset;
    copy->imageOffset.x      = region.textureOffset.x;
    copy->imageOffset.y      = region.textureOffset.y;
    copy->imageOffset.z      = region.textureOffset.z;
    copy->imageExtent.width  = region.textureSize.x;
    copy->imageExtent.height = region.textureSize.y;
    copy->imageExtent.depth  = region.textureSize.z;
}
} // namespace zen