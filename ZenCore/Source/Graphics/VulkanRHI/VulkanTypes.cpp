#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Common/Errors.h"

namespace zen::rhi
{
VkDescriptorType ShaderResourceTypeToVkDescriptorType(ShaderResourceType shaderResourceType)
{
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    switch (shaderResourceType)
    {
        case ShaderResourceType::eSampler:
            //
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case ShaderResourceType::eTexture:
            //
            type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case ShaderResourceType::eSamplerWithTexture:
            type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            break;
        case ShaderResourceType::eImage:
            //
            type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        case ShaderResourceType::eTextureBuffer:
        case ShaderResourceType::eSamplerWithTextureBuffer:
            type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            break;
        case ShaderResourceType::eImageBuffer:
            type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            break;
        case ShaderResourceType::eUniformBuffer:
            //
            type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            break;
        case ShaderResourceType::eStorageBuffer:
            //
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            break;
        case ShaderResourceType::eInputAttachment:
            //
            type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            break;
        default: LOGE("Invalid ShaderResource Type"); break;
    }
    return type;
}

VkShaderStageFlagBits ShaderStageToVkShaderStageFlagBits(ShaderStage stage)
{
    switch (stage)
    {
        case ShaderStage::eVertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::eTesselationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::eTesselationEvaluation:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::eGeometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::eFragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::eCompute: return VK_SHADER_STAGE_COMPUTE_BIT;
        default: return VK_SHADER_STAGE_ALL;
    }
}

VkShaderStageFlags ShaderStageFlagsBitsToVkShaderStageFlags(
    BitField<ShaderStageFlagBits> stageFlags)
{
    VkShaderStageFlags flags{};
    for (uint32_t k = 0; k < ToUnderlying(ShaderStage::eMax); k++)
    {
        ShaderStage stage = static_cast<ShaderStage>(k);
        if (stageFlags.HasFlag(ShaderStageToFlagBits(stage)))
        {
            flags |= ShaderStageToVkShaderStageFlagBits(stage);
        }
    }
    return flags;
}

VkPrimitiveTopology ToVkPrimitiveTopology(DrawPrimitiveType type)
{
    return static_cast<VkPrimitiveTopology>(type);
}

VkCullModeFlags ToVkCullModeFlags(PolygonCullMode mode)
{
    return static_cast<VkCullModeFlags>(mode);
}

VkFrontFace ToVkFrontFace(PolygonFrontFace frontFace)
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

VkCompareOp ToVkCompareOp(CompareOperator op)
{
    return static_cast<VkCompareOp>(op);
}

VkStencilOp ToVkStencilOp(StencilOperation op)
{
    return static_cast<VkStencilOp>(op);
}

VkLogicOp ToVkLogicOp(LogicOperation op)
{
    return static_cast<VkLogicOp>(op);
}

VkBlendOp ToVkBlendOp(BlendOperation op)
{
    return static_cast<VkBlendOp>(op);
}

VkBlendFactor ToVkBlendFactor(BlendFactor factor)
{
    return static_cast<VkBlendFactor>(factor);
}

VkDynamicState ToVkDynamicState(DynamicState state)
{
    return static_cast<VkDynamicState>(state);
}

VkImageType ToVkImageType(TextureType type)
{
    return static_cast<VkImageType>(type);
}

VkImageViewType ToVkImageViewType(TextureType type)
{
    switch (type)
    {
        case TextureType::e1D: return VK_IMAGE_VIEW_TYPE_1D;
        case TextureType::e2D: return VK_IMAGE_VIEW_TYPE_2D;
        case TextureType::e3D: return VK_IMAGE_VIEW_TYPE_3D;
        case TextureType::eCube: return VK_IMAGE_VIEW_TYPE_CUBE;
        case TextureType::eMax: return VK_IMAGE_VIEW_TYPE_MAX_ENUM;
    }
}

VkImageUsageFlags ToVkImageUsageFlags(BitField<TextureUsageFlagBits> flagBits)
{
    VkImageUsageFlags flags{};
    if (flagBits.HasFlag(TextureUsageFlagBits::eTransferSrc))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eTransferDst))
    {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eSampled))
    {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eStorage))
    {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eColorAttachment))
    {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eDepthStencilAttachment))
    {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eTransientAttachment))
    {
        flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    }
    if (flagBits.HasFlag(TextureUsageFlagBits::eInputAttachment))
    {
        flags |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }
    return flags;
}

