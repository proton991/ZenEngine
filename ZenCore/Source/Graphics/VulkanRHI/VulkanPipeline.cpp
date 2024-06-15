#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
#include <vulkan/vulkan.h>
#include <algorithm> // For std::min

static uint32_t DecideDescriptorCount(
    VkDescriptorType type,
    uint32_t count,
    const VkPhysicalDeviceDescriptorIndexingProperties& properties)
{
    switch (type)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindSamplers);
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindSampledImages);
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageImages);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return std::min(count, properties.maxPerStageDescriptorUpdateAfterBindInputAttachments);
        default:
            // If the descriptor type is unknown, return the original count
            return count;
    }
}


ShaderHandle VulkanRHI::CreateShader(const ShaderGroupInfo& sgInfo)
{
    VulkanShader* shader = m_shaderAllocator.Alloc();

    // Create specialization info from tracked state. This is shared by all shaders.
    const auto& specConstants = sgInfo.specializationConstants;
    if (!specConstants.empty())
    {
        shader->entries.resize(specConstants.size());
        for (uint32_t i = 0; i < specConstants.size(); i++)
        {
            VkSpecializationMapEntry& entry = shader->entries[i];
            // fill in data
            entry.constantID = specConstants[i].constantId;
            entry.size       = sizeof(uint32_t);
            entry.offset     = reinterpret_cast<const char*>(&specConstants[i].intValue) -
                reinterpret_cast<const char*>(specConstants.data());
        }
    }
    shader->specializationInfo.mapEntryCount = static_cast<uint32_t>(shader->entries.size());
    shader->specializationInfo.pMapEntries =
        shader->entries.empty() ? nullptr : shader->entries.data();
    shader->specializationInfo.dataSize =
        specConstants.size() * sizeof(ShaderSpecializationConstant);
    shader->specializationInfo.pData = specConstants.empty() ? nullptr : specConstants.data();

    for (uint32_t i = 0; i < sgInfo.shaderStages.size(); i++)
    {
        VkShaderModuleCreateInfo shaderModuleCI;
        InitVkStruct(shaderModuleCI, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        shaderModuleCI.codeSize = sgInfo.sprivCode[i].size();
        shaderModuleCI.pCode    = reinterpret_cast<const uint32_t*>(sgInfo.sprivCode[i].data());
        VkShaderModule module{VK_NULL_HANDLE};
        VKCHECK(vkCreateShaderModule(GetVkDevice(), &shaderModuleCI, nullptr, &module));

        VkPipelineShaderStageCreateInfo pipelineShaderStageCI;
        InitVkStruct(pipelineShaderStageCI, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        pipelineShaderStageCI.stage  = ShaderStageToVkShaderStageFlagBits(sgInfo.shaderStages[i]);
        pipelineShaderStageCI.pName  = "main";
        pipelineShaderStageCI.module = module;
        pipelineShaderStageCI.pSpecializationInfo = &shader->specializationInfo;

        shader->stageCreateInfos.push_back(pipelineShaderStageCI);
    }
    // Create descriptorSetLayouts
    std::vector<std::vector<VkDescriptorSetLayoutBinding>> dsBindings;
    const auto setCount = sgInfo.SRDs.size();
    dsBindings.resize(setCount);
    for (uint32_t i = 0; i < setCount; i++)
    {
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        // collect bindings for set i
        for (uint32_t j = 0; j < sgInfo.SRDs[i].size(); j++)
        {
            const ShaderResourceDescriptor& srd = sgInfo.SRDs[i][j];
            VkDescriptorSetLayoutBinding binding{};
            binding.binding         = srd.binding;
            binding.descriptorType  = ShaderResourceTypeToVkDescriptorType(srd.type);
            binding.descriptorCount = DecideDescriptorCount(
                binding.descriptorType, srd.arraySize, m_device->GetDescriptorIndexingProperties());
            binding.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(srd.stageFlags);
            if (binding.descriptorCount > 1)
            {
                bindingFlags.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
            }
            else { bindingFlags.push_back(0); }
            dsBindings[i].push_back(binding);
        }
        if (dsBindings[i].empty()) continue;
        // create descriptor set layout for set i
        VkDescriptorSetLayoutCreateInfo layoutCI;
        InitVkStruct(layoutCI, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layoutCI.bindingCount = dsBindings[i].size();
        layoutCI.pBindings    = dsBindings[i].data();
        layoutCI.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        // set binding flags
        VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo;
        InitVkStruct(bindingFlagsCreateInfo,
                     VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
        bindingFlagsCreateInfo.bindingCount  = bindingFlags.size();
        bindingFlagsCreateInfo.pBindingFlags = bindingFlags.data();
        layoutCI.pNext                       = &bindingFlagsCreateInfo;

        VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
        VKCHECK(
            vkCreateDescriptorSetLayout(GetVkDevice(), &layoutCI, nullptr, &descriptorSetLayout));
        shader->descriptorSetLayouts.push_back(descriptorSetLayout);
    }

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutCI;
    InitVkStruct(pipelineLayoutCI, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pipelineLayoutCI.pSetLayouts    = shader->descriptorSetLayouts.data();
    pipelineLayoutCI.setLayoutCount = setCount;
    VkPushConstantRange prc{};
    const auto& pc = sgInfo.pushConstants;
    if (pc.size > 0)
    {
        prc.stageFlags = ShaderStageFlagsBitsToVkShaderStageFlags(pc.stageFlags);
        prc.size       = pc.size;
        pipelineLayoutCI.pushConstantRangeCount = 1;
        pipelineLayoutCI.pPushConstantRanges    = &prc;
    }
    vkCreatePipelineLayout(GetVkDevice(), &pipelineLayoutCI, nullptr, &shader->pipelineLayout);

    return ShaderHandle(shader);
}

void VulkanRHI::DestroyShader(ShaderHandle shaderHandle)
{
    VulkanShader* shader = reinterpret_cast<VulkanShader*>(shaderHandle.value);

    for (auto& dsLayout : shader->descriptorSetLayouts)
    {
        vkDestroyDescriptorSetLayout(GetVkDevice(), dsLayout, nullptr);
    }

    vkDestroyPipelineLayout(GetVkDevice(), shader->pipelineLayout, nullptr);

    for (auto& stageCreateInfo : shader->stageCreateInfos)
    {
        vkDestroyShaderModule(GetVkDevice(), stageCreateInfo.module, nullptr);
    }

    m_shaderAllocator.Free(shader);
}

} // namespace zen::rhi