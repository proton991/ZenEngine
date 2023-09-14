#pragma once
#include "Common/UniquePtr.h"
#include <memory>
#include <vulkan/vulkan.h>
#include <vector>
#include <stdexcept>
#include <map>

namespace zen::val
{
enum QueueType
{
    QUEUE_INDEX_GRAPHICS = 0,
    QUEUE_INDEX_COMPUTE,
    QUEUE_INDEX_TRANSFER,
    QUEUE_INDEX_VIDEO_DECODE,
    QUEUE_INDEX_COUNT
};

struct DeviceQueueInfo
{
    DeviceQueueInfo()
    {
        for (auto i = 0; i < QUEUE_INDEX_COUNT; i++)
        {
            familyIndices[i] = VK_QUEUE_FAMILY_IGNORED;
            indices[i]       = uint32_t(-1);
        }
    }
    uint32_t                        familyIndices[QUEUE_INDEX_COUNT] = {};
    uint32_t                        indices[QUEUE_INDEX_COUNT]       = {};
    std::vector<uint32_t>           queueOffsets;
    std::vector<std::vector<float>> queuePriorities;
};

class Instance;
class PhysicalDevice
{
public:
    static UniquePtr<PhysicalDevice> Create(Instance& instance);

    VkBool32 IsPresentSupported(VkSurfaceKHR surface, uint32_t queueFamilyIndex);

    DeviceQueueInfo GetDeviceQueueInfo(VkSurfaceKHR surface);

    auto GetHandle() { return m_handle; }

    const VkPhysicalDeviceFeatures& GetFeatures() const { return m_features; }

    bool IsExtensionSupported(const char* extensionName) const;

    template <typename T>
    T& RequestExtensionFeatures(VkStructureType type);

    VkInstance GetInstanceHandle() const;

    void* GetExtensionFeatureChain() const
    {
        return m_featureChainHead;
    }

    VkPhysicalDeviceFeatures& GetMutableFeatures() { return m_requestedFeatures; }

    const VkPhysicalDeviceFeatures& GetRequestedFeatures() const { return m_requestedFeatures; }

private:
    explicit PhysicalDevice(Instance& instance);

    void PrintExtensions();

    Instance&                          m_instance;
    VkPhysicalDevice                   m_handle     = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties         m_properties = {};
    VkPhysicalDeviceFeatures           m_features   = {};
    std::vector<VkExtensionProperties> m_supportedExtensions;

    VkPhysicalDeviceFeatures m_requestedFeatures{};
    // The extension feature pointer
    void* m_featureChainHead = nullptr;
    // Holds the extension feature structures, we use a map to retain an order of requested structures
    std::map<VkStructureType, std::shared_ptr<void>> m_extensionFeatures;
};
} // namespace zen::val