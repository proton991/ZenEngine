#include "Graphics/Vulkan/CommandbufferVK.h"
#include "Graphics/Vulkan/CommandPoolVK.h"

namespace zen::vulkan {

CommandBuffer::CommandBuffer(const CommandPool& cmdPool, vk::CommandBufferLevel level)
    : DeviceResource(cmdPool.GetDevice()), m_cmdPool(cmdPool), m_level(level) {
  auto cmdBufferAllocInfo = vk::CommandBufferAllocateInfo()
                                .setCommandPool(m_cmdPool.GetHandle())
                                .setLevel(level)
                                .setCommandBufferCount(1);
  SetHandle(GetDeviceHandle().allocateCommandBuffers(cmdBufferAllocInfo).front());
}

CommandBuffer::~CommandBuffer() {
  if (GetHandle()) {
    GetDeviceHandle().freeCommandBuffers(m_cmdPool.GetHandle(), GetHandle());
  }
}

}  // namespace zen::vulkan