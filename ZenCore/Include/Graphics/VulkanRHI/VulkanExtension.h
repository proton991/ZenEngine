#pragma once
#include "Graphics/VulkanRHI/VulkanHeaders.h"
#include "Utils/UniquePtr.h"
#include "Templates/HeapVector.h"
#include "Templates/NameID.h"

namespace zen
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
    uint32_t hasDebugUtils : 1;
};

class VulkanExtension
{
public:
    VulkanExtension(NameID extensionName, EnableMode enableMode = EnableMode::eAuto) :
        m_extensionName(extensionName),
        m_supported(false),
        m_enabled(enableMode == EnableMode::eAuto)
    {}

    NameID GetName() const
    {
        return m_extensionName;
    }

    void SetSupport(bool supported = true)
    {
        m_supported = supported;
    }

    void SetCoreSupport()
    {
        m_supported = true;
        m_core      = true;
    }

    bool RequiresExtensionName() const
    {
        return !m_core;
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
    NameID m_extensionName;
    // supported by driver
    bool m_supported{false};
    // enabled in RHI initialization
    bool m_enabled{false};
    bool m_core{false};
};
class VulkanInstanceExtension;
using VulkanInstanceExtensionArray = HeapVector<UniquePtr<VulkanInstanceExtension>>;

class VulkanInstanceExtension : public VulkanExtension
{
public:
    explicit VulkanInstanceExtension(NameID extensionName,
                                     EnableMode enableMode = EnableMode::eAuto) :
        VulkanExtension(extensionName, enableMode)
    {}

    static HeapVector<VkExtensionProperties> GetSupportedInstanceExtensions(NameID layerName = {});

    static VulkanInstanceExtensionArray GetEnabledInstanceExtensions(
        InstanceExtensionFlags& extensionFlags);
};

class VulkanDeviceExtension;
using VulkanDeviceExtensionArray = HeapVector<UniquePtr<VulkanDeviceExtension>>;

class VulkanDeviceExtension : public VulkanExtension
{
public:
    explicit VulkanDeviceExtension(VulkanDevice* pDevice,
                                   NameID extensionName,
                                   EnableMode enableMode = EnableMode::eAuto) :
        VulkanExtension(extensionName, enableMode), m_pDevice(pDevice)
    {}

    virtual ~VulkanDeviceExtension() = default;

    static HeapVector<VkExtensionProperties> GetSupportedExtensions(VkPhysicalDevice gpu);

    static VulkanDeviceExtensionArray GetEnabledExtensions(VulkanDevice* pDevice);

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
    VulkanDevice* m_pDevice{nullptr};
};

} // namespace zen
