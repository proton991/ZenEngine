#pragma once
#include "SyncObjPool.h"
#include "Common/UniquePtr.h"
#include "Graphics/Val/CommandPool.h"
#include "Graphics/Val/Swapchain.h"
#include "Graphics/Val/Image.h"

namespace zen
{
class RenderFrame
{
public:
    RenderFrame(val::Device& device, UniquePtr<val::Image>&& swapchainImage) :
        m_valDevice(device), m_syncObjPool(m_valDevice), m_swapchainImage(std::move(swapchainImage)) {}

    val::CommandBuffer* RequestCommandBuffer(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode, VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

    VkSemaphore RequestSemaphore() { return m_syncObjPool.RequestSemaphore(); }

    VkFence RequestFence() { return m_syncObjPool.RequestFence(); }

    void Reset();

private:
    val::CommandPool* GetCommandPool(uint32_t queueFamilyIndex, val::CommandPool::ResetMode resetMode);

    val::Device& m_valDevice;

    SynObjPool m_syncObjPool;

    std::unordered_map<uint32_t, UniquePtr<val::CommandPool>> m_cmdPools;

    UniquePtr<val::Image> m_swapchainImage;
};
} // namespace zen