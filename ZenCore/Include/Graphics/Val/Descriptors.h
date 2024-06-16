#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>

namespace zen::val
{
class Device;
struct VulkanDescriptorPoolSizes;

class DescriptorPoolManager
{
public:
    DescriptorPoolManager(const Device& device,
                          const VulkanDescriptorPoolSizes& poolSizes,
                          bool allowFree);

    void Cleanup();

    void ResetPools();

    VkDescriptorPool GrabPool();

private:
    VkDescriptorPool CreatePool();

    const Device& m_device;
    const uint32_t m_maxSets;
    const bool m_allowFree;

    const std::vector<VkDescriptorPoolSize> m_poolSizes;

    std::vector<VkDescriptorPool> m_freePools;
    std::vector<VkDescriptorPool> m_usedPools;
    std::mutex m_mutex;
};

class DescriptorSetAllocator
{
public:
    DescriptorSetAllocator(const Device& device, DescriptorPoolManager& poolManager);

    VkDescriptorSet Allocate(const VkDescriptorSetLayout* layout);

    bool Allocate(VkDescriptorSetLayout* layout, uint32_t count, VkDescriptorSet* outSet);

private:
    const Device& m_device;
    DescriptorPoolManager& m_poolManager;
    VkDescriptorPool m_currentPool{VK_NULL_HANDLE};
    std::mutex m_mutex;
};
} // namespace zen::val