#pragma once
#include <vulkan/vulkan.hpp>
#include "Common/Base.h"
#include "Common/UniquePtr.h"

#include <memory>

namespace zen::vulkan {
class Device;
class CommandBuffer;
class CommandPool {
public:
  ZEN_NO_COPY(CommandPool);
  CommandPool(const Device& device, uint32_t queueFamilyIndex, size_t threadIndex = 0);
  ~CommandPool();

  CommandPool(CommandPool&& other) noexcept;
  CommandPool& operator=(CommandPool&&) = delete;

  vk::CommandPool GetHandle() const { return m_handle; };

  void Reset();

  CommandBuffer& RequestCommandBuffer(vk::CommandBufferLevel = vk::CommandBufferLevel::ePrimary);

private:
  const Device& m_device;
  vk::CommandPool m_handle{nullptr};
  size_t m_threadIndex{0};
  uint32_t m_queueFamilyIndex{0};

  std::vector<UniquePtr<CommandBuffer>> m_primaryCmdBuffers;
  std::vector<UniquePtr<CommandBuffer>> m_secondaryCmdBuffers;
};
}  // namespace zen::vulkan