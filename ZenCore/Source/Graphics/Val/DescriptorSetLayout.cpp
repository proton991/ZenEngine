#include "Graphics/Val/DescriptorSetLayout.h"
#include "Common/Errors.h"
#include "Graphics/Val/Shader.h"

namespace zen::val
{
static VkDescriptorType ConvertToVkDescriptorType(ShaderResourceType type, bool isDynamic)
{
    VkDescriptorType result = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    switch (type)
    {
        case ShaderResourceType::InputAttachment:
            result = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            break;
        case ShaderResourceType::Image:
            result = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case ShaderResourceType::ImageSampler:
            result = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            break;
        case ShaderResourceType::ImageStorage:
            result = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        case ShaderResourceType::Sampler:
            result = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case ShaderResourceType::BufferUniform:
            if (isDynamic)
            {
                result = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            }
            else
            {
                result = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            break;
        case ShaderResourceType::BufferStorage:
            if (isDynamic)
            {
                result = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            }
            else
            {
                result = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            break;
        default:
            LOG_FATAL_ERROR("No conversion possible for the shader resource type.");
            break;
    }

    return result;
}

DescriptorSetLayout::DescriptorSetLayout(Device& device, const uint32_t setIndex, const std::vector<ShaderResource>& shaderResources) :
    DeviceObject(device), m_setIndex(setIndex)
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (auto& resource : shaderResources)
    {
        // Skip shader resources without a binding point
        if (resource.type == ShaderResourceType::Input ||
            resource.type == ShaderResourceType::Output ||
            resource.type == ShaderResourceType::PushConstant ||
            resource.type == ShaderResourceType::SpecializationConstant)
        {
            continue;
        }
        // TODO: Add more resource mode support (update after bind)
        VkDescriptorSetLayoutBinding layoutBinding{};

        layoutBinding.binding         = resource.binding;
        layoutBinding.descriptorCount = resource.arraySize;
        layoutBinding.descriptorType  = ConvertToVkDescriptorType(resource.type, resource.mode == ShaderResourceMode::Dynamic);
        layoutBinding.stageFlags      = static_cast<VkShaderStageFlags>(resource.stages);

        bindings.emplace_back(layoutBinding);
    }
    m_handle = DSLayoutCache::GetInstance(m_device).RequestDSLayout(bindings);
}

VkDescriptorSetLayout DSLayoutCache::RequestDSLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    const size_t hashVal = Hash(bindings);
    auto         it      = m_cache.find(hashVal);
    if (it != m_cache.end())
    {
        return it->second;
    }

    VkDescriptorSetLayout dsLayout = VK_NULL_HANDLE;

    VkDescriptorSetLayoutCreateInfo dsLayoutCI{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsLayoutCI.bindingCount = static_cast<uint32_t>(bindings.size());
    dsLayoutCI.pBindings    = bindings.data();
    CHECK_VK_ERROR_AND_THROW(vkCreateDescriptorSetLayout(m_device.GetHandle(), &dsLayoutCI, nullptr, &dsLayout), "Failed to create descriptor layout");
    if (dsLayout != VK_NULL_HANDLE)
    {
        m_cache[hashVal] = dsLayout;
    }
    return dsLayout;
}

DSLayoutCache::~DSLayoutCache()
{
    // destroy layouts
    for (auto& it : m_cache)
    {
        vkDestroyDescriptorSetLayout(m_device.GetHandle(), it.second, nullptr);
    }
}

size_t DSLayoutCache::Hash(const std::vector<VkDescriptorSetLayoutBinding>& bindings)
{
    size_t result = std::hash<size_t>()(bindings.size());
    for (const VkDescriptorSetLayoutBinding& b : bindings)
    {
        //pack the binding data into a single int64. Not fully correct but its ok
        size_t bindingHash = b.binding | b.descriptorType << 8 | b.descriptorCount << 16 | b.stageFlags << 24;
        //shuffle the packed binding data and xor it with the main hash
        result ^= std::hash<size_t>()(bindingHash);
    }

    return result;
}


} // namespace zen::val