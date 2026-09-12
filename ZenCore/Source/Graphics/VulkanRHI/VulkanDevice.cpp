#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/RHI/RHICommandList.h"
#include "Graphics/RHI/RHICommon.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanCopyCapabilities.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"

#include <algorithm>

namespace zen
{
VulkanDevice::VulkanDevice(VkPhysicalDevice gpu) : m_device(VK_NULL_HANDLE), m_gpu(gpu)
{
    vkGetPhysicalDeviceProperties(m_gpu, &m_gpuProps);
}

std::string VulkanDevice::GetUnsupportedReason(VkPhysicalDevice gpu)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_2)
    {
        return "Vulkan 1.2 is required";
    }
    const auto extensions = VulkanDeviceExtension::GetSupportedExtensions(gpu);
    for (const char* required :
         {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME})
    {
        if (std::none_of(extensions.begin(), extensions.end(), [required](const auto& extension) {
                return strcmp(required, extension.extensionName) == 0;
            }))
        {
            return std::string("Required extension is missing: ") + required;
        }
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    HeapVector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, queues.data());
    if (std::none_of(queues.begin(), queues.end(), [](const auto& queue) {
            return queue.queueCount != 0 &&
                (queue.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
        }))
    {
        return "A graphics queue with compute support is required";
    }

    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures indexing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    features.pNext = &dynamic;
    dynamic.pNext  = &indexing;
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    if (!dynamic.dynamicRendering)
    {
        return "The dynamicRendering feature is required";
    }
    if (!indexing.runtimeDescriptorArray || !indexing.descriptorBindingPartiallyBound ||
        !indexing.descriptorBindingUpdateUnusedWhilePending ||
        !indexing.descriptorBindingSampledImageUpdateAfterBind ||
        !indexing.descriptorBindingVariableDescriptorCount ||
        !indexing.shaderSampledImageArrayNonUniformIndexing)
    {
        return "Descriptor indexing with partially bound, variable, update-after-bind sampled images and nonuniform indexing is required";
    }

    VkPhysicalDeviceDescriptorIndexingProperties limits{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties2.pNext = &limits;
    vkGetPhysicalDeviceProperties2(gpu, &properties2);
    const uint32_t images = GetBindlessHeapCapacity(RHIBindlessHeapType::eTexture2D) +
        GetBindlessHeapCapacity(RHIBindlessHeapType::eTextureCube);
    const uint32_t samplers = GetBindlessHeapCapacity(RHIBindlessHeapType::eSampler);
    if (limits.maxDescriptorSetUpdateAfterBindSampledImages < images ||
        limits.maxPerStageDescriptorUpdateAfterBindSampledImages < images ||
        limits.maxDescriptorSetUpdateAfterBindSamplers < samplers ||
        limits.maxPerStageDescriptorUpdateAfterBindSamplers < samplers ||
        limits.maxPerStageUpdateAfterBindResources < images + samplers ||
        limits.maxUpdateAfterBindDescriptorsInAllPools < images + samplers)
    {
        return "Descriptor indexing limits cannot accommodate the global bindless heaps";
    }
    return {};
}

uint32_t VulkanDevice::GetDescriptorSetUpdateAfterBindLimit(VkDescriptorType descriptorType) const
{
    uint32_t limit = 0;

    switch (descriptorType)
    {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSamplers;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            limit = std::min(
                m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSamplers,
                m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages);
            break;

        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindSampledImages;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageImages;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffers;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            limit =
                m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindUniformBuffersDynamic;
            break;

        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindStorageBuffers;
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            limit = m_descriptorIndexingProperties.maxDescriptorSetUpdateAfterBindInputAttachments;
            break;
        default: break;
    }

    return limit;
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

DataFormat VulkanRHI::GetSupportedDepthFormat()
{
    VkFormat defaulFormat{VK_FORMAT_D16_UNORM};
    const HeapVector<VkFormat> formatList = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                             VK_FORMAT_D24_UNORM_S8_UINT,
                                             VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};

    for (const VkFormat& format : formatList)
    {
        VkFormatProperties formatProps;
        vkGetPhysicalDeviceFormatProperties(m_pDevice->GetPhysicalDeviceHandle(), format,
                                            &formatProps);

        if (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            defaulFormat = format;
            break;
        }
    }

    return static_cast<DataFormat>(defaulFormat);
}

const RHIGPUInfo& VulkanRHI::QueryGPUInfo() const
{
    return m_gpuInfo;
}

RHITextureCopyCapabilities VulkanRHI::GetTextureCopyCapabilities(DataFormat format) const
{
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(m_pDevice->GetPhysicalDeviceHandle(),
                                        static_cast<VkFormat>(format), &properties);

    return MakeTextureCopyCapabilities(properties);
}

RHIQueueCopyCapabilities VulkanRHI::GetQueueCopyCapabilities(RHICommandContextType type) const
{
    const VulkanQueue* queue = m_pDevice->GetQueue(type);
    return MakeQueueCopyCapabilities(m_pDevice->GetQueueFamilyProperties(queue->GetFamilyIndex()));
}

/**
 * Create VkDevice and initialise
 */
void VulkanDevice::Init()
{
    const std::string unsupportedReason = GetUnsupportedReason(m_gpu);
    if (!unsupportedReason.empty())
    {
        LOG_ERROR_AND_THROW("Cannot initialize Vulkan device '{}': {}", m_gpuProps.deviceName,
                            unsupportedReason);
    }
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

    if (GVulkanRHI->GetInstanceExtensionFlags().hasGetPhysicalDeviceProperties)
    {
        VkPhysicalDeviceFeatures2 physicalDeviceFeatures2;
        InitVkStruct(physicalDeviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);

        for (UniquePtr<VulkanDeviceExtension>& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->BeforePhysicalDeviceFeatures(physicalDeviceFeatures2);
            }
        }

        vkGetPhysicalDeviceFeatures2(m_gpu, &physicalDeviceFeatures2);

        for (UniquePtr<VulkanDeviceExtension>& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->AfterPhysicalDeviceFeatures();
            }
        }

        VkPhysicalDeviceProperties2 physicalDeviceProperties2;
        InitVkStruct(physicalDeviceProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        InitVkStruct(m_descriptorIndexingProperties,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES);

        for (UniquePtr<VulkanDeviceExtension>& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->BeforePhysicalDeviceProperties(physicalDeviceProperties2);
            }
        }

        physicalDeviceProperties2.pNext = &m_descriptorIndexingProperties;
        vkGetPhysicalDeviceProperties2(m_gpu, &physicalDeviceProperties2);

        for (UniquePtr<VulkanDeviceExtension>& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->AfterPhysicalDeviceProperties();
            }
        }
    }

    // Optional ray-tracing features require the complete enabled dependency chain.
    m_extensionFlags.hasAccelerationStructure &=
        m_extensionFlags.hasBufferDeviceAddress && m_extensionFlags.hasDeferredHostOperation;
    m_extensionFlags.hasRaytracingPipeline &= m_extensionFlags.hasAccelerationStructure;
    m_extensionFlags.hasRayQuery &= m_extensionFlags.hasAccelerationStructure;
    for (auto& extension : extensionArray)
    {
        const NameID name = extension->GetName();
        if ((name == NameID(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
             !m_extensionFlags.hasAccelerationStructure) ||
            (name == NameID(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
             !m_extensionFlags.hasRaytracingPipeline) ||
            (name == NameID(VK_KHR_RAY_QUERY_EXTENSION_NAME) && !m_extensionFlags.hasRayQuery))
        {
            extension->SetSupport(false);
        }
    }
    SetupDevice(extensionArray);

    m_pFenceManager    = ZEN_NEW() VulkanFenceManager(this);
    m_pSemaphoreManger = ZEN_NEW() VulkanSemaphoreManager(this);
}

void VulkanDevice::SetupDevice(HeapVector<UniquePtr<VulkanDeviceExtension>>& extensions)
{
    VkDeviceCreateInfo deviceInfo;
    InitVkStruct(deviceInfo, VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);

    for (UniquePtr<VulkanDeviceExtension>& extension : extensions)
    {
        if (extension->IsEnabledAndSupported())
        {
            if (extension->RequiresExtensionName())
            {
                m_extensions.emplace_back(extension->GetName());
            }
            extension->BeforeCreateDevice(deviceInfo);
        }
    }

    // set up queue info
    HeapVector<VkDeviceQueueCreateInfo> deviceQueueInfos;

    int32_t graphicsQueueFamilyIndex = -1;
    int32_t computeQueueFamilyIndex  = -1;
    int32_t transferQueueFamilyIndex = -1;
    LOGI("Found {} Vulkan Queue Families", m_queueFamilyProps.size());
    uint32_t numPriorities = 0;

    for (int32_t queueFamilyIndex = 0; queueFamilyIndex < m_queueFamilyProps.size();
         queueFamilyIndex++)
    {
        VkQueueFamilyProperties const& queueFamilyProp = m_queueFamilyProps[queueFamilyIndex];
        if (queueFamilyProp.queueCount == 0)
        {
            continue;
        }
        bool isValidQueue = false;

        if (((queueFamilyProp.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
             (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) &&
            (graphicsQueueFamilyIndex == -1))
        {
            graphicsQueueFamilyIndex = queueFamilyIndex;
            isValidQueue             = true;
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
        queueInfo.queueFamilyIndex = queueFamilyIndex;
        queueInfo.queueCount       = queueFamilyProp.queueCount;
        deviceQueueInfos.emplace_back(queueInfo);
        numPriorities += queueFamilyProp.queueCount;
        LOGI("Initializing Queue Family at index {} ({})", queueFamilyIndex,
             GetQueuePropString(queueFamilyProp));
    }

    HeapVector<float> queuePriorities;
    queuePriorities.resize(numPriorities);
    float* pCurrentPriority = queuePriorities.data();

    for (int i = 0; i < deviceQueueInfos.size(); i++)
    {
        VkDeviceQueueCreateInfo& queueInfo = deviceQueueInfos[i];
        queueInfo.pQueuePriorities         = pCurrentPriority;
        const VkQueueFamilyProperties& queueFamilyProp =
            m_queueFamilyProps[queueInfo.queueFamilyIndex];

        for (int queueIndex = 0; queueIndex < queueFamilyProp.queueCount; queueIndex++)
        {
            *pCurrentPriority++ = 1.0f;
        }
    }

    HeapVector<const char*> extensionNames;
    extensionNames.reserve(m_extensions.size());

    for (NameID extension : m_extensions)
    {
        extensionNames.push_back(extension.CStr());
    }

    deviceInfo.enabledExtensionCount   = static_cast<uint32_t>(extensionNames.size());
    deviceInfo.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();
    deviceInfo.queueCreateInfoCount    = static_cast<uint32_t>(deviceQueueInfos.size());
    deviceInfo.pQueueCreateInfos       = deviceQueueInfos.data();
    deviceInfo.pEnabledFeatures        = &m_physicalDeviceFeatures; // enable all features

    const VkResult result = vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR_AND_THROW("vkCreateDevice failed: {}", int32_t(result));
    }
    LOGI("Vulkan Device Created");

    // display extension info
    for (NameID extension : m_extensions)
    {
        LOGI("Enabled Device Extension: {}", extension.CStr());
    }

    // load device func
    volkLoadDevice(m_device);
    // Keep native compilation data for pipeline recreation during this device's lifetime.
    // Flags stay zero so the driver synchronizes concurrent pipeline creation internally.
    VkPipelineCacheCreateInfo pipelineCacheInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    const VkResult cacheResult =
        vkCreatePipelineCache(m_device, &pipelineCacheInfo, nullptr, &m_pipelineCache);
    if (cacheResult != VK_SUCCESS)
    {
        m_pipelineCache = VK_NULL_HANDLE;
        LOGW("Vulkan pipeline cache unavailable ({}); creating pipelines without a cache",
             int32_t(cacheResult));
    }
    // setup queues
    m_pGfxQueue = ZEN_NEW() VulkanQueue(this, graphicsQueueFamilyIndex);

    if (computeQueueFamilyIndex == -1)
    {
        computeQueueFamilyIndex = graphicsQueueFamilyIndex;
    }

    m_pComputeQueue = ZEN_NEW() VulkanQueue(this, computeQueueFamilyIndex);

    if (transferQueueFamilyIndex == -1)
    {
        transferQueueFamilyIndex = computeQueueFamilyIndex;
    }

    m_pTransferQueue = ZEN_NEW() VulkanQueue(this, transferQueueFamilyIndex);
}

void VulkanDevice::SetObjectName(VkObjectType type, uint64_t handle, NameID name)
{
    VkDebugUtilsObjectNameInfoEXT info;
    InitVkStruct(info, VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);
    info.objectType   = type;
    info.objectHandle = handle;
    info.pObjectName  = name.IsNone() ? nullptr : name.CStr();
    if (GVulkanRHI->GetInstanceExtensionFlags().hasDebugUtils &&
        vkSetDebugUtilsObjectNameEXT != nullptr)
    {
        vkSetDebugUtilsObjectNameEXT(m_device, &info);
    }
}

void VulkanDevice::WaitForIdle()
{
    if (m_device == VK_NULL_HANDLE)
    {
        return;
    }
    const VkResult result = vkDeviceWaitIdle(m_device);

    if (result != VK_SUCCESS)
    {
        // Device loss also ends the lifetime wait, but does not establish valid contents/serials.
        LOGE("Vulkan device idle wait failed: {}", int32_t(result));
        if (GVulkanRHI->GetDevice() == this)
        {
            GVulkanRHI->BlockSubmissions();
        }
        return;
    }

    // GVulkanRHI->GetLegacyImmediateCmdContext()->GetCmdBufferManager()->RefreshFenceStatus();
    for (uint32_t i = 0; i < ToUnderlying(RHICommandContextType::eMax); i++)
    {
        if (auto* queue = GetQueue(static_cast<RHICommandContextType>(i)))
        {
            queue->ProcessPendingWorkloads(0);
        }
    }
}

void VulkanDevice::Destroy()
{
    ZEN_DELETE(m_pGfxQueue);
    ZEN_DELETE(m_pComputeQueue);
    ZEN_DELETE(m_pTransferQueue);

    if (m_pSemaphoreManger != nullptr)
    {
        m_pSemaphoreManger->Destroy();
        ZEN_DELETE(m_pSemaphoreManger);
    }

    if (m_pFenceManager != nullptr)
    {
        m_pFenceManager->Destroy();
        ZEN_DELETE(m_pFenceManager);
    }

    if (m_device != VK_NULL_HANDLE)
    {
        if (m_pipelineCache != VK_NULL_HANDLE)
        {
            vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
            m_pipelineCache = VK_NULL_HANDLE;
        }
        vkDestroyDevice(m_device, nullptr);
    }
}

bool VulkanRHI::IsTransferQueueSharedWithGraphics() const
{
    return m_pDevice->GetTransferQueue()->GetVkHandle() == m_pDevice->GetGfxQueue()->GetVkHandle();
}

uint64_t VulkanRHI::GetLastSubmittedSerial(RHICommandContextType contextType) const
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);

    return pQueue != nullptr ? pQueue->GetLastSubmittedSerial() : 0;
}

uint64_t VulkanRHI::GetLastCompletedSerial(RHICommandContextType contextType)
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);

    if (pQueue != nullptr && !m_submissionBlocked)
    {
        pQueue->ProcessPendingWorkloads(0);
    }

    return pQueue != nullptr ? pQueue->GetLastCompletedSerial() : 0;
}

bool VulkanRHI::WaitForSubmission(RHICommandContextType contextType,
                                  uint64_t submissionSerial,
                                  uint64_t timeoutNS)
{
    VulkanQueue* pQueue = m_pDevice->GetQueue(contextType);
    return pQueue != nullptr && pQueue->WaitForSubmission(submissionSerial, timeoutNS);
}

void VulkanRHI::WaitDeviceIdle()
{
    if (m_pDevice == nullptr || m_pDevice->GetVkHandle() == VK_NULL_HANDLE)
    {
        return;
    }
    m_pDevice->WaitForIdle();
}
} // namespace zen
