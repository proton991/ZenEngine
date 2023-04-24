#include "Graphics/Vulkan/CommandbufferVK.h"
#include "Graphics/Vulkan/CommandPoolVK.h"

namespace zen::vulkan {

CommandBuffer::CommandBuffer(const CommandPool& cmdPool, vk::CommandBufferLevel level)
    : m_cmdPool(cmdPool), m_level(level) {
  auto cmdBufferAllocInfo = vk::CommandBufferAllocateInfo()
                                .setCommandPool(m_cmdPool.GetHandle())
                                .setLevel(level)
                                .setCommandBufferCount(1);
}

CommandBuffer::~CommandBuffer() {}

}  // namespace zen::vulkan