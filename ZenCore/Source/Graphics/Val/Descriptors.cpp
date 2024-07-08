#include "Graphics/Val/Descriptors.h"
#include "Graphics/Val/Device.h"
#include "Graphics/Val/VulkanConfig.h"
#include "Common/Errors.h"

namespace zen::val
{
DescriptorPoolManager::DescriptorPoolManager(const Device& device,
                                             const VulkanDescriptorPoolSizes& poolSizes,
                                             bool allowFree) :
    m_device(device),
    m_maxSets(poolSizes.maxDescriptorSets),
    m_allowFree(allowFree),
    m_poolSizes{
        {VK_DESCRIPTOR_TYPE_SAMPLER, poolSizes.numSeparateSamplerDescriptors},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, poolSizes.numCombinedSamplerDescriptors},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, poolSizes.numSampledImageDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, poolSizes.numStorageImageDescriptors},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, poolSizes.numUniformTexelBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, poolSizes.numStorageTexelBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, poolSizes.numUniformBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, poolSizes.numStorageBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, poolSizes.numUniformBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, poolSizes.numStorageBufferDescriptors},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, poolSizes.numInputAttachmentDescriptors},
    }
{}

void DescriptorPoolManager::Cleanup()
{
    for (auto& pool : m_freePools)
    {
        vkDestroyDescriptorPool(m_device.GetHandle(), pool, nullptr);
    }

    for (auto& pool : m_usedPools)
    {
        vkDestroyDescriptorPool(m_device.GetHandle(), pool, nullptr);
    }
}

void DescriptorPoolManager::ResetPools()
{
    std::lock_guard<std::mutex> Lock{m_mutex};
    for (auto& pool : m_usedPools)
    {
        vkResetDescriptorPool(m_device.GetHandle(), pool, 0);
    }
    m_freePools = std::move(m_usedPools);
    m_usedPools.clear();
}

VkDescriptorPool DescriptorPoolManager::GrabPool()
{
    std::lock_guard<std::mutex> Lock{m_mutex};

    VkDescriptorPool pool{VK_NULL_HANDLE};
    if (!m_freePools.empty())
    {
        pool = m_freePools.back();
        m_freePools.pop_back();
    }
    else
    {
        pool = CreatePool();
    }
    m_usedPools.push_back(pool);
    return pool;
}

VkDescriptorPool DescriptorPoolManager::CreatePool()
{
    VkDescriptorPool pool{VK_NULL_HANDLE};

    VkDescriptorPoolCreateInfo descriptorPoolCI{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    descriptorPoolCI.maxSets       = m_maxSets;
    descriptorPoolCI.pPoolSizes    = m_poolSizes.data();
    descriptorPoolCI.poolSizeCount = static_cast<uint32_t>(m_poolSizes.size());
    descriptorPoolCI.flags = m_allowFree ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT |
            VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT :
                                           VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    vkCreateDescriptorPool(m_device.GetHandle(), &descriptorPoolCI, nullptr, &pool);

    return pool;
}

DescriptorSetAllocator::DescriptorSetAllocator(const Device& device,
                                               DescriptorPoolManager& poolManager) :
    m_device(device), m_poolManager(poolManager)
{}

VkDescriptorSet DescriptorSetAllocator::Allocate(const VkDescriptorSetLayout* layout)
{
    VkDescriptorSet set{VK_NULL_HANDLE};
    if (m_currentPool == VK_NULL_HANDLE)
    {
        m_currentPool = m_poolManager.GrabPool();
    }
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.pNext              = nullptr;
    allocInfo.pSetLayouts        = layout;
    allocInfo.descriptorPool     = m_currentPool;
    allocInfo.descriptorSetCount = 1;

    while (vkAllocateDescriptorSets(m_device.GetHandle(), &allocInfo, &set) != VK_SUCCESS)
    {
        m_currentPool = m_poolManager.GrabPool();
    }
    return set;
}

bool DescriptorSetAllocator::Allocate(VkDescriptorSetLayout* layout,
                                      uint32_t count,
                                      VkDescriptorSet* outSet)
{
    std::lock_guard<std::mutex> Lock{m_mutex};
    if (m_currentPool == VK_NULL_HANDLE)
    {
        m_currentPool = m_poolManager.GrabPool();
    }
    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.pNext              = nullptr;
    allocInfo.pSetLayouts        = layout;
    allocInfo.descriptorPool     = m_currentPool;
    allocInfo.descriptorSetCount = count;

    auto allocResult = vkAllocateDescriptorSets(m_device.GetHandle(), &allocInfo, outSet);

    switch (allocResult)
    {
        case VK_SUCCESS:
            //all good, return
            return true;
            break;
        case VK_ERROR_FRAGMENTED_POOL:
        case VK_ERROR_OUT_OF_POOL_MEMORY:
            //reallocate pool
            break;
        default:
            //unrecoverable error
            return false;
    }
    // allocate a new pool and retry
    m_currentPool = m_poolManager.GrabPool();
    // in case Vulkan Handles are just uint64_t values, overwrite the value
    allocInfo.descriptorPool = m_currentPool;

    allocResult = vkAllocateDescriptorSets(m_device.GetHandle(), &allocInfo, outSet);
    // if it still fails then we have big issues
    return allocResult == VK_SUCCESS;
}
} // namespace zen::val