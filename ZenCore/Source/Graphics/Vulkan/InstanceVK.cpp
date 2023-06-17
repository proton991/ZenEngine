#include "Graphics/Vulkan/InstanceVK.h"
#include "Common/Logging.h"

namespace zen::vulkan {
#ifdef ZEN_DEBUG
VKAPI_ATTR VkBool32 VKAPI_CALL
DebugMessageCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                     VkDebugUtilsMessageTypeFlagsEXT messageType,
                     const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
  if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    LOGW("{} - {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName,
         pCallbackData->pMessage);
  } else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    LOGE("{} - {}: {}", pCallbackData->messageIdNumber, pCallbackData->pMessageIdName,
         pCallbackData->pMessage)
  }
  return VK_FALSE;
}

#endif

bool Instance::IsLayerSupported(const char* layerName) {
  auto it = std::find_if(m_supportedLayers.begin(), m_supportedLayers.end(),
                         [&](vk::LayerProperties& layer) {
                           return strcmp(layer.layerName, layerName) == 0;
                         });
  return it != m_supportedLayers.end();
}

bool Instance::IsExtensionSupported(const char* extensionName) {
  auto it = std::find_if(m_supportedExtensions.begin(), m_supportedExtensions.end(),
                         [&](vk::ExtensionProperties& ext) {
                           return strcmp(ext.extensionName, extensionName) == 0;
                         });
  return it != m_supportedExtensions.end();
}

Instance::Instance(std::vector<const char*> requiredLayers,
                   std::vector<const char*> requiredExtensions) {
  m_supportedLayers     = vk::enumerateInstanceLayerProperties();
  m_supportedExtensions = vk::enumerateInstanceExtensionProperties();

#ifdef ZEN_DEBUG
  requiredLayers.push_back("VK_LAYER_KHRONOS_validation");
  requiredExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  auto debugUtilCI =
      vk::DebugUtilsMessengerCreateInfoEXT()
          .setMessageSeverity(vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                              vk::DebugUtilsMessageSeverityFlagBitsEXT::
                                  eError /* | vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo*/)
          .setMessageType(vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                          vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
                          vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation)
          .setPfnUserCallback(DebugMessageCallback)
          .setPUserData(nullptr);  // Optional
#endif                             // ZEN_DEBUG
  for (const auto& layer : requiredLayers) {
    if (!IsLayerSupported(layer)) {
      LOGE("Required Instance layer {} not supported!", layer);
    } else {
      m_enabledLayers.emplace_back(layer);
      LOGI("Enabling Instance layer {}", layer);
    }
  }
  for (const auto& ext : requiredExtensions) {
    if (!IsExtensionSupported(ext)) {
      LOGE("Required Instance extension {} not supported!", ext);
    } else {
      m_enabledExtensions.emplace_back(ext);
      LOGI("Enabling Instance extension {}", ext);
    }
  }
  auto appInfo = vk::ApplicationInfo()
                     .setApiVersion(VK_API_VERSION_1_3)
                     .setApplicationVersion(VK_MAKE_VERSION(1, 0, 0))
                     .setPApplicationName("ZenApp")
                     .setEngineVersion(VK_MAKE_VERSION(1, 0, 0))
                     .setPEngineName("ZenEngine");
  vk::InstanceCreateInfo instanceCI = vk::InstanceCreateInfo()
                                          .setPApplicationInfo(&appInfo)
                                          .setEnabledExtensionCount(m_enabledExtensions.size())
                                          .setPpEnabledExtensionNames(m_enabledExtensions.data())
                                          .setEnabledLayerCount(m_enabledLayers.size())
                                          .setPpEnabledLayerNames(m_enabledLayers.data())
                                          .setPNext(&debugUtilCI);
  m_handle = vk::createInstanceUnique(instanceCI);
  m_dl     = vk::DispatchLoaderDynamic(m_handle.get(), vkGetInstanceProcAddr);

#ifdef ZEN_DEBUG
  m_debugUtilsMessenger = m_handle->createDebugUtilsMessengerEXTUnique(debugUtilCI, nullptr, m_dl);
#endif  // ZEN_DEBUG
}
}  // namespace zen::vulkan