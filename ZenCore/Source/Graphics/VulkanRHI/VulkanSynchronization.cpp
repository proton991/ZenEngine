#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"

namespace zen::rhi
{
VulkanFence::VulkanFence(VulkanFenceManager* owner, bool createSignaled) :
    m_owner(owner), m_state(createSignaled ? State::eSignaled : State::eInitial)
{
    VkFenceCreateInfo fenceCI;
    InitVkStruct(fenceCI, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    fenceCI.flags = createSignaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    VKCHECK(vkCreateFence(m_owner->GetDevice()->GetVkHandle(), &fenceCI, nullptr, &m_fence));
}

void VulkanFenceManager::Destroy()
{
    VERIFY_EXPR(m_usedFences.empty());
    while (!m_freeFences.empty())
    {
        VulkanFence* fence = m_freeFences.front();
        m_freeFences.pop();
        DestroyFence(fence);
    }
}

VulkanFence* VulkanFenceManager::CreateFence(bool createSignaled)
{
    if (!m_freeFences.empty())
    {
        VulkanFence* fence = m_freeFences.front();
        m_freeFences.pop();
        if (createSignaled) { fence->m_state = VulkanFence::State::eSignaled; }
        return fence;
    }
    VulkanFence* newFence = new VulkanFence(this, createSignaled);
    m_usedFences.push_back(newFence);
    return newFence;
}

void VulkanFenceManager::ReleaseFence(VulkanFence*& fence)
{
    ResetFence(fence);
    auto it = m_usedFences.begin();
    while (it != m_usedFences.end())
    {
        if (*it == fence)
        {
            m_usedFences.erase(it);
            break;
        }
    }
    m_freeFences.push(fence);
    fence = nullptr;
}

bool VulkanFenceManager::IsFenceSignaled(VulkanFence* fence)
{
    if (fence->IsSignaled()) { return true; }
    // double check
    VkResult result = vkGetFenceStatus(m_device->GetVkHandle(), fence->m_fence);
    if (result == VK_SUCCESS)
    {
        fence->m_state = VulkanFence::State::eSignaled;
        return true;
    }
    return false;
}

bool VulkanFenceManager::WaitForFence(VulkanFence* fence, uint64_t timeNS)
{
    VkResult result = vkWaitForFences(m_device->GetVkHandle(), 1, &fence->m_fence, true, timeNS);
    if (result == VK_SUCCESS)
    {
        fence->m_state = VulkanFence::State::eSignaled;
        return true;
    }
    return false;
}

void VulkanFenceManager::ResetFence(VulkanFence* fence)
{
    if (fence->m_state != VulkanFence::State::eInitial)
    {
        VKCHECK(vkResetFences(m_device->GetVkHandle(), 1, &fence->m_fence));
        fence->m_state = VulkanFence::State::eInitial;
    }
}

void VulkanFenceManager::WaitAndReleaseFence(VulkanFence*& fence, uint64_t timeNS)
{
    if (!fence->IsSignaled()) { WaitForFence(fence, timeNS); }
    ResetFence(fence);
    ReleaseFence(fence);
    fence = nullptr;
}

void VulkanFenceManager::DestroyFence(VulkanFence* fence)
{
    vkDestroyFence(m_device->GetVkHandle(), fence->GetVkHandle(), nullptr);
    fence->m_fence = VK_NULL_HANDLE;
    fence->m_state = VulkanFence::State::eInitial;
}

VulkanSemaphore::~VulkanSemaphore()
{
    vkDestroySemaphore(m_device->GetVkHandle(), m_semaphore, nullptr);
    m_semaphore = VK_NULL_HANDLE;
}

void VulkanSemaphoreManager::Destroy()
{
    VERIFY_EXPR(m_usedSemaphores.empty());
    while (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* sem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        delete sem;
    }
}

VulkanSemaphore* VulkanSemaphoreManager::CreateSemaphore()
{
    if (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* sem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        return sem;
    }
    VulkanSemaphore* newSem = new VulkanSemaphore(m_device);
    m_usedSemaphores.push_back(newSem);
    return newSem;
}

void VulkanSemaphoreManager::ReleaseSemaphore(VulkanSemaphore*& sem)
{
    auto it = m_usedSemaphores.begin();
    while (it != m_usedSemaphores.end())
    {
        if (*it == sem)
        {
            m_usedSemaphores.erase(it);
            break;
        }
    }
    m_freeSemaphores.push(sem);
    sem = nullptr;
}
} // namespace zen::rhi