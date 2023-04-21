#pragma once
#include <vulkan/vulkan.hpp>

namespace zen::vulkan {
class Context;
class Device {
public:
  Device(const Context& context);

  template <typename Handle>
  void SetDebugObjName(Handle objHandle, std::string name);

private:
  const Context& m_context;
  vk::PhysicalDevice m_gpu;
  vk::Device m_logicalDevice;
};

}  // namespace zen::vulkan