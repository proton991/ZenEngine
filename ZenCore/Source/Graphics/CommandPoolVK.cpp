#include "Graphics/Vulkan/CommandPoolVK.h"
#include "Graphics/Vulkan/CommandbufferVK.h"
#include "Graphics/Vulkan/DeviceVK.h"

namespace zen::vulkan {

CommandPool::CommandPool(const Device& device, uint32_t queueFamilyIndex,
                         size_t threadIndex /*= 0*/)
    : DeviceResource(device), m_threadIndex(threadIndex), m_queueFamilyIndex(queueFamilyIndex) {
  // reset whole pool
  auto commandPoolCI = vk::CommandPoolCreateInfo()
                           .setFlags(vk::CommandPoolCreateFlagBits::eTransient)
                           .setQueueFamilyIndex(queueFamilyIndex);
  SetHanlde(device.GetHandle().createCommandPool(commandPoolCI));
}

CommandPool::CommandPool(CommandPool&& other) noexcept
    : DeviceResource(std::move(other)),
      m_threadIndex(std::exchange(other.m_threadIndex, {})),
      m_queueFamilyIndex(std::exchange(other.m_queueFamilyIndex, {})) {}

void CommandPool::Reset() {
  GetDeviceHandle().resetCommandPool(GetHandle());
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

  if (GetHandle()) {
    GetDeviceHandle().destroyCommandPool(GetHandle());
  }
}

}  // namespace zen::vulkan