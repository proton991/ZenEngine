#pragma once
#include <vulkan/vulkan_core.h>
#include <string>
namespace zen::rhi
{
enum class VulkanAPIVersion : uint32_t
{
    VULKAN_1_0 = VK_MAKE_API_VERSION(0, 1, 0, 0),
    VULKAN_1_1 = VK_MAKE_API_VERSION(0, 1, 1, 0),
    VULKAN_1_2 = VK_MAKE_API_VERSION(0, 1, 2, 0),
    VULKAN_1_3 = VK_MAKE_API_VERSION(0, 1, 3, 0),
    UNKNOWN
};

class VulkanContextCreateInfo
{
public:
private:
    std::string m_appName;
    std::string m_engineName;

    VulkanAPIVersion m_apiVersion;
};

class VulkanContext
{
public:
private:
    VkInstance       m_instance{nullptr};
    VkPhysicalDevice m_physicalDevice{nullptr};
    VkDevice         m_device{nullptr};
    // Instance Layers
};
} // namespace zen::rhi