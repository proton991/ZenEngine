#include "Graphics/Val/Instance.h"
#include "Common/Errors.h"
#if defined(ZEN_MACOS)
#    include "Graphics/VulkanRHI/Platform/VulkanMacOSPlatform.h"
#endif
#include <array>

namespace zen::val
{
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

void PrintInstanceLayers(const std::vector<VkLayerProperties>& layers)
{
    for (const auto& layer : layers)
    {
        LOGI("Instance Layer: {}", layer.layerName);
    }
}

void PrintInstanceExtensions(const std::vector<VkExtensionProperties>& extensions)
{
    for (const auto& extension : extensions)
    {
        LOGI("Instance Extension: {}", extension.extensionName);
    }
}
static constexpr std::array<const char*, 1> ValidationLayerNames = {
    // Unified validation layer used on Desktop and Mobile platforms
    "VK_LAYER_KHRONOS_validation"};

SharedPtr<Instance> Instance::Create(const Instance::CreateInfo& createInfo)
{
    return MakeShared<Instance>(createInfo);
}

UniquePtr<Instance> Instance::CreateUnique(const Instance::CreateInfo& createInfo)
{
    return MakeUnique<Instance>(createInfo);
}

Instance::~Instance()
{
    if (m_debugMode != DebugMode::Disabled && m_debugUtilsMessenger != VK_NULL_HANDLE)
    {
        vkDestroyDebugUtilsMessengerEXT(m_handle, m_debugUtilsMessenger, nullptr);
    }
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_handle, nullptr);
    }
}

