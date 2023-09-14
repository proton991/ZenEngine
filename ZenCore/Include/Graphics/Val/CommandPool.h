#pragma once

#include <vulkan/vulkan.h>
#include <memory>
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
        uint32_t    queueFamilyIndex{0};
        ResetMode   resetMode{ResetMode::ResetPool};
        uint32_t    threadId{0};
        std::string debugName{};
    };
    static std::unique_ptr<CommandPool> Create(Device& device, const CreateInfo& CI);

    ~CommandPool();

    Device& GetDevice() { return m_device; }

    CommandBuffer& RequestCommandBuffer(VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    void ResetPool();

private:
    void ResetCmdBuffers();
    void FreeCmdBuffers();

    CommandPool(Device& device, const CreateInfo& CI);
    uint32_t  m_threadId{0};
    uint32_t  m_queueFamilyIndex{0};
    ResetMode m_resetMode{ResetMode::ResetPool};

    std::vector<std::unique_ptr<CommandBuffer>> m_primaryCmdBuffers;
    std::vector<std::unique_ptr<CommandBuffer>> m_secondaryCmdBuffers;

    uint32_t m_activePrimaryCmdBufferCnt{0};
    uint32_t m_activeSecondaryCmdBufferCnt{0};
};
} // namespace zen::val