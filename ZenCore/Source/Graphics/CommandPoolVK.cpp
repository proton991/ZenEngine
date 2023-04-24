#include "Graphics/Vulkan/CommandPoolVK.h"
#include "Graphics/Vulkan/CommandbufferVK.h"
#include "Graphics/Vulkan/DeviceVK.h"

namespace zen::vulkan {

CommandPool::CommandPool(const Device& device, uint32_t queueFamilyIndex,
                         size_t threadIndex /*= 0*/)
    : m_device(device), m_threadIndex(threadIndex), m_queueFamilyIndex(queueFamilyIndex) {
  // reset whole pool
  auto commandPoolCI = vk::CommandPoolCreateInfo()
                           .setFlags(vk::CommandPoolCreateFlagBits::eTransient)
                           .setQueueFamilyIndex(queueFamilyIndex);
  m_handle = device.GetHandle().createCommandPool(commandPoolCI);
}

CommandPool::CommandPool(CommandPool&& other) noexcept
    : m_device(other.m_device),
      m_handle(std::exchange(other.m_handle, {})),
      m_threadIndex(std::exchange(other.m_threadIndex, {})),
      m_queueFamilyIndex(std::exchange(other.m_queueFamilyIndex, {})) {}

void CommandPool::Reset() {
  m_device.GetHandle().resetCommandPool(m_handle);
}

CommandBuffer& CommandPool::RequestCommandBuffer(
    vk::CommandBufferLevel level /*= vk::CommandBufferLevel::ePrimary*/) {
  if (level == vk::CommandBufferLevel::ePrimary) {
    m_primaryCmdBuffers.emplace_back(MakeUnique<CommandBuffer>(*this, level));
    return *m_primaryCmdBuffers.back();
  } else {
    m_secondaryCmdBuffers.emplace_back(MakeUnique<CommandBuffer>(*this, level));
    return *m_secondaryCmdBuffers.back();
  }
}

CommandPool::~CommandPool() {
  m_primaryCmdBuffers.clear();
  m_secondaryCmdBuffers.clear();

  if (m_handle) {
    m_device.GetHandle().destroyCommandPool(m_handle);
  }
}

}  // namespace zen::vulkan