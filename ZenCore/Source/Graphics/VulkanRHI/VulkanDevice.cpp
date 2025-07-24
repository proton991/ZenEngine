#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"

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

DataFormat VulkanRHI::GetSupportedDepthFormat()
{
    VkFormat defaulFormat{VK_FORMAT_D16_UNORM};
    const std::vector<VkFormat> formatList = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                              VK_FORMAT_D24_UNORM_S8_UINT,
                                              VK_FORMAT_D16_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (auto& format : formatList)
    {
        VkFormatProperties formatProps;
        vkGetPhysicalDeviceFormatProperties(m_device->GetPhysicalDeviceHandle(), format,
                                            &formatProps);
        if (formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        {
            defaulFormat = format;
            break;
        }
    }

    return static_cast<DataFormat>(defaulFormat);
}


const GPUInfo& VulkanRHI::QueryGPUInfo() const
{
    return m_gpuInfo;
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
            if (extension->IsEnabledAndSupported())
            {
                extension->AfterPhysicalDeviceFeatures();
            }
        }

        VkPhysicalDeviceProperties2 physicalDeviceProperties2;
        InitVkStruct(physicalDeviceProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        InitVkStruct(m_descriptorIndexingProperties,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->BeforePhysicalDeviceProperties(physicalDeviceProperties2);
            }
        }
        physicalDeviceProperties2.pNext = &m_descriptorIndexingProperties;
        vkGetPhysicalDeviceProperties2(m_gpu, &physicalDeviceProperties2);
        for (auto& extension : extensionArray)
        {
            if (extension->IsEnabledAndSupported())
            {
                extension->AfterPhysicalDeviceProperties();
            }
        }
    }

    SetupDevice(extensionArray);

    m_fenceManager    = new VulkanFenceManager(this);
    m_semaphoreManger = new VulkanSemaphoreManager(this);

    m_immediateContext = new VulkanCommandListContext(m_RHI);
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
    // for glsl shader debug printf ext
    m_extensions.emplace_back(VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME);
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
        bool isValidQueue           = false;
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
    deviceInfo.pEnabledFeatures        = &m_physicalDeviceFeatures; // enable all features

    VKCHECK(vkCreateDevice(m_gpu, &deviceInfo, nullptr, &m_device));
    LOGI("Vulkan Device Created");
    // display extension info
    for (const auto& extension : m_extensions)
    {
        LOGI("Enabled Device Extension: {}", extension);
    }
    // load device func
    volkLoadDevice(m_device);
    // setup queues
    m_gfxQueue = new VulkanQueue(this, graphicsQueueFamilyIndex);
    if (computeQueueFamilyIndex == -1)
    {
        computeQueueFamilyIndex = graphicsQueueFamilyIndex;
    }
    m_computeQueue = new VulkanQueue(this, computeQueueFamilyIndex);
    if (transferQueueFamilyIndex == -1)
    {
        transferQueueFamilyIndex = computeQueueFamilyIndex;
    }
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

void VulkanDevice::SubmitCommandsAndFlush()
{
    VulkanCommandBufferManager* mgr = m_immediateContext->GetCmdBufferManager();
    if (mgr->HasPendingUploadCmdBuffer())
    {
        mgr->SubmitUploadCmdBuffer();
    }
    if (mgr->HasPendingActiveCmdBuffer())
    {
        mgr->SubmitActiveCmdBuffer();
    }
    mgr->SetupNewActiveCmdBuffer();
}

void VulkanDevice::WaitForIdle()
{
    VKCHECK(vkDeviceWaitIdle(m_device));
    m_immediateContext->GetCmdBufferManager()->RefreshFenceStatus();
}

void VulkanDevice::Destroy()
{
    m_semaphoreManger->Destroy();
    delete m_semaphoreManger;

    delete m_immediateContext;

    m_fenceManager->Destroy();
    delete m_fenceManager;
    vkDestroyDevice(m_device, nullptr);
}

void VulkanRHI::SubmitAllGPUCommands()
{
    for (auto* ctx : m_cmdListContexts)
    {
        auto* vkCtx = dynamic_cast<VulkanCommandListContext*>(ctx);
        if (vkCtx->GetCmdBufferManager()->HasPendingActiveCmdBuffer())
        {
            vkCtx->GetCmdBufferManager()->SubmitActiveCmdBuffer();
            vkCtx->GetCmdBufferManager()->SetupNewActiveCmdBuffer();
        }
        if (vkCtx->GetCmdBufferManager()->HasPendingUploadCmdBuffer())
        {
            vkCtx->GetCmdBufferManager()->SubmitUploadCmdBuffer();
        }
    }
    auto* immediateMgr = m_device->GetImmediateCmdContext()->GetCmdBufferManager();
    if (immediateMgr->HasPendingUploadCmdBuffer())
    {
        immediateMgr->SubmitActiveCmdBuffer();
        immediateMgr->SetupNewActiveCmdBuffer();
    }
    if (immediateMgr->HasPendingUploadCmdBuffer())
    {
        immediateMgr->SubmitUploadCmdBuffer();
    }
}


void VulkanRHI::WaitDeviceIdle()
{
    m_device->WaitForIdle();
}
} // namespace zen::rhi
