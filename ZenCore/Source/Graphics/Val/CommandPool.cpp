#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/CommandBuffer.h"
#include "Common/Errors.h"

namespace zen::val
{
std::unique_ptr<CommandPool> CommandPool::Create(Device& device, const CommandPool::CreateInfo& CI)
{
    auto* cmdPool = new CommandPool(device, CI);
    return std::unique_ptr<CommandPool>(cmdPool);
}

CommandPool::CommandPool(Device& device, const CommandPool::CreateInfo& CI) :
    DeviceObject(device, CI.debugName), m_threadId(CI.threadId), m_queueFamilyIndex(CI.queueFamilyIndex), m_resetMode(CI.resetMode)
{
    VkCommandPoolCreateFlags flags;
    switch (m_resetMode)
    {
        case ResetMode::ResetBuffer:
        case ResetMode::ReAllocate:
            flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            break;
        case ResetMode::ResetPool:
        default:
            flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            break;
    }

    VkCommandPoolCreateInfo cmdPoolCI{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cmdPoolCI.queueFamilyIndex = m_queueFamilyIndex;
    cmdPoolCI.flags            = flags;

    CHECK_VK_ERROR_AND_THROW(vkCreateCommandPool(m_device.GetHandle(), &cmdPoolCI, nullptr, &m_handle), "Failed to create command pool");
}

CommandPool::~CommandPool()
{
    // free command buffers
    FreeCmdBuffers();

    if (m_handle != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(m_device.GetHandle(), m_handle, nullptr);
    }
}

CommandBuffer& CommandPool::RequestCommandBuffer(VkCommandBufferLevel level)
{
    if (level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
    {
        if (m_activePrimaryCmdBufferCnt < m_primaryCmdBuffers.size())
        {
            return *m_primaryCmdBuffers[m_activePrimaryCmdBufferCnt++];
        }
        m_primaryCmdBuffers.emplace_back(std::make_unique<CommandBuffer>(*this, level));
        m_activePrimaryCmdBufferCnt++;
        return *m_primaryCmdBuffers.back();
    }
    else
    {
        if (m_activeSecondaryCmdBufferCnt < m_secondaryCmdBuffers.size())
        {
            return *m_secondaryCmdBuffers[m_activeSecondaryCmdBufferCnt++];
        }
        m_secondaryCmdBuffers.emplace_back(std::make_unique<CommandBuffer>(*this, level));
        m_activeSecondaryCmdBufferCnt++;
        return *m_secondaryCmdBuffers.back();
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
            m_activePrimaryCmdBufferCnt   = 0;
            m_activeSecondaryCmdBufferCnt = 0;
            m_primaryCmdBuffers.clear();
            m_secondaryCmdBuffers.clear();
            break;
        }
    }
}

void CommandPool::ResetCmdBuffers()
{
    std::for_each(m_primaryCmdBuffers.begin(), m_primaryCmdBuffers.end(), [&](std::unique_ptr<CommandBuffer>& cmdBuffer) {
        cmdBuffer->Reset();
    });
    std::for_each(m_secondaryCmdBuffers.begin(), m_secondaryCmdBuffers.end(), [&](std::unique_ptr<CommandBuffer>& cmdBuffer) {
        cmdBuffer->Reset();
    });
}

void CommandPool::FreeCmdBuffers()
{
    std::vector<VkCommandBuffer> buffers;
    std::for_each(m_primaryCmdBuffers.begin(), m_primaryCmdBuffers.end(), [&](std::unique_ptr<CommandBuffer>& cmdBuffer) {
        buffers.push_back(cmdBuffer->GetHandle());
    });
    std::for_each(m_secondaryCmdBuffers.begin(), m_secondaryCmdBuffers.end(), [&](std::unique_ptr<CommandBuffer>& cmdBuffer) {
        buffers.push_back(cmdBuffer->GetHandle());
    });
    vkFreeCommandBuffers(m_device.GetHandle(), m_handle, buffers.size(), buffers.data());
}
} // namespace zen::val