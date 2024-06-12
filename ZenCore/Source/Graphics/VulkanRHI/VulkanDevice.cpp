#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"

namespace zen::rhi
{
VulkanDevice::VulkanDevice(VulkanRHI* RHI, VkPhysicalDevice gpu) :
    m_RHI(RHI), m_device(VK_NULL_HANDLE), m_gpu(gpu)
{
    vkGetPhysicalDeviceProperties(m_gpu, &m_gpuProps);
}

static std::string GetQueuePropString(const VkQueueFamilyProperties& queueProp)
{
    std::string queuePropStr;
    if ((queueProp.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT)
    {
        queuePropStr += " Gfx";
    }
    if ((queueProp.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
    {
        queuePropStr += " Compute";
    }
    if ((queueProp.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT)
    {
        queuePropStr += " Transfer";
    }
    if ((queueProp.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) == VK_QUEUE_SPARSE_BINDING_BIT)
    {
        queuePropStr += " Sparse";
    }
    return queuePropStr;
}
/**
 * Create VkDevice and initialise
 */
void VulkanDevice::Init()
{
    // query base features
    vkGetPhysicalDeviceFeatures(m_gpu, &m_physicalDeviceFeatures);
    // query gpu properties
    vkGetPhysicalDeviceProperties(m_gpu, &m_physicalDeviceProperties);
    // query queue properties
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &count, nullptr);
    VERIFY_EXPR(count > 0);
    m_queueFamilyProps.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(m_gpu, &count, m_queueFamilyProps.data());

    VulkanDeviceExtensionArray extensionArray = VulkanDeviceExtension::GetEnabledExtensions(this);
    if (m_RHI->GetInstanceExtensionFlags().hasGetPhysicalDeviceProperties)
    {
        VkPhysicalDeviceFeatures2 physicalDeviceFeatures2;
        InitVkStruct(physicalDeviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->BeforePhysicalDeviceFeatures(physicalDeviceFeatures2);
            }
        }
        vkGetPhysicalDeviceFeatures2(m_gpu, &physicalDeviceFeatures2);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported()) { extension->AfterPhysicalDeviceFeatures(); }
        }

        VkPhysicalDeviceProperties2 physicalDeviceProperties2;
        InitVkStruct(physicalDeviceProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->BeforePhysicalDeviceProperties(physicalDeviceProperties2);
            }
        }
        vkGetPhysicalDeviceProperties2(m_gpu, &physicalDeviceProperties2);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported()) { extension->AfterPhysicalDeviceProperties(); }
        }
    }

    SetupDevice(extensionArray);
}

void VulkanDevice::SetupDevice(std::vector<UniquePtr<VulkanDeviceExtension>>& extensions)
{
    VkDeviceCreateInfo deviceInfo;
    InitVkStruct(deviceInfo, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

    for (auto& extension : extensions)
    {
        if (extension->IsEnabledAndSupported())
        {
            m_extensions.emplace_back(extension->GetName());
            extension->BeforeCreateDevice(deviceInfo);
        }
    }
    // set up queue info
    std::vector<VkDeviceQueueCreateInfo> deviceQueueInfos;

    int32_t graphicsQueueFamilyIndex = -1;
    int32_t computeQueueFamilyIndex  = -1;
    int32_t transferQueueFamilyIndex = -1;
    LOGI("Found {} Vulkan Queue Families", m_queueFamilyProps.size());
    uint32_t numPriorities = 0;
    for (int32_t queueFamilyIndex = 0; queueFamilyIndex < m_queueFamilyProps.size();
         queueFamilyIndex++)
    {
        const auto& queueFamilyProp = m_queueFamilyProps[queueFamilyIndex];
        bool        isValidQueue    = false;
        if ((queueFamilyProp.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT)
        {
            if (graphicsQueueFamilyIndex == -1)
            {
                graphicsQueueFamilyIndex = queueFamilyIndex;
                isValidQueue             = true;
            }
        }
        if ((queueFamilyProp.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT)
        {
            // prefer dedicated compute queue
            if (computeQueueFamilyIndex == -1 && graphicsQueueFamilyIndex != queueFamilyIndex)
            {
                computeQueueFamilyIndex = queueFamilyIndex;
                isValidQueue            = true;
            }
        }
        if ((queueFamilyProp.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT)
        {
            // prefer non-graphics transfer queue
            if (transferQueueFamilyIndex == -1 &&
                (queueFamilyProp.queueFlags & VK_QUEUE_GRAPHICS_BIT) != VK_QUEUE_GRAPHICS_BIT &&
                (queueFamilyProp.queueFlags & VK_QUEUE_COMPUTE_BIT) != VK_QUEUE_COMPUTE_BIT)
            {
                transferQueueFamilyIndex = queueFamilyIndex;
                isValidQueue             = true;
            }
        }
        if (!isValidQueue)
        {
            LOGI("Skipping Invalid Queue Family at index: {} ({})", queueFamilyIndex,
                 GetQueuePropString(queueFamilyProp));
            continue;
        }
        VkDeviceQueueCreateInfo queueInfo;
        InitVkStruct(queueInfo, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueInfo.queueFamilyIndex = deviceQueueInfos.size();
        queueInfo.queueCount       = queueFamilyProp.queueCount;
        deviceQueueInfos.emplace_back(queueInfo);
        numPriorities += queueFamilyProp.queueCount;
        LOGI("Initializing Queue Family at index {} ({})", queueFamilyIndex,
             GetQueuePropString(queueFamilyProp));
    }
    std::vector<float> queuePriorities;
    queuePriorities.resize(numPriorities);
    float* currentPriority = queuePriorities.data();
    for (auto i = 0; i < deviceQueueInfos.size(); i++)
    {
        VkDeviceQueueCreateInfo& queueInfo = deviceQueueInfos[i];
        queueInfo.pQueuePriorities         = currentPriority;
        const VkQueueFamilyProperties& queueFamilyProp =
            m_queueFamilyProps[queueInfo.queueFamilyIndex];
        for (auto queueIndex = 0; queueIndex < queueFamilyProp.queueCount; queueIndex++)
        {
            *currentPriority++ = 1.0f;
        }
    }

    deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(m_extensions.size());
    deviceInfo.ppEnabledExtensionNames = m_extensions.empty() ? nullptr : m_extensions.data();
    deviceInfo.queueCreateInfoCount    = static_cast<uint32_t>(deviceQueueInfos.size());
    deviceInfo.pQueueCreateInfos       = deviceQueueInfos.data();

    VKCHECK(vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device));
    LOGI("Vulkan Device Created");
    // display extension info
    for (const auto& extension : m_extensions) { LOGI("Enabled Device Extension: {}", extension); }
    // load device func
    volkLoadDevice(m_device);
    // setup queues
    m_gfxQueue = new VulkanQueue(this, graphicsQueueFamilyIndex);
    if (computeQueueFamilyIndex == -1) { computeQueueFamilyIndex = graphicsQueueFamilyIndex; }
    m_computeQueue = new VulkanQueue(this, computeQueueFamilyIndex);
    if (transferQueueFamilyIndex == -1) { transferQueueFamilyIndex = computeQueueFamilyIndex; }
    m_transferQueue = new VulkanQueue(this, transferQueueFamilyIndex);
}

void VulkanDevice::SetObjectName(VkObjectType type, uint64_t handle, const char* name)
{
    VkDebugUtilsObjectNameInfoEXT info;
    InitVkStruct(info, VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name;
    vkSetDebugUtilsObjectNameEXT(m_device, &info);
}

void VulkanDevice::Destroy() { vkDestroyDevice(m_device, nullptr); }
} // namespace zen::rhi
