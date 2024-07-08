#pragma once
#include "Instance.h"
#include <memory>
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
    uint32_t familyIndices[QUEUE_INDEX_COUNT] = {};
    uint32_t indices[QUEUE_INDEX_COUNT]       = {};
    std::vector<uint32_t> queueOffsets;
    std::vector<std::vector<float>> queuePriorities;
};


class PhysicalDevice
{
public:
    static UniquePtr<PhysicalDevice> CreateUnique(Instance& instance);

    VkBool32 IsPresentSupported(VkSurfaceKHR surface, uint32_t queueFamilyIndex);

    DeviceQueueInfo GetDeviceQueueInfo(VkSurfaceKHR surface);

    auto GetHandle()
    {
        return m_handle;
    }

    const VkPhysicalDeviceFeatures& GetFeatures() const
    {
        return m_features;
    }

    bool IsExtensionSupported(const char* extensionName) const;

    template <typename T> T& RequestExtensionFeatures(VkStructureType type)
    {
        // We cannot request extension features if the physical device properties 2 instance extension isn't enabled
        if (!m_instance.IsExtensionEnabled(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        {

            throw std::runtime_error(
                "Couldn't request feature from device as " +
                std::string(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) +
                " isn't enabled!");
        }

        // If the type already exists in the map, return a casted pointer to get the extension feature struct
        auto it = m_extensionFeatures.find(type);
        if (it != m_extensionFeatures.end())
        {
            return *static_cast<T*>(it->second.get());
        }

        // Get the extension feature
        VkPhysicalDeviceFeatures2KHR physicalDeviceFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR};
        T extension{type};
        physicalDeviceFeatures.pNext = &extension;
        vkGetPhysicalDeviceFeatures2KHR(m_handle, &physicalDeviceFeatures);

        // Insert the extension feature into the extension feature map so its ownership is held
        m_extensionFeatures.insert({type, std::make_shared<T>(extension)});

        // Pull out the dereference void pointer, we can assume its type based on the template
        auto* extensionPtr = static_cast<T*>(m_extensionFeatures.find(type)->second.get());

        // If an extension feature has already been requested, we shift the linked list down by one
        // Making this current extension the new base pointer
        if (m_featureChainHead)
        {
            extensionPtr->pNext = m_featureChainHead;
        }
        m_featureChainHead = extensionPtr;

        return *extensionPtr;
    }

    VkInstance GetInstanceHandle() const;

    void* GetExtensionFeatureChain() const
    {
        return m_featureChainHead;
    }

    VkPhysicalDeviceFeatures& GetMutableFeatures()
    {
        return m_requestedFeatures;
    }

    const VkPhysicalDeviceFeatures& GetRequestedFeatures() const
    {
        return m_requestedFeatures;
    }

private:
    explicit PhysicalDevice(Instance& instance);

    void PrintExtensions();

    Instance& m_instance;
    VkPhysicalDevice m_handle               = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_properties = {};
    VkPhysicalDeviceFeatures m_features     = {};
    std::vector<VkExtensionProperties> m_supportedExtensions;

    VkPhysicalDeviceFeatures m_requestedFeatures{};
    // The extension feature pointer
    void* m_featureChainHead = nullptr;
    // Holds the extension feature structures, we use a map to retain an order of requested structures
    std::map<VkStructureType, std::shared_ptr<void>> m_extensionFeatures;
    friend class Device;
};
} // namespace zen::val