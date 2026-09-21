#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanExtension.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"

namespace zen
{
VulkanMemoryAllocator* GVkMemAllocator = nullptr;
VulkanRHI* GVulkanRHI                  = nullptr;

namespace
{
static bool HasExtensionEnabled(const HeapVector<NameID>& extensions, NameID extensionName)
{
    return std::find(extensions.begin(), extensions.end(), extensionName) != extensions.end();
}

static const char* GetPhysicalDeviceTypeName(VkPhysicalDeviceType deviceType)
{
    const char* result{};

    switch (deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: result = "Other"; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: result = "Integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: result = "Discrete"; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: result = "Virtual"; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: result = "CPU"; break;
        default: result = "Unknown"; break;
    }

    return result;
}

static int64_t GetPhysicalDeviceTypeScore(VkPhysicalDeviceType deviceType)
{
    int64_t result{};

    switch (deviceType)
    {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: result = 40000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: result = 30000; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: result = 20000; break;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER: result = 15000; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: result = 1000; break;
        default: result = 0; break;
    }

    return result;
}

struct VulkanQueueFamilySummary
{
    bool hasGraphics{false};
    bool hasDedicatedCompute{false};
    bool hasDedicatedTransfer{false};
    bool hasAsyncTransfer{false};
};

struct VulkanPhysicalDeviceCandidateInfo
{
    bool isValid{false};
    std::string unsupportedReason;
    int64_t score{0};
    uint64_t deviceLocalMemoryBytes{0};
    VkPhysicalDeviceProperties properties{};
    VulkanQueueFamilySummary queueSummary{};
    bool hasDescriptorIndexing{false};
    bool hasDynamicRendering{false};
    bool hasTimelineSemaphore{false};
    bool hasBufferDeviceAddress{false};
    bool hasAccelerationStructure{false};
    bool hasRayTracingPipeline{false};
    bool hasRayQuery{false};
    bool hasGeometryShader{false};
    bool hasSamplerAnisotropy{false};
};

static VulkanQueueFamilySummary GetQueueFamilySummary(VkPhysicalDevice physicalDevice)
{
    VulkanQueueFamilySummary result{};

    VulkanQueueFamilySummary summary{};

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);

    if (queueFamilyCount == 0)
    {
        result = summary;
    }
    else
    {
        HeapVector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                 queueFamilyProperties.data());

        for (const VkQueueFamilyProperties& queueFamilyProperty : queueFamilyProperties)
        {
            if (queueFamilyProperty.queueCount == 0)
            {
                continue;
            }

            const bool hasGraphics =
                (queueFamilyProperty.queueFlags & VK_QUEUE_GRAPHICS_BIT) == VK_QUEUE_GRAPHICS_BIT;
            const bool hasCompute =
                (queueFamilyProperty.queueFlags & VK_QUEUE_COMPUTE_BIT) == VK_QUEUE_COMPUTE_BIT;
            const bool hasTransfer =
                (queueFamilyProperty.queueFlags & VK_QUEUE_TRANSFER_BIT) == VK_QUEUE_TRANSFER_BIT;

            if (hasGraphics)
            {
                summary.hasGraphics = true;
            }

            if (hasCompute && !hasGraphics)
            {
                summary.hasDedicatedCompute = true;
            }

            if (hasTransfer && !hasGraphics)
            {
                summary.hasAsyncTransfer = true;
            }

            if (hasTransfer && !hasGraphics && !hasCompute)
            {
                summary.hasDedicatedTransfer = true;
            }
        }

        result = summary;
    }

    return result;
}

