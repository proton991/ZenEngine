#pragma once
#include <vector>
#include <vulkan/vulkan.hpp>
namespace zen::vulkan {
class Instance {
public:
  Instance(std::vector<const char*> requiredLayers, std::vector<const char*> requiredExtensions);

  [[nodiscard]] vk::Instance GetHandle() const { return m_handle.get(); }
  [[nodiscard]] vk::DispatchLoaderDynamic GetDL() const { return m_dl; }

private:
  bool IsLayerSupported(const char* layerName);
  bool IsExtensionSupported(const char* extensionName);
  vk::UniqueInstance m_handle;
  vk::DispatchLoaderDynamic m_dl;
  std::vector<vk::LayerProperties> m_supportedLayers;
  std::vector<vk::ExtensionProperties> m_supportedExtensions;
  std::vector<const char*> m_enabledLayers;
  std::vector<const char*> m_enabledExtensions;
  vk::UniqueHandle<vk::DebugUtilsMessengerEXT, vk::DispatchLoaderDynamic> m_debugUtilsMessenger;
};
}  // namespace zen::vulkan