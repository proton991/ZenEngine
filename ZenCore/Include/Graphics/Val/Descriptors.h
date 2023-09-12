#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include <mutex>
#include <unordered_map>

namespace zen::val
{
class Device;
struct VulkanDescriptorPoolSizes;

class DescriptorPoolManager
{
public:
    DescriptorPoolManager(Device& device, const VulkanDescriptorPoolSizes& poolSizes, bool allowFree);

    void Cleanup();

    void ResetPools();

    VkDescriptorPool GrabPool();

private:
    VkDescriptorPool CreatePool();

    const Device&  m_device;
    const uint32_t m_maxSets;
    const bool     m_allowFree;

    const std::vector<VkDescriptorPoolSize> m_poolSizes;

    std::vector<VkDescriptorPool> m_freePools;
    std::vector<VkDescriptorPool> m_usedPools;
    std::mutex                    m_mutex;
};

class DescriptorSetAllocator
{
public:
    DescriptorSetAllocator(Device& device, DescriptorPoolManager& poolManager);

    VkDescriptorSet Allocate(const VkDescriptorSetLayout* layout);

    bool Allocate(const VkDescriptorSetLayout* layout, uint32_t count, VkDescriptorSet* outSet);

private:
    Device&                m_device;
    DescriptorPoolManager& m_poolManager;
    VkDescriptorPool       m_currentPool{VK_NULL_HANDLE};
    std::mutex             m_mutex;
};

class DescriptorWriter
{
public:
    explicit DescriptorWriter(VkDescriptorSet ds);

    void BindBuffer(uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, uint32_t descriptorCount = 1);

    void BindImage(uint32_t binding, VkDescriptorType type, VkSampler sampler, VkImageView imageView, VkImageLayout imageLayout, uint32_t descriptorCount = 1);

    void ApplyWrites(VkDevice vkDevice);

private:
    VkDescriptorSet                   m_descriptorSet;
    std::vector<VkWriteDescriptorSet> m_writes;
};

} // namespace zen::val