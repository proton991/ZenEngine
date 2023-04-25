#include "Graphics/Vulkan/DeviceVK.h"

namespace zen::vulkan {

Device::Device(const Context& context) {
  m_loader    = context.m_loader;
  m_gpu       = context.GetGPU();
  m_handle    = context.GetLogicalDevice();
  m_queueInfo = context.m_queueInfo;
}

uint32_t Device::GetQueueFamliyIndex(QueueIndices index) const {
  return m_queueInfo.familyIndices[index];
}

}  // namespace zen::vulkan