static uint64_t GetDeviceLocalMemoryBytes(VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    uint64_t deviceLocalMemoryBytes = 0;

    for (uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i)
    {
        const VkMemoryHeap& memoryHeap = memoryProperties.memoryHeaps[i];

        if ((memoryHeap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            deviceLocalMemoryBytes += memoryHeap.size;
        }
    }

    return deviceLocalMemoryBytes;
}

// static VulkanRequiredDeviceExtensionSupport QueryRequiredDeviceExtensionSupport(
//     VkPhysicalDevice physicalDevice)
// {
//     VulkanRequiredDeviceExtensionSupport extensionSupport{};
//     const HeapVector<VkExtensionProperties> supportedExtensions =
//         VulkanDeviceExtension::GetSupportedExtensions(physicalDevice);
//     extensionSupport.hasSwapchain =
//         HasSupportedExtension(supportedExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
//     return extensionSupport;
// }

static VulkanPhysicalDeviceCandidateInfo EvaluatePhysicalDeviceCandidate(
    VkPhysicalDevice physicalDevice)
{
    VulkanPhysicalDeviceCandidateInfo result{};

    static constexpr uint64_t cMiB = 1024ull * 1024ull;

    VulkanPhysicalDeviceCandidateInfo candidateInfo{};
    vkGetPhysicalDeviceProperties(physicalDevice, &candidateInfo.properties);
    candidateInfo.queueSummary = GetQueueFamilySummary(physicalDevice);

    candidateInfo.unsupportedReason = VulkanDevice::GetUnsupportedReason(physicalDevice);
    if (!candidateInfo.unsupportedReason.empty())
    {
        result = candidateInfo;
    }
    else
    {
        VkPhysicalDeviceFeatures2 physicalDeviceFeatures2{};
        InitVkStruct(physicalDeviceFeatures2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);

        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures{};
        InitVkStruct(descriptorIndexingFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES);
        VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRenderingFeatures{};
        InitVkStruct(dynamicRenderingFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR);
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineSemaphoreFeatures{};
        InitVkStruct(timelineSemaphoreFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddressFeatures{};
        InitVkStruct(bufferDeviceAddressFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES);
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructureFeatures{};
        InitVkStruct(accelerationStructureFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR);
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipelineFeatures{};
        InitVkStruct(rayTracingPipelineFeatures,
                     VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR);
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{};
        InitVkStruct(rayQueryFeatures, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR);

        physicalDeviceFeatures2.pNext       = &descriptorIndexingFeatures;
        descriptorIndexingFeatures.pNext    = &dynamicRenderingFeatures;
        dynamicRenderingFeatures.pNext      = &timelineSemaphoreFeatures;
        timelineSemaphoreFeatures.pNext     = &bufferDeviceAddressFeatures;
        bufferDeviceAddressFeatures.pNext   = &accelerationStructureFeatures;
        accelerationStructureFeatures.pNext = &rayTracingPipelineFeatures;
        rayTracingPipelineFeatures.pNext    = &rayQueryFeatures;
        rayQueryFeatures.pNext              = nullptr;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &physicalDeviceFeatures2);

        candidateInfo.hasGeometryShader =
            physicalDeviceFeatures2.features.geometryShader == VK_TRUE;
        candidateInfo.hasSamplerAnisotropy =
            physicalDeviceFeatures2.features.samplerAnisotropy == VK_TRUE;
        candidateInfo.hasDescriptorIndexing =
            descriptorIndexingFeatures.runtimeDescriptorArray == VK_TRUE &&
            descriptorIndexingFeatures.descriptorBindingPartiallyBound == VK_TRUE &&
            descriptorIndexingFeatures.descriptorBindingUpdateUnusedWhilePending == VK_TRUE &&
            descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount == VK_TRUE;
        candidateInfo.hasDynamicRendering  = dynamicRenderingFeatures.dynamicRendering == VK_TRUE;
        candidateInfo.hasTimelineSemaphore = timelineSemaphoreFeatures.timelineSemaphore == VK_TRUE;
        candidateInfo.hasBufferDeviceAddress =
            bufferDeviceAddressFeatures.bufferDeviceAddress == VK_TRUE;
        candidateInfo.hasAccelerationStructure =
            accelerationStructureFeatures.accelerationStructure == VK_TRUE;
        candidateInfo.hasRayTracingPipeline =
            rayTracingPipelineFeatures.rayTracingPipeline == VK_TRUE;
        candidateInfo.hasRayQuery = rayQueryFeatures.rayQuery == VK_TRUE;

        candidateInfo.deviceLocalMemoryBytes = GetDeviceLocalMemoryBytes(physicalDevice);
        candidateInfo.isValid                = true;

        candidateInfo.score += GetPhysicalDeviceTypeScore(candidateInfo.properties.deviceType);
        candidateInfo.score +=
            static_cast<int64_t>(candidateInfo.deviceLocalMemoryBytes / (1024ull * cMiB)) * 250;
        candidateInfo.score +=
            static_cast<int64_t>(candidateInfo.properties.limits.maxImageDimension2D / 1024);
        candidateInfo.score +=
            static_cast<int64_t>(candidateInfo.properties.limits.maxPushConstantsSize / 32);

        if (candidateInfo.queueSummary.hasDedicatedCompute)
        {
            candidateInfo.score += 5000;
        }

        if (candidateInfo.queueSummary.hasDedicatedTransfer)
        {
            candidateInfo.score += 3000;
        }

        if (candidateInfo.queueSummary.hasAsyncTransfer)
        {
            candidateInfo.score += 1000;
        }

        if (candidateInfo.hasDescriptorIndexing)
        {
            candidateInfo.score += 4000;
        }

        if (candidateInfo.hasDynamicRendering)
        {
            candidateInfo.score += 2500;
        }

        if (candidateInfo.hasTimelineSemaphore)
        {
            candidateInfo.score += 2500;
        }

        if (candidateInfo.hasBufferDeviceAddress)
        {
            candidateInfo.score += 2000;
        }

        if (candidateInfo.hasAccelerationStructure)
        {
            candidateInfo.score += 1500;
        }

        if (candidateInfo.hasRayTracingPipeline)
        {
            candidateInfo.score += 1500;
        }

        if (candidateInfo.hasRayQuery)
        {
            candidateInfo.score += 1000;
        }

        if (candidateInfo.hasGeometryShader)
        {
            candidateInfo.score += 500;
        }

        if (candidateInfo.hasSamplerAnisotropy)
        {
            candidateInfo.score += 250;
        }

        result = candidateInfo;
    }

    return result;
}
} // namespace

