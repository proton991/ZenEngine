#include "Graphics/Rendering/RenderFrame.h"
#include "Common/Errors.h"

namespace zen
{
val::CommandPool* RenderFrame::GetCommandPool(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode)
{
    auto it = m_cmdPools.find(queueFamilyIndex);
    if (it != m_cmdPools.end())
    {
        return it->second.Get();
    }
    val::CommandPool::CreateInfo cmdPoolCI{};
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    cmdPoolCI.resetMode        = resetMode;

    auto [insertIt, inserted] = m_cmdPools.emplace(queueFamilyIndex, MakeUnique<val::CommandPool>(m_valDevice, cmdPoolCI));

    if (!inserted)
    {
        LOG_FATAL_ERROR_AND_THROW("Failed to insert command pool in render frame");
    }
    return insertIt->second.Get();
}

val::CommandBuffer* RenderFrame::RequestCommandBuffer(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode, VkCommandBufferLevel level)
{
    auto* commandPool = GetCommandPool(queueFamilyIndex, resetMode);
    return commandPool->RequestCommandBuffer(level);
}

void RenderFrame::Reset()
{
    m_syncObjPool.WaitForFences();
    m_syncObjPool.ResetFences();
    m_syncObjPool.ResetSemaphores();

    for (auto& it : m_cmdPools)
    {
        it.second->ResetPool();
    }
}
} // namespace zen