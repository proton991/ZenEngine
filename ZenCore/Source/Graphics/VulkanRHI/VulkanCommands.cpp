#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"

namespace zen::rhi
{
VulkanCommandList::VulkanCommandList(VulkanDevice* device,
                                     VulkanCommandBufferManager* cmdBufferManager) :
    m_device(device), m_cmdBufferManager(cmdBufferManager)
{}

void VulkanCommandList::Begin()
{
    m_currentCmdBuffer = m_cmdBufferManager->GetActiveCommandBuffer();
    m_currentCmdBuffer->Begin();
}

void VulkanCommandList::End() {}
} // namespace zen::rhi