struct VulkanLayer
{
    VkLayerProperties layerProperties;
    HeapVector<VkExtensionProperties> layerExtensions;
};

VKAPI_ATTR VkBool32 VKAPI_CALL
DebugUtilsMessengerCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                            VkDebugUtilsMessageTypeFlagsEXT messageType,
                            const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
                            void* pUserData)
{
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        LOGW("{} - {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName,
             pCallbackData->pMessage);
    }
    else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        LOGE("{} - {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName,
             pCallbackData->pMessage)
    }

    return VK_FALSE;
}

void VulkanRHI::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& dbgMessengerCI)
{
    constexpr VkDebugUtilsMessageSeverityFlagsEXT messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    constexpr VkDebugUtilsMessageTypeFlagsEXT messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    InitVkStruct(dbgMessengerCI, VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
    dbgMessengerCI.pNext           = nullptr;
    dbgMessengerCI.flags           = 0;
    dbgMessengerCI.messageSeverity = messageSeverity;
    dbgMessengerCI.messageType     = messageType;
    dbgMessengerCI.pfnUserCallback = DebugUtilsMessengerCallback;
    dbgMessengerCI.pUserData       = this;
}

static HeapVector<VulkanLayer> GetSupportedLayers()
{
    HeapVector<VulkanLayer> layers;
    HeapVector<VkLayerProperties> layerProperties;

    uint32_t count = 0;
    VKCHECK(vkEnumerateInstanceLayerProperties(&count, nullptr));

    if (count > 0)
    {
        layers.resize(count);
        layerProperties.resize(count);
        VKCHECK(vkEnumerateInstanceLayerProperties(&count, layerProperties.data()));

        for (uint32_t i = 0; i < count; i++)
        {
            layers[i].layerProperties = layerProperties[i];
            layers[i].layerExtensions = VulkanInstanceExtension::GetSupportedInstanceExtensions(
                layerProperties[i].layerName);
        }
    }

    std::sort(layers.begin(), layers.end(), [](const VulkanLayer& lhs, const VulkanLayer& rhs) {
        return strcmp(lhs.layerProperties.layerName, rhs.layerProperties.layerName) < 0;
    });

    return layers;
}

static void FlagInstanceExtensionSupported(VulkanInstanceExtensionArray& instanceExtensions,
                                           NameID extensionName)
{
    for (UniquePtr<VulkanInstanceExtension>& extension : instanceExtensions)
    {
        if (extension->GetName() == extensionName)
        {
            extension->SetSupport();
        }
    }
}

void VulkanRHI::SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions)
{
    HeapVector<VulkanLayer> supportedLayers = GetSupportedLayers();

    for (VulkanLayer const& layer : supportedLayers)
    {
        LOGI("Supported Layer: {}", layer.layerProperties.layerName);
    }

    // Add Debug Layer
    const NameID debugLayerName("VK_LAYER_KHRONOS_validation");

    {
        bool valid = true;

        int index = -1;

        for (uint32_t i = 0; i < supportedLayers.size(); i++)
        {
            if (strcmp(supportedLayers[i].layerProperties.layerName, debugLayerName.CStr()) == 0)
            {
                index = i;
            }
        }

        if (index == -1)
        {
            LOGE("Requested {} layer not supported!", debugLayerName.CStr());
            valid = false;
        }

        if (valid)
        {
            m_instanceLayers.push_back(debugLayerName);

            for (VkExtensionProperties& extension : supportedLayers[index].layerExtensions)
            {
                FlagInstanceExtensionSupported(instanceExtensions, extension.extensionName);
            }
        }
    }

    for (NameID layer : m_instanceLayers)
    {
        LOGI("Enabled Instance Layer: {}", layer.CStr());
    }
}

