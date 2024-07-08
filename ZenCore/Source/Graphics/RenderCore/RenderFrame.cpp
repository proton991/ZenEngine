#include "Graphics/RenderCore/RenderFrame.h"
#include "Common/Errors.h"

namespace zen
{
std::vector<UniquePtr<val::CommandPool>>& RenderFrame::GetCommandPools(
    uint32_t queueFamilyIndex,
    val::CommandPool::ResetMode resetMode)
{
    auto it = m_cmdPools.find(queueFamilyIndex);
    if (it != m_cmdPools.end())
    {
        return it->second;
    }
    val::CommandPool::CreateInfo cmdPoolCI{};
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    cmdPoolCI.resetMode        = resetMode;

    std::vector<UniquePtr<val::CommandPool>> cmdPools;
    for (uint32_t i = 0; i < m_threadCount; i++)
    {
        cmdPoolCI.threadId = i;
        cmdPools.push_back(val::CommandPool::Create(m_valDevice, cmdPoolCI));
    }
    auto [insertIt, inserted] = m_cmdPools.emplace(queueFamilyIndex, std::move(cmdPools));

    if (!inserted)
    {
        LOG_FATAL_ERROR_AND_THROW("Failed to insert command pool in render frame");
    }
    return insertIt->second;
}

val::CommandBuffer* RenderFrame::RequestCommandBuffer(uint32_t queueFamilyIndex,
                                                      val::CommandPool::ResetMode resetMode,
                                                      VkCommandBufferLevel level,
                                                      uint32_t threadId)
{
    auto& cmdPools = GetCommandPools(queueFamilyIndex, resetMode);

    auto cmdPoolIt = std::find_if(cmdPools.begin(), cmdPools.end(),
                                  [&threadId](UniquePtr<val::CommandPool>& commandPool) {
                                      return commandPool->GetThreadId() == threadId;
                                  });
    return (*cmdPoolIt)->RequestCommandBuffer(level);
}

void RenderFrame::Reset()
{
    m_syncObjPool.WaitForFences();
    m_syncObjPool.ResetFences();
    m_syncObjPool.ResetSemaphores();

    for (auto& cmdPoolsPerQueue : m_cmdPools)
    {
        for (UniquePtr<val::CommandPool>& cmdPool : cmdPoolsPerQueue.second)
        {
            cmdPool->ResetPool();
        }
    }

    m_stagingBuffer->ResetOffset();
    m_stagingBuffer->Flush();
}
} // namespace zen