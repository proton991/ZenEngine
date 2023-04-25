#pragma once
#include <vulkan/vulkan.hpp>
#include "ContextVK.h"

namespace zen::vulkan {
class Context;
class Device {
public:
  Device(const Context& context);

  vk::Device GetHandle() const { return m_handle; }

  template <typename Handle>
  void SetDebugObjName(Handle objHandle, std::string name) const;

  uint32_t GetQueueFamliyIndex(QueueIndices index) const;

private:
  vk::DispatchLoaderDynamic m_loader;
  vk::PhysicalDevice m_gpu;
  vk::Device m_handle;
  DeviceQueueInfo m_queueInfo{};
};

template <typename Handle>
void Device::SetDebugObjName(Handle objHandle, std::string name) const {

  auto objNameInfo = vk::DebugUtilsObjectNameInfoEXT()
                         .setObjectHandle(uint64_t(Handle::CType(objHandle)))
                         .setObjectType(objHandle.objectType)
                         .setPObjectName(name.c_str());
  if (m_loader.vkSetDebugUtilsObjectNameEXT) {
    m_handle.setDebugUtilsObjectNameEXT(objNameInfo, m_loader);
  }
}
}  // namespace zen::vulkan