void VulkanRHI::SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions)
{
    for (UniquePtr<VulkanInstanceExtension>& extension : instanceExtensions)
    {
        if (extension->IsEnabledAndSupported())
        {
            m_instanceExtensions.emplace_back(extension->GetName());
            LOGI("Enabled Instance Extension: {}", extension->GetName().CStr());
        }
    }
}

void VulkanRHI::CreateInstance()
{
    VkApplicationInfo appInfo;
    InitVkStruct(appInfo, VK_STRUCTURE_TYPE_APPLICATION_INFO);
    appInfo.apiVersion         = ZEN_VK_API_VERSION;
    appInfo.applicationVersion = ZEN_VK_APP_VERSION;
    appInfo.engineVersion      = ZEN_ENGINE_VERSION;
    appInfo.pApplicationName   = "ZenEngineRHI";
    appInfo.pEngineName        = "ZenEngine";

    VulkanInstanceExtensionArray instanceExtensions =
        VulkanInstanceExtension::GetEnabledInstanceExtensions(m_instanceExtensionFlags);

    SetupInstanceLayers(instanceExtensions);

    SetupInstanceExtensions(instanceExtensions);

    VkDebugUtilsMessengerCreateInfoEXT debugMessengerCI;
    PopulateDebugMessengerCreateInfo(debugMessengerCI);

    HeapVector<VkValidationFeatureEnableEXT> validationFeatureEnables = {
        VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT};

    VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures    = validationFeatureEnables.data();
    validationFeatures.pNext = m_instanceExtensionFlags.hasDebugUtils ? &debugMessengerCI : nullptr;

    HeapVector<const char*> layerNames;
    layerNames.reserve(m_instanceLayers.size());

    for (NameID layer : m_instanceLayers)
    {
        layerNames.push_back(layer.CStr());
    }

    HeapVector<const char*> extensionNames;
    extensionNames.reserve(m_instanceExtensions.size());

    for (NameID extension : m_instanceExtensions)
    {
        extensionNames.push_back(extension.CStr());
    }

    VkInstanceCreateInfo instanceInfo;
    InitVkStruct(instanceInfo, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    if (HasExtensionEnabled(m_instanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    instanceInfo.pApplicationInfo        = &appInfo;
    instanceInfo.enabledLayerCount       = static_cast<uint32_t>(layerNames.size());
    instanceInfo.ppEnabledLayerNames     = layerNames.empty() ? nullptr : layerNames.data();
    instanceInfo.enabledExtensionCount   = static_cast<uint32_t>(extensionNames.size());
    instanceInfo.ppEnabledExtensionNames = extensionNames.empty() ? nullptr : extensionNames.data();
    const bool validationEnabled =
        HasExtensionEnabled(m_instanceLayers, NameID("VK_LAYER_KHRONOS_validation"));
    instanceInfo.pNext = validationEnabled ?
        static_cast<const void*>(&validationFeatures) :
        (m_instanceExtensionFlags.hasDebugUtils ? &debugMessengerCI : nullptr);

    const VkResult result = vkCreateInstance(&instanceInfo, nullptr, &m_instance);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR_AND_THROW("vkCreateInstance failed: {}", int32_t(result));
    }
    volkLoadInstance(m_instance);
    // setup debug messenger callback
    if (m_instanceExtensionFlags.hasDebugUtils)
    {
        const VkResult messengerResult =
            vkCreateDebugUtilsMessengerEXT(m_instance, &debugMessengerCI, nullptr, &m_messenger);
        if (messengerResult != VK_SUCCESS)
        {
            LOG_ERROR_AND_THROW("vkCreateDebugUtilsMessengerEXT failed: {}",
                                int32_t(messengerResult));
        }
    }

    VERIFY_EXPR(m_instance != VK_NULL_HANDLE);
    LOGI("Vulkan Instance Created");
}

void VulkanRHI::SelectGPU()
{
    uint32_t gpuCount = 0;
    VkResult result   = vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr);

    if (result != VK_SUCCESS || gpuCount == 0)
    {
        LOG_ERROR_AND_THROW("No Vulkan physical devices are available (result {})",
                            int32_t(result));
    }
    HeapVector<VkPhysicalDevice> physicalDevices;
    physicalDevices.resize(gpuCount);
    result = vkEnumeratePhysicalDevices(m_instance, &gpuCount, physicalDevices.data());
    if (result != VK_SUCCESS)
    {
        LOG_ERROR_AND_THROW("vkEnumeratePhysicalDevices failed: {}", int32_t(result));
    }

    uint32_t selectedIndex   = 0;
    bool foundValidCandidate = false;
    VulkanPhysicalDeviceCandidateInfo selectedCandidateInfo{};

    for (uint32_t i = 0; i < physicalDevices.size(); i++)
    {
        const VulkanPhysicalDeviceCandidateInfo candidateInfo =
            EvaluatePhysicalDeviceCandidate(physicalDevices[i]);

        LOGI("Found Vulkan Compatible GPU: {} ({})", candidateInfo.properties.deviceName,
             GetPhysicalDeviceTypeName(candidateInfo.properties.deviceType));

        if (!candidateInfo.isValid)
        {
            LOGW("Rejecting GPU '{}': {}", candidateInfo.properties.deviceName,
                 candidateInfo.unsupportedReason);
            continue;
        }

        LOGI(
            "GPU Candidate '{}': score={}, localMemory={} MiB, dedicatedCompute={}, dedicatedTransfer={}, asyncTransfer={}, descriptorIndexing={}, dynamicRendering={}, timelineSemaphore={}, bufferDeviceAddress={}, rayTracingPipeline={}, rayQuery={}, geometryShader={}",
            candidateInfo.properties.deviceName, candidateInfo.score,
            candidateInfo.deviceLocalMemoryBytes / (1024ull * 1024ull),
            candidateInfo.queueSummary.hasDedicatedCompute,
            candidateInfo.queueSummary.hasDedicatedTransfer,
            candidateInfo.queueSummary.hasAsyncTransfer, candidateInfo.hasDescriptorIndexing,
            candidateInfo.hasDynamicRendering, candidateInfo.hasTimelineSemaphore,
            candidateInfo.hasBufferDeviceAddress, candidateInfo.hasRayTracingPipeline,
            candidateInfo.hasRayQuery, candidateInfo.hasGeometryShader);

        const bool isBetterCandidate = !foundValidCandidate ||
            candidateInfo.score > selectedCandidateInfo.score ||
            (candidateInfo.score == selectedCandidateInfo.score &&
             candidateInfo.deviceLocalMemoryBytes > selectedCandidateInfo.deviceLocalMemoryBytes);

        if (isBetterCandidate)
        {
            selectedIndex         = i;
            selectedCandidateInfo = candidateInfo;
            foundValidCandidate   = true;
        }
    }

    if (!foundValidCandidate)
    {
        LOG_ERROR_AND_THROW(
            "No Vulkan device satisfies the required Vulkan 1.2, dynamic rendering and descriptor indexing profile; see rejection reasons above.");
    }

    LOGI("Selected Vulkan GPU: {} ({}, score={}, localMemory={} MiB)",
         selectedCandidateInfo.properties.deviceName,
         GetPhysicalDeviceTypeName(selectedCandidateInfo.properties.deviceType),
         selectedCandidateInfo.score,
         selectedCandidateInfo.deviceLocalMemoryBytes / (1024ull * 1024ull));

    m_pDevice = ZEN_NEW() VulkanDevice(physicalDevices[selectedIndex]);
}

VulkanRHI::VulkanRHI() : m_resourceAllocator(ZEN_DEFAULT_PAGESIZE, false)
{
#if defined(ZEN_MACOS)
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
#endif

    if (volkInitialize() != VK_SUCCESS)
    {
        LOG_ERROR_AND_THROW("Failed to initialize volk!");
    }

    m_resourceAllocator.Init();
    GVkMemAllocator = ZEN_NEW() VulkanMemoryAllocator();

    GVulkanRHI = this;
}

VkPhysicalDevice VulkanRHI::GetPhysicalDevice() const
{
    return m_pDevice->GetPhysicalDeviceHandle();
}

VkDevice VulkanRHI::GetVkDevice() const
{
    return m_pDevice->GetVkHandle();
}

IRHICommandContext* VulkanRHI::GetCommandContext(RHICommandContextType contextType)
{
    return ZEN_NEW() FVulkanCommandListContext(contextType, m_pDevice);
}

IRHICommandContext* VulkanRHI::GetTransferCommandContext()
{
    if (m_pTransferContext == nullptr)
    {
        m_pTransferContext =
            ZEN_NEW() FVulkanCommandListContext(RHICommandContextType::eTransfer, m_pDevice);
    }

    return m_pTransferContext;
}

void VulkanRHI::Init()
{
    try
    {
        m_pResourceFactory = ZEN_NEW() VulkanResourceFactory();

        CreateInstance();
        SelectGPU();
        m_pDevice->Init();

        m_gpuInfo.supportGeometryShader = m_pDevice->GetPhysicalDeviceFeatures().geometryShader;
        m_gpuInfo.uniformBufferAlignment =
            m_pDevice->GetPhysicalDeviceProperties().limits.minUniformBufferOffsetAlignment;
        m_gpuInfo.storageBufferAlignment =
            m_pDevice->GetPhysicalDeviceProperties().limits.minStorageBufferOffsetAlignment;

        // m_vkMemAllocator->Init(m_instance, m_device->GetPhysicalDeviceHandle(),
        //                        m_device->GetVkHandle());

        GVkMemAllocator->Init(m_instance, m_pDevice->GetPhysicalDeviceHandle(),
                              m_pDevice->GetVkHandle(),
                              m_pDevice->GetExtensionFlags().hasBufferDeviceAddress != 0);

        m_pDescriptorPoolManager2        = ZEN_NEW() VulkanDescriptorPoolManager2(m_pDevice);
        m_pBindlessDescriptorPoolManager = ZEN_NEW() VulkanBindlessDescriptorPoolManager();
        m_pBindlessDescriptorPoolManager->Init();
        m_pUniformBufferAllocator = ZEN_NEW() VulkanUniformBufferAllocator();
        m_pUniformBufferAllocator->Init(RHIFrameState::kMaxFramesInFlight, 4 * 1024 * 1024, 8);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
}

RHIBindlessHandle VulkanRHI::RegisterBindlessResource(RHIResource* pResource, uint32_t slotIndex)
{
    RHIBindlessHandle handle;
    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->RegisterBindlessResource(pResource, slotIndex, &handle);
    }
    return handle;
}

bool VulkanRHI::UnregisterBindlessResource(RHIBindlessHandle handle)
{
    return m_pBindlessDescriptorPoolManager != nullptr &&
        m_pBindlessDescriptorPoolManager->UnregisterBindlessResource(handle);
}

bool VulkanRHI::IsBindlessResourceRegistered(RHIBindlessHandle handle)
{
    return m_pBindlessDescriptorPoolManager != nullptr &&
        m_pBindlessDescriptorPoolManager->IsRegistered(handle);
}

void VulkanRHI::CollectRetiredBindlessResources()
{
    if (m_pDevice != nullptr && !AreSubmissionsBlocked())
    {
        QueryLastCompletedSerial(RHICommandContextType::eGraphics);
        QueryLastCompletedSerial(RHICommandContextType::eAsyncCompute);
        QueryLastCompletedSerial(RHICommandContextType::eTransfer);
    }
    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->CollectRetiredResources();
    }
    m_lifetimeTracker.Collect();
}