Instance::Instance(const Instance::CreateInfo& CI)
{
    uint32_t layerCount = 0;
    CHECK_VK_ERROR_AND_THROW(vkEnumerateInstanceLayerProperties(&layerCount, nullptr),
                             "Failed to enumerate layer count")
    m_supportedLayers.resize(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, m_supportedLayers.data());
    LOGI("Supported Instance Layer Count: {}", m_supportedLayers.size())
    PrintInstanceLayers(m_supportedLayers);

    EnumerateInstanceExtensions(nullptr, m_supportedExtensions);
    LOGI("Supported Instance Extension Count: {}", m_supportedExtensions.size())
    PrintInstanceExtensions(m_supportedExtensions);

    std::vector<const char*> enabledExtensions;
    if (IsExtensionSupported(m_supportedExtensions, VK_KHR_SURFACE_EXTENSION_NAME))
    {
        enabledExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

        // Enable surface extensions depending on OS
#if defined(VK_USE_PLATFORM_WIN32_KHR)
        enabledExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
        enabledExtensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
        enabledExtensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
        enabledExtensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
        enabledExtensions.push_back(VK_KHR_XCB_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_IOS_MVK)
        enabledExtensions.push_back(VK_MVK_IOS_SURFACE_EXTENSION_NAME);
#endif
#if defined(VK_USE_PLATFORM_MACOS_MVK)
        enabledExtensions.push_back("VK_EXT_metal_surface");
        enabledExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

#endif
    };
    if (IsExtensionSupported(m_supportedExtensions,
                             VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        enabledExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    if (CI.ppEnabledExtensionNames != nullptr)
    {
        for (uint32_t ext = 0; ext < CI.enabledExtensionCount; ++ext)
        {
            const auto* extName = CI.ppEnabledExtensionNames[ext];
            if (extName == nullptr)
                continue;
            if (IsExtensionSupported(m_supportedExtensions, extName))
                enabledExtensions.push_back(extName);
            else
                LOGW("Instance extension {} is not available", extName);
        }
    }
    auto apiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr)
    {
        uint32_t maxApiVersion = 0;
        vkEnumerateInstanceVersion(&maxApiVersion);
        apiVersion = maxApiVersion;
    }

    std::vector<const char*> enabledLayers;
    if (CI.enableValidation)
    {
        if (IsExtensionSupported(m_supportedExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            // Prefer VK_EXT_debug_utils
            m_debugMode = DebugMode::Utils;
        }
        else if (IsExtensionSupported(m_supportedExtensions, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
        {
            // If debug utils are unavailable (e.g. on Android), use VK_EXT_debug_report
            m_debugMode = DebugMode::Report;
        }

        for (const auto* layerName : ValidationLayerNames)
        {
            uint32_t layerVer = ~0u;
            if (!IsLayerSupported(layerName, layerVer))
            {
                LOGW("Validation layer {} is not available.", layerName)
                continue;
            }

            // Beta extensions may vary and result in a crash.
            // New enums are not supported and may cause validation error.
            if (layerVer < VK_HEADER_VERSION_COMPLETE)
            {
                LOGW("Layer {} with version {}.{}.{} is less than header version {}.{}.{}",
                     layerName, VK_API_VERSION_MAJOR(layerVer), VK_API_VERSION_MINOR(layerVer),
                     VK_API_VERSION_PATCH(layerVer),
                     VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE),
                     VK_API_VERSION_MINOR(VK_HEADER_VERSION_COMPLETE),
                     VK_API_VERSION_PATCH(VK_HEADER_VERSION_COMPLETE))
            }

            enabledLayers.push_back(layerName);

            if (m_debugMode != DebugMode::Utils)
            {
                // On Android, VK_EXT_debug_utils extension may not be supported by the loader,
                // but supported by the layer.
                std::vector<VkExtensionProperties> layerExtensions;
                if (EnumerateInstanceExtensions(layerName, layerExtensions))
                {
                    if (IsExtensionSupported(layerExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
                        m_debugMode = DebugMode::Utils;

                    if (m_debugMode == DebugMode::Disabled &&
                        IsExtensionSupported(layerExtensions, VK_EXT_DEBUG_REPORT_EXTENSION_NAME))
                        m_debugMode = DebugMode::Report; // Resort to debug report
                }
                else
                {
                    LOG_ERROR_AND_THROW("Failed to enumerate extensions for layer {}", layerName);
                }
            }
        }
        if (m_debugMode == DebugMode::Utils)
            enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        else if (m_debugMode == DebugMode::Report)
            enabledExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
        else
            LOGW(
                "Neither {} nor {} extension is available. Debug tools (validation layer message logging, performance markers, etc.) will be disabled.",
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }
    if (CI.ppEnabledLayerNames != nullptr)
    {
        for (size_t i = 0; i < CI.enabledLayerCount; ++i)
        {
            const auto* LayerName = CI.ppEnabledLayerNames[i];
            if (LayerName == nullptr)
                return;
            uint32_t layerVer = 0;
            if (IsLayerSupported(LayerName, layerVer))
                enabledLayers.push_back(LayerName);
            else
                LOGW("Instance layer {} is not available", LayerName);
        }
    }
    constexpr VkDebugUtilsMessageSeverityFlagsEXT messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    constexpr VkDebugUtilsMessageTypeFlagsEXT messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    VkDebugUtilsMessengerCreateInfoEXT dbgMessengerCI{};
    dbgMessengerCI.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbgMessengerCI.pNext           = nullptr;
    dbgMessengerCI.flags           = 0;
    dbgMessengerCI.messageSeverity = messageSeverity;
    dbgMessengerCI.messageType     = messageType;
    dbgMessengerCI.pfnUserCallback = DebugUtilsMessengerCallback;
    dbgMessengerCI.pUserData       = nullptr;

    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pNext              = nullptr; // Pointer to an extension-specific structure.
    appInfo.pApplicationName   = nullptr;
    appInfo.applicationVersion = 0; // Developer-supplied version number of the application
    appInfo.pEngineName        = "Diligent Engine";
    appInfo.engineVersion =
        1000; // Developer-supplied version number of the engine used to create the application.
    appInfo.apiVersion = apiVersion;
    enabledExtensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    VkInstanceCreateInfo vkInstanceCI{};
#if defined(ZEN_MACOS)
    vkInstanceCI.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    vkInstanceCI.sType                 = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vkInstanceCI.pNext                 = nullptr; // Pointer to an extension-specific structure.
    vkInstanceCI.pApplicationInfo      = &appInfo;
    vkInstanceCI.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    vkInstanceCI.ppEnabledExtensionNames =
        enabledExtensions.empty() ? nullptr : enabledExtensions.data();
    vkInstanceCI.enabledLayerCount   = static_cast<uint32_t>(enabledLayers.size());
    vkInstanceCI.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();
    vkInstanceCI.pNext               = &dbgMessengerCI;

    CHECK_VK_ERROR_AND_THROW(vkCreateInstance(&vkInstanceCI, nullptr, &m_handle),
                             "Failed to create Vulkan instance");
    volkLoadInstance(m_handle);
    m_enabledExtensions = std::move(enabledExtensions);

    // If requested, we enable the default validation layers for debugging
    if (m_debugMode == DebugMode::Utils)
    {
        CHECK_VK_ERROR_AND_THROW(vkCreateDebugUtilsMessengerEXT(m_handle, &dbgMessengerCI, nullptr,
                                                                &m_debugUtilsMessenger),
                                 "Failed to create debugUtil messenger")
    }
    LOGI("VkInstance created!")
    LOGI("Enabled Instance Layers count: {}", enabledLayers.size())
    for (const auto& layer : enabledLayers)
    {
        LOGI("\tEnabled Instance Layer: {}", layer);
    }
    LOGI("Enabled Instance Extensions count: {}", m_enabledExtensions.size())
    for (const auto& ext : m_enabledExtensions)
    {
        LOGI("\tEnabled Instance Extension: {}", ext);
    }
    // Enumerate physical devices
    {
        // Physical device
        uint32_t physicalDeviceCount = 0;
        // Get the number of available physical devices
        auto err = vkEnumeratePhysicalDevices(m_handle, &physicalDeviceCount, nullptr);
        CHECK_VK_ERROR(err, "Failed to get physical device count");
        if (physicalDeviceCount == 0)
            LOG_ERROR_AND_THROW("No physical devices found on the system");

        // Enumerate devices
        m_physicalDevices.resize(physicalDeviceCount);
        err = vkEnumeratePhysicalDevices(m_handle, &physicalDeviceCount, m_physicalDevices.data());
        CHECK_VK_ERROR(err, "Failed to enumerate physical devices");
        VERIFY_EXPR(m_physicalDevices.size() == physicalDeviceCount);
    }
}

bool Instance::EnumerateInstanceExtensions(const char* layerName,
                                           std::vector<VkExtensionProperties>& extensions)
{
    uint32_t ExtensionCount = 0;

    if (vkEnumerateInstanceExtensionProperties(layerName, &ExtensionCount, nullptr) != VK_SUCCESS)
        return false;

    extensions.resize(ExtensionCount);
    if (vkEnumerateInstanceExtensionProperties(layerName, &ExtensionCount, extensions.data()) !=
        VK_SUCCESS)
    {
        extensions.clear();
        return false;
    }
    return true;
}

bool Instance::IsLayerSupported(const char* layerName, uint32_t& version)
{
    auto it = std::find_if(m_supportedLayers.begin(), m_supportedLayers.end(),
                           [&](VkLayerProperties& layer) {
                               if (strcmp(layer.layerName, layerName) == 0)
                               {
                                   version = layer.specVersion;
                                   return true;
                               }
                               else
                               {
                                   return false;
                               }
                           });
    return it != m_supportedLayers.end();
}

bool Instance::IsExtensionEnabled(const char* name)
{
    return std::find_if(m_enabledExtensions.begin(), m_enabledExtensions.end(),
                        [name](const char* enabledExtension) {
                            return strcmp(name, enabledExtension) == 0;
                        }) != m_enabledExtensions.end();
}

bool Instance::IsExtensionSupported(std::vector<VkExtensionProperties>& extensions,
                                    const char* extensionName)
{
    auto it = std::find_if(extensions.begin(), extensions.end(), [&](VkExtensionProperties& ext) {
        return strcmp(ext.extensionName, extensionName) == 0;
    });
    return it != extensions.end();
}
VkPhysicalDevice Instance::SelectPhysicalDevice()
{
    assert(!m_physicalDevices.empty());
    // simply return the first physical device
    return m_physicalDevices[0];
}


} // namespace zen::val