#pragma once
#include <vulkan/vulkan.h>
#include "DeviceObject.h"

namespace zen::val
{
class CommandPool;
class CommandBuffer : public DeviceObject<VkCommandBuffer, VK_OBJECT_TYPE_COMMAND_BUFFER>
{
public:
    CommandBuffer(CommandPool& cmdPool, VkCommandBufferLevel level, std::string debugName = "");
    
    void Reset();

private:
    CommandPool&               m_cmdPool;
    const VkCommandBufferLevel m_level;
    VkCommandBuffer            m_handle{VK_NULL_HANDLE};
};
} // namespace zen::val