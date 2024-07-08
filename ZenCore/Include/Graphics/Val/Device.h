#pragma once
#include "Common/SharedPtr.h"
#include "Common/UniquePtr.h"
#include "VulkanHeaders.h"
#include "PhysicalDevice.h"
#include <vector>
#include "Common/HashMap.h"
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
        PhysicalDevice* pPhysicalDevice            = nullptr;
        uint32_t enabledExtensionCount             = 0;
        const char* const* ppEnabledExtensionNames = nullptr;
        // enable raytracing features and extensions
        bool enableRaytracing = false;
    };

    static SharedPtr<Device> Create(const CreateInfo& CI);
    static UniquePtr<Device> CreateUnique(const CreateInfo& CI);

    explicit Device(const CreateInfo& CI);
    ~Device();

    bool IsExtensionEnabled(const char* extension) const;

    VkDevice GetHandle() const
    {
        return m_handle;
    }

    VkPhysicalDevice GetPhysicalDeviceHandle() const
    {
        return m_physicalDevice->GetHandle();
    }

    const Queue& GetQueue(QueueType queueType) const;

    VmaAllocator GetAllocator() const
    {
        return m_memAllocator;
    }

    VkInstance GetInstanceHandle() const
    {
        return m_physicalDevice->GetInstanceHandle();
    }

    void WaitIdle() const;

    auto GetGPUProperties() const
    {
        return m_physicalDevice->m_properties;
    }

private:
    VkDevice m_handle{VK_NULL_HANDLE};
    PhysicalDevice* m_physicalDevice{nullptr};
    std::vector<const char*> m_enabledExtensions;
    HashMap<int, Queue> m_queues;
    VmaAllocator m_memAllocator{nullptr};
};
} // namespace zen::val