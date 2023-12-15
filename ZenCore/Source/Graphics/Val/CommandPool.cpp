#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Common/Errors.h"

namespace zen::val
{
UniquePtr<CommandPool> CommandPool::Create(const Device& device, const CommandPool::CreateInfo& CI)
{
    auto* cmdPool = new CommandPool(device, CI);
    return UniquePtr<CommandPool>(cmdPool);
}

CommandPool::CommandPool(const Device& device, const CommandPool::CreateInfo& CI) :
    DeviceObject(device),
    m_threadId(CI.threadId),
    m_queueFamilyIndex(CI.queueFamilyIndex),
    m_resetMode(CI.resetMode)
{
    VkCommandPoolCreateFlags flags;
    switch (m_resetMode)
    {
        case ResetMode::ResetBuffer:
        case ResetMode::ReAllocate: flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; break;
        case ResetMode::ResetPool:
        default: flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; break;
    }

    VkCommandPoolCreateInfo cmdPoolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmdPoolCI.queueFamilyIndex = m_queueFamilyIndex;
    cmdPoolCI.flags            = flags;

    CHECK_VK_ERROR_AND_THROW(
        vkCreateCommandPool(m_device.GetHandle(), &cmdPoolCI, nullptr, &m_handle),
        "Failed to create command pool");
}

CommandPool::~CommandPool()
{
    m_primaryCmdBuffers.clear();
    m_secondaryCmdBuffers.clear();
    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device.GetHandle(), m_handle, nullptr);
    }
}

CommandBuffer* CommandPool::RequestCommandBuffer(VkCommandBufferLevel level)
{
    if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
    {
        if (m_activePrimaryCmdBufferCnt < m_primaryCmdBuffers.size())
        {
            return m_primaryCmdBuffers[m_activePrimaryCmdBufferCnt++].Get();
        }
        m_primaryCmdBuffers.emplace_back(MakeUnique<CommandBuffer>(*this, level));
        m_activePrimaryCmdBufferCnt++;
        return m_primaryCmdBuffers.back().Get();
    }
    else
    {
        if (m_activeSecondaryCmdBufferCnt < m_secondaryCmdBuffers.size())
        {
            return m_secondaryCmdBuffers[m_activeSecondaryCmdBufferCnt++].Get();
        }
        m_secondaryCmdBuffers.emplace_back(MakeUnique<CommandBuffer>(*this, level));
        m_activeSecondaryCmdBufferCnt++;
        return m_secondaryCmdBuffers.back().Get();
    }
}

void CommandPool::ResetPool()
{
    switch (m_resetMode)
    {
        case ResetMode::ResetPool:
        {
            vkResetCommandPool(m_device.GetHandle(), m_handle, 0);
            break;
        }
        case ResetMode::ResetBuffer:
        {
            ResetCmdBuffers();
            break;
        }
        case ResetMode::ReAllocate:
        {
            FreeCmdBuffers();
            m_primaryCmdBuffers.clear();
            m_secondaryCmdBuffers.clear();
            break;
        }
    }
    m_activePrimaryCmdBufferCnt   = 0;
    m_activeSecondaryCmdBufferCnt = 0;
}

void CommandPool::ResetCmdBuffers()
{
    std::for_each(m_primaryCmdBuffers.begin(), m_primaryCmdBuffers.end(),
                  [&](UniquePtr<CommandBuffer>& cmdBuffer) { cmdBuffer->Reset(); });
    std::for_each(m_secondaryCmdBuffers.begin(), m_secondaryCmdBuffers.end(),
                  [&](UniquePtr<CommandBuffer>& cmdBuffer) { cmdBuffer->Reset(); });
}

void CommandPool::FreeCmdBuffers()
{
    std::vector<VkCommandBuffer> buffers;
    std::for_each(
        m_primaryCmdBuffers.begin(), m_primaryCmdBuffers.end(),
        [&](UniquePtr<CommandBuffer>& cmdBuffer) { buffers.push_back(cmdBuffer->GetHandle()); });
    std::for_each(
        m_secondaryCmdBuffers.begin(), m_secondaryCmdBuffers.end(),
        [&](UniquePtr<CommandBuffer>& cmdBuffer) { buffers.push_back(cmdBuffer->GetHandle()); });
    vkFreeCommandBuffers(m_device.GetHandle(), m_handle, buffers.size(), buffers.data());
}
} // namespace zen::val