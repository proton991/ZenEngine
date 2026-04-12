#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"

namespace zen
{
VulkanMemoryAllocator* GVkMemAllocator = nullptr;
VulkanRHI* GVulkanRHI                  = nullptr;

namespace
{
static bool HasExtensionEnabled(const HeapVector<const char*>& extensions,
                                const char* pExtensionName)
{
    return std::find_if(extensions.begin(), extensions.end(),
                        [pExtensionName](const char* pEnabledExtension) {
                            return strcmp(pEnabledExtension, pExtensionName) == 0;
                        }) != extensions.end();
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
    std::sort(layers.begin(), layers.end(), [](const VulkanLayer& a, const VulkanLayer& b) {
        return strcmp(a.layerProperties.layerName, b.layerProperties.layerName) < 0;
    });
    return layers;
}

void VulkanRHI::SetupInstanceLayers(VulkanInstanceExtensionArray& instanceExtensions)
{
    HeapVector<VulkanLayer> supportedLayers = GetSupportedLayers();
    for (const auto& layer : supportedLayers)
    {
        LOGI("Supported Layer: {}", layer.layerProperties.layerName);
    }
    auto flagExtensionSupport = [&](const char* pExtensionName) {
        for (auto& extension : instanceExtensions)
        {
            if (strcmp(extension->GetName(), pExtensionName) == 0)
            {
                extension->SetSupport();
            }
        }
    };

    auto addRequestLayer = [&](const char* pLayerName) -> bool {
        int index = -1;
        for (uint32_t i = 0; i < supportedLayers.size(); i++)
        {
            if (strcmp(supportedLayers[i].layerProperties.layerName, pLayerName) == 0)
            {
                index = i;
            }
        }
        if (index == -1)
        {
            LOGE("Requested {} layer not supported!", pLayerName);
            return false;
        }
        m_instanceLayers.push_back(pLayerName);
        for (VkExtensionProperties& extension : supportedLayers[index].layerExtensions)
        {
            flagExtensionSupport(extension.extensionName);
        }
        return true;
    };

    // Add Debug Layer
    const char* pDebugLayerName = "VK_LAYER_KHRONOS_validation";
    addRequestLayer(pDebugLayerName);
    for (auto& layer : m_instanceLayers)
    {
        LOGI("Enabled Instance Layer: {}", layer);
    }
}

void VulkanRHI::SetupInstanceExtensions(VulkanInstanceExtensionArray& instanceExtensions)
{
    for (auto& extension : instanceExtensions)
    {
        if (extension->IsEnabledAndSupported())
        {
            m_instanceExtensions.emplace_back(extension->GetName());
            LOGI("Enabled Instance Extension: {}", extension->GetName());
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

    auto instanceExtensions =
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
    validationFeatures.pNext                         = &debugMessengerCI;

    VkInstanceCreateInfo instanceInfo;
    InitVkStruct(instanceInfo, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
#if defined(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)
    if (HasExtensionEnabled(m_instanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    {
        instanceInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif
    instanceInfo.pApplicationInfo    = &appInfo;
    instanceInfo.enabledLayerCount   = static_cast<uint32_t>(m_instanceLayers.size());
    instanceInfo.ppEnabledLayerNames = m_instanceLayers.empty() ? nullptr : m_instanceLayers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(m_instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames =
        m_instanceExtensions.empty() ? nullptr : m_instanceExtensions.data();
    instanceInfo.pNext = &validationFeatures;

    VKCHECK(vkCreateInstance(&instanceInfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);
    // setup debug messenger callback
    VKCHECK(vkCreateDebugUtilsMessengerEXT(m_instance, &debugMessengerCI, nullptr, &m_messenger));

    VERIFY_EXPR(m_instance != VK_NULL_HANDLE);
    LOGI("Vulkan Instance Created");
}

void VulkanRHI::SelectGPU()
{
    uint32_t gpuCount = 0;
    VkResult result   = vkEnumeratePhysicalDevices(m_instance, &gpuCount, nullptr);
    if (result == VK_ERROR_INITIALIZATION_FAILED)
    {
        LOGE("No Vulkan compatible device found!");
        assert(false);
    }
    VKCHECK(result);
    VERIFY_EXPR(gpuCount >= 1);
    HeapVector<VkPhysicalDevice> physicalDevices;
    physicalDevices.resize(gpuCount);
    VKCHECK(vkEnumeratePhysicalDevices(m_instance, &gpuCount, physicalDevices.data()));
    // select first discrete device
    uint32_t index = 0;
    bool found     = false;
    for (uint32_t i = 0; i < physicalDevices.size(); i++)
    {
        VkPhysicalDeviceProperties deviceProperties{};
        vkGetPhysicalDeviceProperties(physicalDevices[i], &deviceProperties);
        LOGI("Found Vulkan Compatible GPU: {}", deviceProperties.deviceName);
        if (!found && deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            index = i;
            found = true;
        }
    }
    m_pDevice = ZEN_NEW() VulkanDevice(physicalDevices[index]);
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
    // m_vkMemAllocator = new VulkanMemoryAllocator();
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

LegacyRHICommandListContext* VulkanRHI::CreateLegacyCmdListContext()
{
    LegacyRHICommandListContext* pContext = ZEN_NEW() LegacyVulkanCommandListContext(this);
    m_legacyCmdListContexts.push_back(pContext);
    return pContext;
}

LegacyVulkanCommandListContext::LegacyVulkanCommandListContext(VulkanRHI* pRHI) : m_pVkRHI(pRHI)
{
    m_pCmdBufferMgr =
        ZEN_NEW() VulkanCommandBufferManager(pRHI->GetDevice(), pRHI->GetDevice()->GetGfxQueue());
}

LegacyVulkanCommandListContext::~LegacyVulkanCommandListContext()
{
    ZEN_DELETE(m_pCmdBufferMgr);
}

LegacyVulkanCommandListContext* VulkanRHI::GetLegacyImmediateCmdContext() const
{
    return static_cast<LegacyVulkanCommandListContext*>(m_pLegacyImmediateContext);
}

void VulkanRHI::Init()
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

    GVkMemAllocator->Init(m_instance, m_pDevice->GetPhysicalDeviceHandle(), m_pDevice->GetVkHandle());

    m_pDescriptorPoolManager = ZEN_NEW() VulkanDescriptorPoolManager(m_pDevice);

    m_pLegacyImmediateContext     = ZEN_NEW() LegacyVulkanCommandListContext(this);
    m_pLegacyImmediateCommandList = ZEN_NEW() LegacyVulkanCommandList(
        static_cast<LegacyVulkanCommandListContext*>(m_pLegacyImmediateContext));
}

void VulkanRHI::Destroy()
{
    WaitDeviceIdle();
    // delete m_vkMemAllocator;
    ZEN_DELETE(m_pLegacyImmediateContext);
    ZEN_DELETE(m_pLegacyImmediateCommandList);

    // if (m_pTransferContext != nullptr)
    // {
    //     ZEN_DELETE(m_pTransferContext);
    // }

    ZEN_DELETE(GVkMemAllocator);

    ZEN_DELETE(m_pDescriptorPoolManager);

    for (auto* pContext : m_legacyCmdListContexts)
    {
        ZEN_DELETE(pContext);
    }

    for (const auto& kv : m_renderPassCache)
    {
        vkDestroyRenderPass(m_pDevice->GetVkHandle(), kv.second, nullptr);
    }

    for (const auto& kv : m_framebufferCache)
    {
        vkDestroyFramebuffer(m_pDevice->GetVkHandle(), kv.second, nullptr);
    }

    m_pDevice->Destroy();
    ZEN_DELETE(m_pDevice);

    ZEN_DELETE(m_pResourceFactory);

    // destroy debug utils messenger
    vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    // destroy instance
    vkDestroyInstance(m_instance, nullptr);
}
} // namespace zen