void VulkanRHI::BeginFrame()
{
    GetRHIThread().CheckOwnership();
    CollectRetiredBindlessResources();
    VERIFY_EXPR(m_pDescriptorPoolManager2 != nullptr);
    VERIFY_EXPR(m_pUniformBufferAllocator != nullptr);
    m_pDescriptorPoolManager2->BeginFrame(
        static_cast<uint32_t>(ToValue(GRHIFrameState.GetFrameNumber())));
    // RenderDevice has waited for this frame slot before reusing its uniform storage.
    m_pUniformBufferAllocator->BeginFrame(ToIndex(GRHIFrameState.GetFrameSlot()));
}

void VulkanRHI::Destroy()
{
    WaitDeviceIdle();
    DestroyPlatformCommandListPool();

    // delete m_vkMemAllocator;
    // ZEN_DELETE(m_pLegacyImmediateContext);
    // ZEN_DELETE(m_pLegacyImmediateCommandList);

    // if (m_pTransferContext != nullptr)
    // {
    //     ZEN_DELETE(m_pTransferContext);
    //     m_pTransferContext = nullptr;
    // }

    if (m_pUniformBufferAllocator != nullptr)
    {
        m_pUniformBufferAllocator->Destroy();
        ZEN_DELETE(m_pUniformBufferAllocator);
        m_pUniformBufferAllocator = nullptr;
    }

    if (m_pDescriptorPoolManager2 != nullptr)
    {
        m_pDescriptorPoolManager2->Destroy();
        ZEN_DELETE(m_pDescriptorPoolManager2);
        m_pDescriptorPoolManager2 = nullptr;
    }

    if (m_pBindlessDescriptorPoolManager != nullptr)
    {
        m_pBindlessDescriptorPoolManager->Destroy();
        ZEN_DELETE(m_pBindlessDescriptorPoolManager);
        m_pBindlessDescriptorPoolManager = nullptr;
    }

    m_lifetimeTracker.Destroy();
    ZEN_DELETE(GVkMemAllocator);
    GVkMemAllocator = nullptr;

    for (const std::pair<const uint32_t, VkRenderPass>& kv : m_renderPassCache)
    {
        vkDestroyRenderPass(m_pDevice->GetVkHandle(), kv.second, nullptr);
    }

    for (const std::pair<const uint32_t, VkFramebuffer>& kv : m_framebufferCache)
    {
        vkDestroyFramebuffer(m_pDevice->GetVkHandle(), kv.second, nullptr);
    }

    if (m_pDevice != nullptr)
    {
        m_pDevice->Destroy();
        ZEN_DELETE(m_pDevice);
        m_pDevice = nullptr;
    }

    ZEN_DELETE(m_pResourceFactory);
    m_pResourceFactory = nullptr;

    // destroy debug utils messenger
    if (m_messenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
        m_messenger = VK_NULL_HANDLE;
    }
    // destroy instance
    if (m_instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }
}
} // namespace zen
