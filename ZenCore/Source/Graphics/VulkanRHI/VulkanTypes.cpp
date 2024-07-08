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
        case ShaderStage::eTesselationConrol: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::eTesselationEvaluation:
            return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
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
    return static_cast<VkSampleCountFlagBits>(count);
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
        case TextureLayout::eDepthStencilTarget: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        case TextureLayout::eShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case TextureLayout::eTransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case TextureLayout::eTransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        default: return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}
} // namespace zen::rhi