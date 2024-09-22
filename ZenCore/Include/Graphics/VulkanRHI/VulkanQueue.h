#pragma once
#include "VulkanHeaders.h"
#include <vector>

namespace zen::rhi
{
class VulkanDevice;
class VulkanCommandBuffer;

class VulkanQueue
{
public:
    VulkanQueue(VulkanDevice* device, uint32_t familyIndex);

    uint32_t GetFamilyIndex() const
    {
        return m_familyIndex;
    }

    uint32_t GetQueueIndex() const
    {
        return m_queueIndex;
    }

    VkQueue GetVkHandle() const
    {
        return m_handle;
    }

    VulkanCommandBuffer* GetLastSubmittedCmdBuffer() const
    {
        return m_lastSubmittedCmdBuffer;
    }

    void GetLastSubmitInfo(VulkanCommandBuffer*& cmdBuffer, uint64_t* fenceSignaledCounter) const;

    void Submit(VulkanCommandBuffer* cmdBuffer,
                uint32_t numSignalSemaphores,
                VkSemaphore* signalSemaphores);

    void Submit(VulkanCommandBuffer* cmdBuffer, VkSemaphore signalSemaphore);

    void Submit(VulkanCommandBuffer* cmdBuffer);

private:
    void UpdateLastSubmittedCmdBuffer(VulkanCommandBuffer* cmdBuffer);

    VulkanDevice* m_device{nullptr};
    VkQueue m_handle{VK_NULL_HANDLE};
    uint32_t m_familyIndex;
    uint32_t m_queueIndex;

    VulkanCommandBuffer* m_lastSubmittedCmdBuffer{nullptr};
};
} // namespace zen::rhi