#include "Graphics/Val/PhysicalDevice.h"
#include "Graphics/Val/Instance.h"
#include "Common/Errors.h"

namespace zen::val
{
UniquePtr<PhysicalDevice> PhysicalDevice::CreateUnique(Instance& instance)
{
    auto* physicalDevice = new PhysicalDevice(instance);
    return UniquePtr<PhysicalDevice>(physicalDevice);
}

PhysicalDevice::PhysicalDevice(Instance& instance) :
    m_instance(instance), m_handle(instance.SelectPhysicalDevice())
{
    vkGetPhysicalDeviceProperties(m_handle, &m_properties);
    vkGetPhysicalDeviceFeatures(m_handle, &m_features);
    LOGI("Using Physical Device: {}", m_properties.deviceName)
    uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(m_handle, nullptr, &extensionCount, nullptr);
    if (extensionCount > 0)
    {
        m_supportedExtensions.resize(extensionCount);
        vkEnumerateDeviceExtensionProperties(m_handle, nullptr, &extensionCount,
                                             m_supportedExtensions.data());
    }
    //    PrintExtensions();
}

void PhysicalDevice::PrintExtensions()
{
    LOGI("Device supported extension count: {}", m_supportedExtensions.size());
    for (const auto& extension : m_supportedExtensions)
    {
        LOGI("Supported device extension name : {}", extension.extensionName);
    }
}

VkBool32 PhysicalDevice::IsPresentSupported(VkSurfaceKHR surface, uint32_t queueFamilyIndex)
{
    VkBool32 supported{VK_FALSE};

    if (surface != VK_NULL_HANDLE)
        CHECK_VK_ERROR(
            vkGetPhysicalDeviceSurfaceSupportKHR(m_handle, queueFamilyIndex, surface, &supported),
            "Failed to get surface support");

    return supported;
}

DeviceQueueInfo PhysicalDevice::GetDeviceQueueInfo(VkSurfaceKHR surface)
{
    DeviceQueueInfo info{};
    uint32_t queueFamilyCount = 0;
    std::vector<VkQueueFamilyProperties> queueFamilyProps;
    vkGetPhysicalDeviceQueueFamilyProperties(m_handle, &queueFamilyCount, nullptr);
    queueFamilyProps.resize(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_handle, &queueFamilyCount, queueFamilyProps.data());
    VERIFY_EXPR(queueFamilyCount > 0);
    std::vector<uint32_t> queueOffsets(queueFamilyProps.size());
    std::vector<std::vector<float>> queuePriorities(queueFamilyProps.size());

    const auto findProperQueue = [&](uint32_t& family, uint32_t& index, VkQueueFlags required,
                                     VkQueueFlags ignored, float priority) -> bool {
        for (unsigned familyIndex = 0; familyIndex < queueFamilyProps.size(); familyIndex++)
        {
            if (queueFamilyProps[familyIndex].queueFlags & ignored)
                continue;

            // A graphics queue candidate must support present for us to select it.
            if ((required & VK_QUEUE_GRAPHICS_BIT) && surface)
            {
                if (!IsPresentSupported(surface, familyIndex))
                    continue;
            }

            if (queueFamilyProps[familyIndex].queueCount > 0 &&
                (queueFamilyProps[familyIndex].queueFlags & required))
            {
                family = familyIndex;
                queueFamilyProps[familyIndex].queueCount--;
                index = queueOffsets[familyIndex]++;
                queuePriorities[familyIndex].push_back(priority);
                return true;
            }
        }
        return false;
    };
    // find graphics queue
    if (!findProperQueue(info.familyIndices[QUEUE_INDEX_GRAPHICS],
                         info.indices[QUEUE_INDEX_GRAPHICS], VK_QUEUE_GRAPHICS_BIT, 0, 1.0f))
    {
        throw std::runtime_error("Failed to find a graphics queue");
    }
    // Prefer another graphics queue since we can do async graphics that way.
    // The compute queue is to be treated as high priority since we also do async graphics on it.
    if (!findProperQueue(info.familyIndices[QUEUE_INDEX_COMPUTE], info.indices[QUEUE_INDEX_COMPUTE],
                         VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0, 1.0f))
    {
        if (!findProperQueue(info.familyIndices[QUEUE_INDEX_COMPUTE],
                             info.indices[QUEUE_INDEX_COMPUTE], VK_QUEUE_COMPUTE_BIT, 0, 1.0f))
        {
            // fall back to the graphics queue if we must.
            info.familyIndices[QUEUE_INDEX_COMPUTE] = info.familyIndices[QUEUE_INDEX_GRAPHICS];
            info.indices[QUEUE_INDEX_COMPUTE]       = info.indices[QUEUE_INDEX_GRAPHICS];
        }
    }
    // For transfer, try to find a queue which only supports transfer, e.g. DMA queue.
    // If not, fall back to a dedicated compute queue.
    // Finally, fall back to same queue as compute.
    if (!findProperQueue(info.familyIndices[QUEUE_INDEX_TRANSFER],
                         info.indices[QUEUE_INDEX_TRANSFER], VK_QUEUE_TRANSFER_BIT,
                         VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0.5f) &&
        !findProperQueue(info.familyIndices[QUEUE_INDEX_TRANSFER],
                         info.indices[QUEUE_INDEX_TRANSFER], VK_QUEUE_COMPUTE_BIT,
                         VK_QUEUE_GRAPHICS_BIT, 0.5f))
    {
        info.familyIndices[QUEUE_INDEX_TRANSFER] = info.familyIndices[QUEUE_INDEX_COMPUTE];
        info.indices[QUEUE_INDEX_TRANSFER]       = info.indices[QUEUE_INDEX_COMPUTE];
    }

    info.queuePriorities = std::move(queuePriorities);
    info.queueOffsets    = std::move(queueOffsets);

    return info;
}

bool PhysicalDevice::IsExtensionSupported(const char* extensionName) const
{
    auto it = std::find_if(m_supportedExtensions.begin(), m_supportedExtensions.end(),
                           [&](const VkExtensionProperties& extension) {
                               return strcmp(extension.extensionName, extensionName) == 0;
                           });
    return it != m_supportedExtensions.end();
}

VkInstance PhysicalDevice::GetInstanceHandle() const
{
    return m_instance.GetHandle();
}
} // namespace zen::val