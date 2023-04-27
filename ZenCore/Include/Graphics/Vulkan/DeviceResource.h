#pragma once
#include <vulkan/vulkan.hpp>
#include "Common/Base.h"
#include "DeviceVK.h"

namespace zen::vulkan {

template <class VkHandle>
class DeviceResource {
public:
  ZEN_NO_COPY(DeviceResource);
  DeviceResource(const Device& device, VkHandle handle = nullptr)
      : m_device(device), m_handle(handle) {}

  DeviceResource(DeviceResource&& other)
      : m_handle(std::exchange(other.m_handle, {})), m_device(other.m_device) {
    SetDebugName(other.m_debugName);
  }

  DeviceResource& operator=(DeviceResource&& other) {
    m_handle = std::exchange(other.m_handle, {});
    m_device = other.m_device;
    SetDebugName(other.m_debugName);
  }

  inline const VkHandle& GetHandle() const { return m_handle; }

  inline VkHandle& GetHandle() { return m_handle; }

  void SetHanlde(VkHandle handle) { m_handle = handle; }

  void SetDebugName(const std::string& name) {
    m_debugName = name;
    m_device.SetDebugObjName(m_handle, m_debugName);
  }

  const Device& GetDevice() const { return m_device; }

  vk::Device GetDeviceHandle() { return m_device.GetHandle(); }

  VmaAllocator GetVmaAllocator() const { return m_device.GetMemAllocator(); }

private:
  const Device& m_device;
  VkHandle m_handle;
  std::string m_debugName;
};
}  // namespace zen::vulkan