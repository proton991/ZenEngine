#pragma once
#include <vulkan/vulkan.h>

namespace zen::val
{
class CommandPool;
class CommandBuffer
{
public:
    CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level);

    VkCommandBuffer GetHandle() const { return m_handle; }

    void Reset();

private:
    CommandPool&               m_cmdPool;
    const VkCommandBufferLevel m_level;
    VkCommandBuffer            m_handle{VK_NULL_HANDLE};
};
} // namespace zen::val