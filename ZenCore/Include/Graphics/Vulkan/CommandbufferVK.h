#pragma once
#include <vulkan/vulkan.hpp>

namespace zen::vulkan {
class CommandPool;
class CommandBuffer {
public:
  CommandBuffer(const CommandPool& cmdPool, vk::CommandBufferLevel level);
  ~CommandBuffer();

private:
  const CommandPool& m_cmdPool;
  vk::CommandBuffer m_handle{nullptr};
  const vk::CommandBufferLevel m_level{};
};
}  // namespace zen::vulkan