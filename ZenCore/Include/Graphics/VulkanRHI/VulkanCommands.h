#pragma once
#include "Graphics/RHI/RHICommands.h"

namespace zen::rhi
{
class VulkanDevice;
class VulkanCommandBuffer;
class VulkanCommandBufferManager;

class VulkanCommandList : public RHICommandList
{
public:
    VulkanCommandList(VulkanDevice* device, VulkanCommandBufferManager* cmdBufferManager);
    virtual ~VulkanCommandList() = default;

    void Begin() final;
    void End() final;

private:
    VulkanDevice* m_device{nullptr};
    VulkanCommandBufferManager* m_cmdBufferManager{nullptr};
    VulkanCommandBuffer* m_currentCmdBuffer{nullptr};
};
} // namespace zen::rhi