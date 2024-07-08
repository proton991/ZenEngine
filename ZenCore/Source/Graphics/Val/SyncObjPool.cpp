#include "Graphics/Val//SyncObjPool.h"
#include "Common/Errors.h"
#include "Common/Helpers.h"

namespace zen::val
{
VkFence SynObjPool::RequestFence()
{
    if (m_numActiveFences < m_fences.size())
    {
        return m_fences[m_numActiveFences++];
    }
    VkFence fence{VK_NULL_HANDLE};
    VkFenceCreateInfo fenceCI{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    CHECK_VK_ERROR_AND_THROW(vkCreateFence(m_valDevice.GetHandle(), &fenceCI, nullptr, &fence),
                             "Failed to create fence");
    m_fences.push_back(fence);
    m_numActiveFences++;

    return m_fences.back();
}

SynObjPool::~SynObjPool()
{
    for (auto& fence : m_fences)
    {
        vkDestroyFence(m_valDevice.GetHandle(), fence, nullptr);
    }
    for (auto& sem : m_semaphores)
    {
        vkDestroySemaphore(m_valDevice.GetHandle(), sem, nullptr);
    }
    m_fences.clear();
    m_semaphores.clear();
}

VkSemaphore SynObjPool::RequestSemaphore()
{
    if (m_numActiveSemaphores < m_semaphores.size())
    {
        return m_semaphores[m_numActiveSemaphores++];
    }
    VkSemaphore semaphore{VK_NULL_HANDLE};
    VkSemaphoreCreateInfo semaphoreCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK_VK_ERROR_AND_THROW(
        vkCreateSemaphore(m_valDevice.GetHandle(), &semaphoreCI, nullptr, &semaphore),
        "Failed to create semaphore");
    m_semaphores.push_back(semaphore);
    m_numActiveSemaphores++;
    return m_semaphores.back();
}

VkSemaphore SynObjPool::RequestSemaphoreWithOwnership()
{
    if (m_numActiveSemaphores < m_semaphores.size())
    {
        auto sem = m_semaphores.back();
        m_semaphores.pop_back();
        return sem;
    }
    VkSemaphore semaphore{VK_NULL_HANDLE};
    VkSemaphoreCreateInfo semaphoreCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK_VK_ERROR_AND_THROW(
        vkCreateSemaphore(m_valDevice.GetHandle(), &semaphoreCI, nullptr, &semaphore),
        "Failed to create semaphore");
    return semaphore;
}

void SynObjPool::ReleaseSemaphoreWithOwnership(VkSemaphore semaphore)
{
    m_releasedSemaphores.push_back(semaphore);
}

void SynObjPool::WaitForFences(uint32_t timeout) const
{
    if (m_numActiveFences < 1 || m_fences.empty())
    {
        return;
    }
    CHECK_VK_ERROR_AND_THROW(vkWaitForFences(m_valDevice.GetHandle(), util::ToU32(m_fences.size()),
                                             m_fences.data(), true, timeout),
                             "Failed to wait for fences");
}

void SynObjPool::ResetFences()
{
    if (m_numActiveFences < 1 || m_fences.empty())
    {
        return;
    }
    CHECK_VK_ERROR_AND_THROW(
        vkResetFences(m_valDevice.GetHandle(), util::ToU32(m_fences.size()), m_fences.data()),
        "Failed to reset fences");
    m_numActiveFences = 0;
}

void SynObjPool::ResetSemaphores()
{
    m_numActiveSemaphores = 0;
    for (auto& sem : m_releasedSemaphores)
    {
        m_semaphores.push_back(sem);
    }
    m_releasedSemaphores.clear();
}
} // namespace zen::val