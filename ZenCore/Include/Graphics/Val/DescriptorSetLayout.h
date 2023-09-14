#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include "Common/ObjectBase.h"
#include "DeviceObject.h"

namespace zen::val
{
struct ShaderResource;

class DescriptorSetLayout : public DeviceObject<VkDescriptorSetLayout, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT>
{
public:
    DescriptorSetLayout(Device& device, uint32_t setIndex, const std::vector<ShaderResource>& shaderResources);
    
private:
    const uint32_t m_setIndex;
};

class DSLayoutCache
{
public:
    ZEN_SINGLETON(DSLayoutCache)

    static DSLayoutCache& GetInstance(Device& device)
    {
        static DSLayoutCache dsLayoutCache{device};
        return dsLayoutCache;
    }

    VkDescriptorSetLayout RequestDSLayout(const std::vector<VkDescriptorSetLayoutBinding>& bindings);

    ~DSLayoutCache();

private:
    explicit DSLayoutCache(Device& device) :
        m_device(device) {}
    Device& m_device;
    size_t  Hash(const std::vector<VkDescriptorSetLayoutBinding>& bindings);
    // DSLayout cache
    std::unordered_map<size_t, VkDescriptorSetLayout> m_cache;
};

} // namespace zen::val