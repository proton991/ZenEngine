#pragma once
#include <memory>
#include "VulkanHeaders.h"
#include "PhysicalDevice.h"
#include <vector>
#include <unordered_map>
#include "vk_mem_alloc.h"

namespace zen::val
{
class Queue;
class Instance;
class Device
{
public:
    struct CreateInfo
    {
        PhysicalDevice*    pPhysicalDevice         = nullptr;
        uint32_t           enabledExtensionCount   = 0;
        const char* const* ppEnabledExtensionNames = nullptr;
    };
    static std::shared_ptr<Device> Create(const CreateInfo& CI);
    ~Device();

    bool IsExtensionEnabled(const char* extension) const;

    VkDevice GetHandle() const { return m_handle; }

    VkPhysicalDevice GetPhysicalDeviceHandle() const { return m_physicalDevice->GetHandle(); }

    const Queue& GetQueue(QueueType queueType);

    VmaAllocator GetAllocator() const { return m_memAllocator; }

    VkInstance GetInstanceHandle() const { return m_physicalDevice->GetInstanceHandle(); }

private:
    explicit Device(const CreateInfo& CI);
    VkDevice                       m_handle{VK_NULL_HANDLE};
    PhysicalDevice*                m_physicalDevice{nullptr};
    std::vector<const char*>       m_enabledExtensions;
    std::unordered_map<int, Queue> m_queues;
    VmaAllocator                   m_memAllocator{nullptr};
};
} // namespace zen::val