VkBufferUsageFlags ToVkBufferUsageFlags(BitField<BufferUsageFlagBits> flags)
{
    VkBufferUsageFlags vkFlags{};
    if (flags.HasFlag(BufferUsageFlagBits::eTransferSrcBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eTransferDstBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eTextureBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eImageBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eUniformBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eStorageBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eIndexBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eVertexBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if (flags.HasFlag(BufferUsageFlagBits::eIndirectBuffer))
    {
        vkFlags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    return vkFlags;
}

VkFormat ToVkFormat(DataFormat format)
{
    return static_cast<VkFormat>(format);
}

VkAttachmentLoadOp ToVkAttachmentLoadOp(RenderTargetLoadOp loadOp)
{
    return static_cast<VkAttachmentLoadOp>(loadOp);
}

VkAttachmentStoreOp ToVkAttachmentStoreOp(RenderTargetStoreOp storeOp)
{
    return static_cast<VkAttachmentStoreOp>(storeOp);
}

VkImageLayout ToVkImageLayout(TextureLayout layout)
{
    switch (layout)
    {
        case TextureLayout::eUndefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case TextureLayout::eGeneral: return VK_IMAGE_LAYOUT_GENERAL;
        case TextureLayout::eColorTarget: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case TextureLayout::eDepthStencilTarget:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        case TextureLayout::eShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case TextureLayout::eTransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureLayout::eTransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VkAccessFlags ToVkAccessFlags(BitField<AccessFlagBits> access)
{
    return static_cast<VkAccessFlags>(access);
}

VkImageAspectFlags ToVkAspectFlags(BitField<TextureAspectFlagBits> aspect)
{
    return static_cast<VkImageAspectFlags>(aspect);
}

VkFilter ToVkFilter(SamplerFilter filter)
{
    return static_cast<VkFilter>(filter);
}

VkSamplerAddressMode ToVkSamplerAddressMode(SamplerRepeatMode mode)
{
    return static_cast<VkSamplerAddressMode>(mode);
}


VkBorderColor ToVkBorderColor(SamplerBorderColor color)
{
    return static_cast<VkBorderColor>(color);
}

VkClearColorValue ToVkClearColor(const RenderPassClearValue& clearValue)
{

    VkClearColorValue colorValue{};
    colorValue.float32[0] = clearValue.color.r;
    colorValue.float32[1] = clearValue.color.g;
    colorValue.float32[2] = clearValue.color.b;
    colorValue.float32[3] = clearValue.color.a;

    return colorValue;
}

VkClearDepthStencilValue ToVkClearDepthStencil(const RenderPassClearValue& clearValue)
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

void ToVkImageSubresourceRange(const TextureSubResourceRange& range,
                               VkImageSubresourceRange* vkRange)
{
    *vkRange                = {};
    vkRange->aspectMask     = ToVkAspectFlags(range.aspect);
    vkRange->layerCount     = range.layerCount;
    vkRange->levelCount     = range.levelCount;
    vkRange->baseArrayLayer = range.baseArrayLayer;
    vkRange->baseMipLevel   = range.baseMipLevel;
}

void ToVkImageSubresourceLayers(const TextureSubresourceLayers& layers,
                                VkImageSubresourceLayers* vkLayers)
{
    *vkLayers                = {};
    vkLayers->aspectMask     = ToVkAspectFlags(layers.aspect);
    vkLayers->layerCount     = layers.layerCount;
    vkLayers->mipLevel       = layers.mipmap;
    vkLayers->baseArrayLayer = layers.baseArrayLayer;
}

void ToVkImageCopy(const TextureCopyRegion& region, VkImageCopy* copy)
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

void ToVkBufferImageCopy(const BufferTextureCopyRegion& region, VkBufferImageCopy* copy)
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
} // namespace zen::rhi