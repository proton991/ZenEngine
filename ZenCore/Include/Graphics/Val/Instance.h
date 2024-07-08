#pragma once
#include "Common/SharedPtr.h"
#include "Common/UniquePtr.h"
#include <vector>
#include "VulkanHeaders.h"

namespace zen::val
{
void PrintInstanceLayers(const std::vector<VkLayerProperties>& layers);
void PrintInstanceExtensions(const std::vector<VkExtensionProperties>& enabledExtensions);

class Instance
{
public:
    struct CreateInfo
    {
        bool enableValidation                      = true;
        uint32_t enabledLayerCount                 = 0;
        const char* const* ppEnabledLayerNames     = nullptr;
        uint32_t enabledExtensionCount             = 0;
        const char* const* ppEnabledExtensionNames = nullptr;
    };

    static SharedPtr<Instance> Create(const CreateInfo& createInfo);
    static UniquePtr<Instance> CreateUnique(const CreateInfo& createInfo);

    explicit Instance(const CreateInfo& createInfo);
    ~Instance();

    bool IsExtensionSupported(std::vector<VkExtensionProperties>& extensions,
                              const char* extensionName);
    bool IsLayerSupported(const char* layerName, uint32_t& version);
    bool IsExtensionEnabled(const char* name);



    VkInstance GetHandle() const
    {
        return m_handle;
    }

    VkPhysicalDevice SelectPhysicalDevice();

private:
    bool EnumerateInstanceExtensions(const char* layerName,
                                     std::vector<VkExtensionProperties>& extensions);

    enum DebugMode
    {
        Disabled,
        Utils,
        Report
    };
    DebugMode m_debugMode = DebugMode::Disabled;

    std::vector<VkLayerProperties> m_supportedLayers;
    std::vector<VkExtensionProperties> m_supportedExtensions;
    std::vector<const char*> m_enabledExtensions;
    std::vector<VkPhysicalDevice> m_physicalDevices;
    VkInstance m_handle                            = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugUtilsMessenger = VK_NULL_HANDLE;
};
} // namespace zen::val
