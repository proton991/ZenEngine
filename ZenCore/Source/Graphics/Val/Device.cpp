#include <vector>
#include "Graphics/Val/Device.h"
#include "Graphics/Val/Queue.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
Device::~Device()
{
    if (m_memAllocator != VK_NULL_HANDLE)
    {
        VmaTotalStatistics stats;
        vmaCalculateStatistics(m_memAllocator, &stats);

        LOGI("Total device memory leaked: {} bytes.", stats.total.statistics.allocationBytes);

        vmaDestroyAllocator(m_memAllocator);
    }

    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_handle, nullptr);
    }
}

SharedPtr<Device> Device::Create(const Device::CreateInfo& CI)
{
    //    auto* valDevice = new Device(CI);
    return MakeShared<Device>(CI);
}

UniquePtr<Device> Device::CreateUnique(const Device::CreateInfo& CI)
{
    return MakeUnique<Device>(CI);
}

bool Device::IsExtensionEnabled(const char* extension) const
{
    return std::find_if(m_enabledExtensions.begin(), m_enabledExtensions.end(),
                        [extension](const char* enabled_extension) {
                            return strcmp(extension, enabled_extension) == 0;
                        }) != m_enabledExtensions.end();
}

Device::Device(const Device::CreateInfo& CI)
{
    m_physicalDevice = CI.pPhysicalDevice;

    auto descriptorIndexingFeatures =
        m_physicalDevice->RequestExtensionFeatures<VkPhysicalDeviceDescriptorIndexingFeatures>(
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
    descriptorIndexingFeatures.runtimeDescriptorArray                             = true;
    descriptorIndexingFeatures.descriptorBindingPartiallyBound                    = true;
    descriptorIndexingFeatures.shaderInputAttachmentArrayDynamicIndexing          = true;
    descriptorIndexingFeatures.shaderUniformTexelBufferArrayDynamicIndexing       = true;
    descriptorIndexingFeatures.shaderStorageTexelBufferArrayDynamicIndexing       = true;
    descriptorIndexingFeatures.shaderUniformBufferArrayNonUniformIndexing         = true;
    descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing          = true;
    descriptorIndexingFeatures.shaderStorageBufferArrayNonUniformIndexing         = true;
    descriptorIndexingFeatures.shaderStorageImageArrayNonUniformIndexing          = true;
    descriptorIndexingFeatures.shaderInputAttachmentArrayNonUniformIndexing       = true;
    descriptorIndexingFeatures.shaderUniformTexelBufferArrayNonUniformIndexing    = true;
    descriptorIndexingFeatures.shaderStorageTexelBufferArrayNonUniformIndexing    = true;
    descriptorIndexingFeatures.descriptorBindingUniformBufferUpdateAfterBind      = true;
    descriptorIndexingFeatures.descriptorBindingSampledImageUpdateAfterBind       = true;
    descriptorIndexingFeatures.descriptorBindingStorageImageUpdateAfterBind       = true;
    descriptorIndexingFeatures.descriptorBindingStorageBufferUpdateAfterBind      = true;
    descriptorIndexingFeatures.descriptorBindingUniformTexelBufferUpdateAfterBind = true;
    descriptorIndexingFeatures.descriptorBindingStorageTexelBufferUpdateAfterBind = true;
    m_enabledExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);

#if defined(ZEN_MACOS)
    m_enabledExtensions.push_back("VK_KHR_portability_subset");
#endif

    if (CI.enableRaytracing)
    {
        auto asFeature =
            m_physicalDevice
                ->RequestExtensionFeatures<VkPhysicalDeviceAccelerationStructureFeaturesKHR>(
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
        asFeature.accelerationStructure = true;

        auto rtPipelineFeature =
            m_physicalDevice
                ->RequestExtensionFeatures<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
        rtPipelineFeature.rayTracingPipeline = true;

        auto bufferDeviceAddrFeature =
            m_physicalDevice->RequestExtensionFeatures<VkPhysicalDeviceBufferDeviceAddressFeatures>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR);
        bufferDeviceAddrFeature.bufferDeviceAddress = true;

        m_enabledExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        m_enabledExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        m_enabledExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        m_enabledExtensions.push_back(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    }
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    // fill in queue create info
    auto queueInfo = CI.pPhysicalDevice->GetDeviceQueueInfo(VK_NULL_HANDLE);
    for (uint32_t familyIndex = 0; familyIndex < queueInfo.queueOffsets.size(); familyIndex++)
    {
        if (queueInfo.queueOffsets[familyIndex] != 0)
        {
            VkDeviceQueueCreateInfo queueCI{};
            queueCI.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCI.queueCount       = queueInfo.queueOffsets[familyIndex];
            queueCI.pQueuePriorities = queueInfo.queuePriorities[familyIndex].data();
            queueCI.queueFamilyIndex = familyIndex;
            queueCreateInfos.push_back(queueCI);
        }
    }

    for (auto i = 0u; i < CI.enabledExtensionCount; i++)
    {
        auto extensionName = CI.ppEnabledExtensionNames[i];
        if (CI.pPhysicalDevice->IsExtensionSupported(extensionName))
        {
            m_enabledExtensions.push_back(extensionName);
        }
        else
        {
            LOGE("Requested device extension: {} is not supported! (Ignored)", extensionName);
        }
    }
    VkDeviceCreateInfo deviceCI{};
    deviceCI.sType                 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCI.pQueueCreateInfos     = queueCreateInfos.data();
    deviceCI.queueCreateInfoCount  = queueCreateInfos.size();
    deviceCI.enabledExtensionCount = m_enabledExtensions.size();
    deviceCI.ppEnabledExtensionNames =
        m_enabledExtensions.empty() ? nullptr : m_enabledExtensions.data();
    deviceCI.pEnabledFeatures = &CI.pPhysicalDevice->GetRequestedFeatures();
    deviceCI.pNext            = CI.pPhysicalDevice->GetExtensionFeatureChain();

    CHECK_VK_ERROR_AND_THROW(
        vkCreateDevice(CI.pPhysicalDevice->GetHandle(), &deviceCI, nullptr, &m_handle),
        "Failed to create device!")
    volkLoadDevice(m_handle);
    SetDeviceName(m_handle, "VulkanLogicalDevice");

    LOGI("Enabled Device Extensions count: {}", m_enabledExtensions.size())
    for (const auto& ext : m_enabledExtensions)
    {
        LOGI("\tEnabled Device Extension: {}", ext);
    }

    // get device queues
    for (auto queueType = 0; queueType < QUEUE_INDEX_COUNT; queueType++)
    {
        const auto familyIndex = queueInfo.familyIndices[queueType];
        if (familyIndex != VK_QUEUE_FAMILY_IGNORED)
        {
            bool supportPresent =
                CI.pPhysicalDevice->IsPresentSupported(VK_NULL_HANDLE, familyIndex);

            m_queues.emplace(
                queueType, Queue(*this, familyIndex, queueInfo.indices[queueType], supportPresent));
        }
    }

    VmaVulkanFunctions vmaVkFunc{};
    vmaVkFunc.vkGetInstanceProcAddr               = vkGetInstanceProcAddr;
    vmaVkFunc.vkGetDeviceProcAddr                 = vkGetDeviceProcAddr;
    vmaVkFunc.vkAllocateMemory                    = vkAllocateMemory;
    vmaVkFunc.vkBindBufferMemory                  = vkBindBufferMemory;
    vmaVkFunc.vkBindImageMemory                   = vkBindImageMemory;
    vmaVkFunc.vkCreateBuffer                      = vkCreateBuffer;
    vmaVkFunc.vkCreateImage                       = vkCreateImage;
    vmaVkFunc.vkDestroyBuffer                     = vkDestroyBuffer;
    vmaVkFunc.vkDestroyImage                      = vkDestroyImage;
    vmaVkFunc.vkFlushMappedMemoryRanges           = vkFlushMappedMemoryRanges;
    vmaVkFunc.vkFreeMemory                        = vkFreeMemory;
    vmaVkFunc.vkGetBufferMemoryRequirements       = vkGetBufferMemoryRequirements;
    vmaVkFunc.vkGetImageMemoryRequirements        = vkGetImageMemoryRequirements;
    vmaVkFunc.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vmaVkFunc.vkGetPhysicalDeviceProperties       = vkGetPhysicalDeviceProperties;
    vmaVkFunc.vkInvalidateMappedMemoryRanges      = vkInvalidateMappedMemoryRanges;
    vmaVkFunc.vkMapMemory                         = vkMapMemory;
    vmaVkFunc.vkUnmapMemory                       = vkUnmapMemory;
    vmaVkFunc.vkCmdCopyBuffer                     = vkCmdCopyBuffer;

    VmaAllocatorCreateInfo vmaCI{};
    vmaCI.physicalDevice = CI.pPhysicalDevice->GetHandle();
    vmaCI.device         = m_handle;
    vmaCI.instance       = CI.pPhysicalDevice->GetInstanceHandle();

    if (IsExtensionEnabled(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
    {
        vmaCI.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    }

    vmaCI.pVulkanFunctions = &vmaVkFunc;

    CHECK_VK_ERROR_AND_THROW(vmaCreateAllocator(&vmaCI, &m_memAllocator), "Failed to create VMA");
}

const Queue& Device::GetQueue(QueueType queueType) const
{
    if (m_queues.count(queueType))
    {
        return m_queues.at(queueType);
    }
    LOG_ERROR_AND_THROW("Queue not found!");
}

void Device::WaitIdle() const
{
    vkDeviceWaitIdle(m_handle);
}
} // namespace zen::val