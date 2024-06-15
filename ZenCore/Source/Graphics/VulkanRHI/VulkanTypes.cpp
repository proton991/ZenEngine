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
} // namespace zen::rhi