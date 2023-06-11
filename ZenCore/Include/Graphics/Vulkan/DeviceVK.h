#pragma once
#include <vulkan/vulkan.hpp>
#include "Common/UniquePtr.h"
#include "DeviceContextVK.h"
#include "vma/vk_mem_alloc.h"

namespace zen::vulkan {
class Context;
class ResourceCache;
class Device {
public:
  Device(const DeviceContext& context);
  ~Device();

  vk::Device GetHandle() const { return m_handle; }

  template <typename Handle>
  void SetDebugObjName(Handle objHandle, std::string name) const;

  uint32_t GetQueueFamliyIndex(QueueIndices index) const;

  auto GetMemAllocator() const { return m_allocator; }

  auto& GetResourceCache() { return m_resourceCache; }

private:
  void InitVma();
  vk::Instance m_instance;
  vk::DispatchLoaderDynamic m_loader;
  vk::PhysicalDevice m_gpu;
  vk::Device m_handle;
  VmaAllocator m_allocator;
  DeviceQueueInfo m_queueInfo{};
  UniquePtr<ResourceCache> m_resourceCache;
};

template <typename Handle>
void Device::SetDebugObjName(Handle objHandle, std::string name) const {

  auto objNameInfo = vk::DebugUtilsObjectNameInfoEXT()
                         .setObjectHandle(uint64_t(static_cast<typename Handle::CType>(objHandle)))
                         .setObjectType(objHandle.objectType)
                         .setPObjectName(name.c_str());
  if (m_loader.vkSetDebugUtilsObjectNameEXT) {
    m_handle.setDebugUtilsObjectNameEXT(objNameInfo, m_loader);
  }
}
}  // namespace zen::vulkan