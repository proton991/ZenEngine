#pragma once
#include "RenderFrame.h"
#include "Graphics/Val/Swapchain.h"
#include "Common/UniquePtr.h"
#include "Platform/GlfwWindow.h"


namespace zen
{
class RenderContext
{
public:
    RenderContext(val::Device& device, platform::GlfwWindowImpl* window);

    val::CommandBuffer* StartFrame(val::CommandPool::ResetMode resetMode);

    void EndFrame();

    RenderFrame& GetActiveFrame() { return m_frames[m_activeFrameIndex]; }

    VkFormat GetSwapchainFormat() const { return m_swapchain->GetFormat(); }

    VkExtent2D GetSwapchainExtent2D() const { return m_swapchain->GetExtent2D(); }

    StagingBuffer* GetCurrentStagingBuffer() { return GetActiveFrame().GetStagingBuffer(); }

    val::CommandBuffer* GetCommandBuffer()
    {
        return m_commandPool->RequestCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    }

    void ResetCommandPool()
    {
        m_commandPool->ResetPool();
    }

    void SubmitImmediate(val::CommandBuffer* pCmdBuffer);

    template <typename T>
    void UpdateUniformBuffer(const T* data, UniformBuffer* uniformBuffer, val::CommandBuffer* pCmdBuffer);

    template <typename T>
    void UpdateUniformBuffer(ArrayView<T> data, UniformBuffer* uniformBuffer, val::CommandBuffer* pCmdBuffer);

private:
    void Init();

    void StartFrameInternal();

    void RecreateSwapchain();

    void SubmitInternal();

    val::Device& m_valDevice;
    // queue
    const val::Queue& m_queue;
    // VkSurfaceKHR is created and managed by RenderContext
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    // val::Swapchain is created and managed by RenderContext
    UniquePtr<val::Swapchain> m_swapchain;
    // store frames
    std::vector<RenderFrame> m_frames;
    // active frame index
    uint32_t m_activeFrameIndex{0};
    // whether a frame is active or not
    bool m_frameActive{false};
    // semaphore signaled when image is acquired and being waited when submit
    VkSemaphore m_imageAcquiredSem{VK_NULL_HANDLE};
    // semaphore signaled when commands are submitted
    VkSemaphore m_renderFinished{VK_NULL_HANDLE};
    // current command buffer provided by RenderFrame
    val::CommandBuffer* m_activeCmdBuffer{nullptr};
    // common command pool
    UniquePtr<val::CommandPool> m_commandPool;
    // common sync obj pool
    SynObjPool m_synObjPool;
};
} // namespace zen