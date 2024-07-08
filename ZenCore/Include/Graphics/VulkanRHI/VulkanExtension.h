#pragma once
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Common/UniquePtr.h"
#include <string>

namespace zen::rhi
{
class VulkanDevice;

enum class VulkanAPIVersion : uint32_t
{
    VULKAN_1_0 = VK_MAKE_API_VERSION(0, 1, 0, 0),
    VULKAN_1_1 = VK_MAKE_API_VERSION(0, 1, 1, 0),
    VULKAN_1_2 = VK_MAKE_API_VERSION(0, 1, 2, 0),
    VULKAN_1_3 = VK_MAKE_API_VERSION(0, 1, 3, 0),
    UNKNOWN
};

enum class EnableMode : uint32_t
{
    eAuto,
    eManual
};

struct InstanceExtensionFlags
{
    uint32_t hasGetPhysicalDeviceProperties : 1;
};

class VulkanExtension
{
public:
    VulkanExtension(const char* extensionName, EnableMode enableMode = EnableMode::eAuto) :
        m_extensionName(extensionName),
        m_supported(false),
        m_enabled(enableMode == EnableMode::eAuto)
    {}

    const char* GetName() const
    {
        return m_extensionName;
    }

    void SetSupport()
    {
        m_supported = true;
    }

    void SetEnable()
    {
        m_enabled = true;
    }

    bool IsEnabledAndSupported() const
    {
        return m_enabled && m_supported;
    }

private:
    const char* m_extensionName{nullptr};
    // supported by driver
    bool m_supported{false};
    // enabled in RHI initialization
    bool m_enabled{false};
};
class VulkanInstanceExtension;
using VulkanInstanceExtensionArray = std::vector<UniquePtr<VulkanInstanceExtension>>;

class VulkanInstanceExtension : public VulkanExtension
{
public:
    explicit VulkanInstanceExtension(const char* extensionName,
                                     EnableMode enableMode = EnableMode::eAuto) :
        VulkanExtension(extensionName, enableMode)
    {}

    static std::vector<VkExtensionProperties> GetSupportedInstanceExtensions(
        const char* layerName = nullptr);

    static VulkanInstanceExtensionArray GetEnabledInstanceExtensions(
        InstanceExtensionFlags& extensionFlags);
};

class VulkanDeviceExtension;
using VulkanDeviceExtensionArray = std::vector<UniquePtr<VulkanDeviceExtension>>;

class VulkanDeviceExtension : public VulkanExtension
{
public:
    explicit VulkanDeviceExtension(VulkanDevice* device,
                                   const char* extensionName,
                                   EnableMode enableMode = EnableMode::eAuto) :
        VulkanExtension(extensionName, enableMode), m_device(device)
    {}

    virtual ~VulkanDeviceExtension() = default;

    static std::vector<VkExtensionProperties> GetSupportedExtensions(VkPhysicalDevice gpu);

    static VulkanDeviceExtensionArray GetEnabledExtensions(VulkanDevice* device);

#ifdef VK_KHR_get_physical_device_properties2
    virtual void BeforePhysicalDeviceProperties(
        VkPhysicalDeviceProperties2& PhysicalDeviceProperties2)
    {}

    virtual void AfterPhysicalDeviceProperties() {}

    virtual void BeforePhysicalDeviceFeatures(
        VkPhysicalDeviceFeatures2KHR& physicalDeviceFeatures2Khr)
    {}

    virtual void AfterPhysicalDeviceFeatures() {}

    virtual void BeforeCreateDevice(VkDeviceCreateInfo& DeviceCI) {}

#endif

protected:
    VulkanDevice* m_device{nullptr};
};

} // namespace zen::rhi