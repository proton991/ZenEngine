#include "Graphics/Vulkan/DeviceVK.h"
#include "Graphics/Vulkan/ContextVK.h"

namespace zen::vulkan {
template <typename Handle>
void Device::SetDebugObjName(Handle objHandle, std::string name) {

  auto objNameInfo = vk::DebugUtilsObjectNameInfoEXT()
                         .setObjectHandle(uint64_t(Handle::CType(objHandle)))
                         .setObjectType(objHandle.objectType)
                         .setPObjectName(name.c_str());
  if (m_context.m_loader.vkSetDebugUtilsObjectNameEXT) {
    m_logicalDevice.setDebugUtilsObjectNameEXT(objNameInfo, m_context.m_loader);
  }
}

Device::Device(const Context& context) : m_context(context) {
  m_gpu           = m_context.GetGPU();
  m_logicalDevice = m_context.GetLogicalDevice();
}
}  // namespace zen::vulkan