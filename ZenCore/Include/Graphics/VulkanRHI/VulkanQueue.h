#pragma once
#include "VulkanCommandBuffer.h"
#include "VulkanHeaders.h"
#include "Templates/HeapVector.h"
#include "Templates/Queue.h"

namespace zen
{
class VulkanWorkload;
class VulkanDevice;
class VulkanCommandBuffer;
class FVulkanCommandBufferPool;

class VulkanQueue
{
public:
    VulkanQueue(VulkanDevice* device, uint32_t familyIndex);

    FVulkanCommandBufferPool* AcquireCommandBufferPool(VulkanCommandBufferType type);

    void RecycleCommandBufferPool(FVulkanCommandBufferPool* pCmdBufferPool);

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

    HeapVector<FVulkanCommandBufferPool*> m_cmdBufferPools;

    Queue<VulkanWorkload*> m_workloadsPendingProcess;
};
} // namespace zen