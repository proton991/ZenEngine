#pragma once
#include <queue>
#include "VulkanCommon.h"
#include "Graphics/RHI/RHIResource.h"

namespace zen::rhi
{
class VulkanDevice;
class VulkanFenceManager;

class VulkanFence
{
public:
    explicit VulkanFence(VulkanFenceManager* owner, bool createSignaled = false);

    VkFence GetVkHandle() const
    {
        return m_fence;
    }

    bool IsSignaled() const
    {
        return m_state == State::eSignaled;
    }

    VulkanFenceManager* GetOwner() const
    {
        return m_owner;
    }

private:
    ~VulkanFence() = default;

    enum class State
    {
        eInitial,
        eSignaled
    };
    VkFence m_fence{VK_NULL_HANDLE};
    VulkanFenceManager* m_owner{nullptr};
    State m_state{State::eInitial};

    friend class VulkanFenceManager;
};

class VulkanFenceManager
{
public:
    explicit VulkanFenceManager(VulkanDevice* device) : m_device(device) {}

    void Destroy();

    VulkanDevice* GetDevice() const
    {
        return m_device;
    }

    VulkanFence* CreateFence(bool createSignaled = false);

    void ReleaseFence(VulkanFence*& fence);

    bool IsFenceSignaled(VulkanFence* fence);

    bool WaitForFence(VulkanFence* fence, uint64_t timeNS);

    void ResetFence(VulkanFence* fence);

    void WaitAndReleaseFence(VulkanFence*& fence, uint64_t timeNS);

private:
    void DestroyFence(VulkanFence* fence);

    VulkanDevice* m_device{nullptr};
    std::vector<VulkanFence*> m_usedFences;
    std::queue<VulkanFence*> m_freeFences;
};

class VulkanSemaphore : public RHIResource
{
public:
    explicit VulkanSemaphore(VulkanDevice* device) : m_device(device) {}

    ~VulkanSemaphore();

    VkSemaphore GetVkHandle() const
    {
        return m_semaphore;
    }

private:
    VulkanDevice* m_device{nullptr};
    VkSemaphore m_semaphore{VK_NULL_HANDLE};
};


class VulkanSemaphoreManager
{
public:
    explicit VulkanSemaphoreManager(VulkanDevice* device) : m_device(device) {}

    void Destroy();

    VulkanSemaphore* CreateSemaphore();

    void ReleaseSemaphore(VulkanSemaphore*& sem);

private:
    VulkanDevice* m_device{nullptr};
    std::vector<VulkanSemaphore*> m_usedSemaphores;
    std::queue<VulkanSemaphore*> m_freeSemaphores;
};
} // namespace zen::rhi