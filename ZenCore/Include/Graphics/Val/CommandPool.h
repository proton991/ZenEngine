#pragma once

#include <vulkan/vulkan.h>
#include "Common/UniquePtr.h"
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
class CommandBuffer;

class CommandPool : public DeviceObject<VkCommandPool, VK_OBJECT_TYPE_COMMAND_POOL>
{
public:
    enum class ResetMode
    {
        ResetPool,
        ResetBuffer,
        ReAllocate
    };
    struct CreateInfo
    {
        uint32_t queueFamilyIndex{0};
        ResetMode resetMode{ResetMode::ResetPool};
        uint32_t threadId{0};
    };
    static UniquePtr<CommandPool> Create(const Device& device, const CreateInfo& CI);

    CommandPool(const Device& device, const CreateInfo& CI);

    ~CommandPool();

    const Device& GetDevice()
    {
        return m_device;
    }

    CommandBuffer* RequestCommandBuffer(
        VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    void ResetPool();

    auto GetThreadId() const
    {
        return m_threadId;
    }

private:
    void ResetCmdBuffers();
    void FreeCmdBuffers();

    uint32_t m_threadId{0};
    uint32_t m_queueFamilyIndex{0};
    ResetMode m_resetMode{ResetMode::ResetPool};

    std::vector<UniquePtr<CommandBuffer>> m_primaryCmdBuffers;
    std::vector<UniquePtr<CommandBuffer>> m_secondaryCmdBuffers;

    uint32_t m_activePrimaryCmdBufferCnt{0};
    uint32_t m_activeSecondaryCmdBufferCnt{0};
};
} // namespace zen::val