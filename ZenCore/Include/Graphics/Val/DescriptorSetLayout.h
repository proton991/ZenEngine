#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "Common/ObjectBase.h"
#include "DeviceObject.h"

namespace zen::val
{
struct ShaderResource;
enum class ShaderResourceType;
VkDescriptorType ConvertToVkDescriptorType(ShaderResourceType type, bool isDynamic);

class DescriptorSetLayout :
    public DeviceObject<VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT>
{
public:
    DescriptorSetLayout(const Device& device,
                        uint32_t setIndex,
                        const std::vector<ShaderResource>& shaderResources);

    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;

    auto GetHash() const
    {
        return m_hash;
    }

    auto GetSetIndex() const
    {
        return m_setIndex;
    }

private:
    const uint32_t m_setIndex;
    size_t m_hash{};
};
} // namespace zen::val