#pragma once
#include "Common/UniquePtr.h"
#include "DeviceResource.h"

#include <memory>

namespace zen::vulkan {
class Device;
class CommandBuffer;
class CommandPool : public DeviceResource<vk::CommandPool> {
public:
  ZEN_NO_COPY(CommandPool);
  CommandPool(const Device& device, uint32_t queueFamilyIndex, size_t threadIndex = 0);
  ~CommandPool();

  CommandPool(CommandPool&& other) noexcept;
  CommandPool& operator=(CommandPool&&) = delete;

  void Reset();

  CommandBuffer& RequestCommandBuffer(vk::CommandBufferLevel = vk::CommandBufferLevel::ePrimary);

private:
  size_t m_threadIndex{0};
  uint32_t m_queueFamilyIndex{0};

  std::vector<UniquePtr<CommandBuffer>> m_primaryCmdBuffers;
  std::vector<UniquePtr<CommandBuffer>> m_secondaryCmdBuffers;
};
}  // namespace zen::vulkan