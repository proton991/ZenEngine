#pragma once
#include "SyncObjPool.h"
#include "Common/UniquePtr.h"
#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/Swapchain.h"
#include "Graphics/Val/Image.h"
#include "RenderBuffers.h"

namespace zen
{
class RenderFrame
{
public:
    RenderFrame(const val::Device& device, UniquePtr<val::Image>&& swapchainImage) :
        m_valDevice(device), m_syncObjPool(m_valDevice), m_swapchainImage(std::move(swapchainImage))
    {
        m_stagingBuffer = MakeUnique<StagingBuffer>(m_valDevice, MAX_STAGING_BUFFER_SIZE);
    }

    val::CommandBuffer* RequestCommandBuffer(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    VkSemaphore RequestSemaphore() { return m_syncObjPool.RequestSemaphore(); }

    VkSemaphore RequestSemaphoreWithOwnership() { return m_syncObjPool.RequestSemaphoreWithOwnership(); }

    void ReleaseSemaphoreWithOwnership(VkSemaphore sem) { m_syncObjPool.ReleaseSemaphoreWithOwnership(sem); }

    VkFence RequestFence() { return m_syncObjPool.RequestFence(); }

    void Reset();

    val::Image* GetSwapchainImage() const { return m_swapchainImage.Get(); }

    StagingBuffer* GetStagingBuffer() const { return m_stagingBuffer.Get(); }

private:
    val::CommandPool* GetCommandPool(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode);

    const val::Device& m_valDevice;

    SynObjPool m_syncObjPool;

    std::unordered_map<uint32_t, UniquePtr<val::CommandPool>> m_cmdPools;

    UniquePtr<val::Image> m_swapchainImage;

    UniquePtr<StagingBuffer> m_stagingBuffer;
};
} // namespace zen