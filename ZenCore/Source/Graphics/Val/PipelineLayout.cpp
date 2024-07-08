#include "Graphics/Val/PipelineLayout.h"
#include "Graphics/Val/DescriptorSetLayout.h"
#include "Graphics/Val/VulkanStrings.h"
#include "Common/Errors.h"

namespace zen::val
{
PipelineLayout::PipelineLayout(const Device& device,
                               const std::vector<ShaderModule*>& shaderModules) :
    DeviceObject(device), m_shaderModules(shaderModules)
{
    for (auto* shaderModule : shaderModules)
    {
        for (const auto& shaderResource : shaderModule->GetResources())
        {
            std::string key = shaderResource.name;
            if (shaderResource.type == ShaderResourceType::Input ||
                shaderResource.type == ShaderResourceType::Output)
            {
                key = VkToString(shaderResource.stages).append("_").append(key);
            }
            if (!m_shaderResources.count(key))
            {
                m_shaderResources.emplace(key, shaderResource);
            }
            else
            {
                if (m_shaderResources[key].stages != shaderResource.stages)
                {
                    m_shaderResources[key].stages |= shaderResource.stages;
                }
            }
        }
    }
    for (auto& it : m_shaderResources)
    {
        auto& shaderResource = it.second;
        const auto set       = shaderResource.set;
        if (!m_perSetResource.count(set))
        {
            m_perSetResource.emplace(set, std::vector<ShaderResource>{shaderResource});
        }
        else
        {
            m_perSetResource[set].push_back(shaderResource);
        }
    }
    std::vector<VkDescriptorSetLayout> dsLayoutHandles;
    for (auto& it : m_perSetResource)
    {
        auto dsLayout = DescriptorSetLayout(m_device, it.first, it.second);
        if (dsLayout.GetHandle() != VK_NULL_HANDLE)
        {
            // Collect all the descriptor set layout handles, maintaining set order
            dsLayoutHandles.push_back(dsLayout.GetHandle());
            m_dsLayouts.emplace_back(std::move(dsLayout));
        }
    }

    std::vector<VkPushConstantRange> pushConstantRanges;
    for (auto& resource : GetResources(ShaderResourceType::PushConstant))
    {
        VkPushConstantRange pcr{};
        pcr.stageFlags = resource.stages;
        pcr.size       = 128;
        pcr.offset     = 0;
        pushConstantRanges.push_back(pcr);
    }

    VkPipelineLayoutCreateInfo pipelineCI{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};

    pipelineCI.setLayoutCount         = static_cast<uint32_t>(dsLayoutHandles.size());
    pipelineCI.pSetLayouts            = dsLayoutHandles.data();
    pipelineCI.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
    pipelineCI.pPushConstantRanges    = pushConstantRanges.data();

    // Create the Vulkan pipeline layout handle
    CHECK_VK_ERROR_AND_THROW(
        vkCreatePipelineLayout(m_device.GetHandle(), &pipelineCI, nullptr, &m_handle),
        "Failed to create pipeline layout");
}

PipelineLayout::PipelineLayout(PipelineLayout&& other) noexcept : DeviceObject(std::move(other))
{
    m_perSetResource  = std::move(other.m_perSetResource);
    m_shaderResources = std::move(other.m_shaderResources);
    m_shaderModules   = std::move(other.m_shaderModules);
    m_dsLayouts       = std::move(other.m_dsLayouts);
}

std::vector<ShaderResource> PipelineLayout::GetResources(ShaderResourceType type,
                                                         VkShaderStageFlagBits stage) const
{
    std::vector<ShaderResource> result;

    for (auto& it : m_shaderResources)
    {
        auto& shader_resource = it.second;

        if (shader_resource.type == type || type == ShaderResourceType::All)
        {
            if (shader_resource.stages == stage || stage == VK_SHADER_STAGE_ALL)
            {
                result.push_back(shader_resource);
            }
        }
    }

    return result;
}

PipelineLayout::~PipelineLayout()
{
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(m_device.GetHandle(), m_handle, nullptr);
    }
}
} // namespace zen::val