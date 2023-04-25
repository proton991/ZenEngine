#pragma once
#include <vulkan/vulkan.hpp>
#include "Common/Base.h"
#include "DeviceResource.h"

namespace zen::vulkan {
class CommandPool;
class CommandBuffer : public DeviceResource<vk::CommandBuffer> {
public:
  ZEN_NO_COPY(CommandBuffer)
  CommandBuffer(const CommandPool& cmdPool, vk::CommandBufferLevel level);
  ~CommandBuffer();

private:
  const CommandPool& m_cmdPool;
  vk::CommandBuffer m_handle{nullptr};
  const vk::CommandBufferLevel m_level{};
};
}  // namespace zen::vulkan