#pragma once

#include "Graphics/RHI/RHICommandList.h"


namespace zen
{
class VulkanCommandBuffer;
class VulkanFence;
// Hodls submission info for VulkanQueue
// In the future, adds GPU profiling related metrics in this structure
class VulkanWorkload
{
public:
private:
    VulkanCommandBuffer* m_pCmdBuffer{nullptr};
    VulkanFence* m_pFence{nullptr}; // Used at vkQueueSubmit
};
} // namespace zen