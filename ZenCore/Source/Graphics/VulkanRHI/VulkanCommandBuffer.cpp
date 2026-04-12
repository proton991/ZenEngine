#include "Memory/Memory.h"
#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"

namespace zen
{
VulkanCommandBufferPool::~VulkanCommandBufferPool()
{
    for (VulkanCommandBuffer* pCmdBuffer : m_usedCmdBuffers)
    {
        ZEN_DELETE(pCmdBuffer);
    }
    for (VulkanCommandBuffer* pCmdBuffer : m_freeCmdBuffers)
    {
        ZEN_DELETE(pCmdBuffer);
    }
    vkDestroyCommandPool(m_pDevice->GetVkHandle(), m_cmdPool, nullptr);
}

void VulkanCommandBufferPool::Init(uint32_t queueFamilyIndex)
{
    VkCommandPoolCreateInfo cmdPoolCI;
    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    VKCHECK(vkCreateCommandPool(m_pDevice->GetVkHandle(), &cmdPoolCI, nullptr, &m_cmdPool));
}

void VulkanCommandBufferPool::RefreshFenceStatus(VulkanCommandBuffer* pSkipCmdBuffer)
{
    for (VulkanCommandBuffer* pCmdBuffer : m_usedCmdBuffers)
    {
        if (pCmdBuffer != pSkipCmdBuffer)
        {
            pCmdBuffer->RefreshFenceStatus();
        }
    }
}

VulkanCommandBuffer* VulkanCommandBufferPool::CreateCmdBuffer(bool isUploadOnly)
{
    auto it = m_freeCmdBuffers.end();
    while (it != m_freeCmdBuffers.begin())
    {
        --it;
        VulkanCommandBuffer* pCmdBuffer = *it;
        if (pCmdBuffer->m_isUploadOnly == isUploadOnly)
        {
            m_freeCmdBuffers.erase(it);
            pCmdBuffer->AllocMemory();
            m_usedCmdBuffers.push_back(pCmdBuffer);
            return pCmdBuffer;
        }
    }
    VulkanCommandBuffer* pNewCmdBuffer = ZEN_NEW() VulkanCommandBuffer(this, isUploadOnly);
    m_usedCmdBuffers.push_back(pNewCmdBuffer);
    return pNewCmdBuffer;
}

void VulkanCommandBufferPool::FreeUnusedCmdBuffers(VulkanQueue* pQueue)
{
    VulkanCommandBuffer* pLastSubmittedCommandBuffer = pQueue->GetLastSubmittedCmdBuffer();

    auto it = m_usedCmdBuffers.end();
    while (it != m_usedCmdBuffers.begin())
    {
        --it;
        VulkanCommandBuffer* pCmdBuffer = *it;
        if (pCmdBuffer != pLastSubmittedCommandBuffer &&
                pCmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
            pCmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
        {
            pCmdBuffer->FreeMemory();
            it = m_usedCmdBuffers.erase(it);
            m_freeCmdBuffers.push_back(pCmdBuffer);
        }
    }
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBufferPool* pPool, bool isUploadOnly) :
    m_pCmdBufferPool(pPool), m_isUploadOnly(isUploadOnly)
{
    AllocMemory();

    m_pFence = m_pCmdBufferPool->GetDevice()->GetFenceManager()->CreateFence();
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
    VulkanFenceManager* pFenceManager = m_pCmdBufferPool->GetDevice()->GetFenceManager();

    if (m_state == State::eSubmitted)
    {
        // wait 33ms
        uint64_t timeNS = 33 * 1000 * 1000LL;
        pFenceManager->WaitAndReleaseFence(m_pFence, timeNS);
    }
    else
    {
        pFenceManager->ReleaseFence(m_pFence);
    }

    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void VulkanCommandBuffer::AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* pSem)
{
    HeapVector<VulkanSemaphore*> vec = {pSem};
    AddWaitSemaphore(waitFlags, vec);
}

void VulkanCommandBuffer::AddWaitSemaphore(VkPipelineStageFlags waitFlags,
                                           const HeapVector<VulkanSemaphore*>& sems)
{
    for (auto sem : sems)
    {
        m_waitFlags.push_back(waitFlags);
        m_waitSemaphores.push_back(sem);
    }
}

void VulkanCommandBuffer::AllocMemory()
{
    VkCommandBufferAllocateInfo allocInfo;
    InitVkStruct(allocInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocInfo.commandPool        = m_pCmdBufferPool->GetVkCmdPool();
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    VKCHECK(vkAllocateCommandBuffers(m_pCmdBufferPool->GetDevice()->GetVkHandle(), &allocInfo,
                                     &m_cmdBuffer));
    m_state = State::eReadyForBegin;
}

void VulkanCommandBuffer::RefreshFenceStatus()
{
    if (m_state == State::eSubmitted)
    {
        VulkanFenceManager* pFenceManager = m_pCmdBufferPool->GetDevice()->GetFenceManager();
        if (pFenceManager->IsFenceSignaled(m_pFence))
        {
            m_submittedWaitSemaphores.clear();
            m_pFence->GetOwner()->ResetFence(m_pFence);
            m_state = State::eNeedReset;
            m_fenceSignaledCounter++;
        }
    }
}

void VulkanCommandBuffer::FreeMemory()
{
    vkFreeCommandBuffers(m_pCmdBufferPool->GetDevice()->GetVkHandle(),
                         m_pCmdBufferPool->GetVkCmdPool(), 1, &m_cmdBuffer);
    m_state     = State::eNotAllocated;
    m_cmdBuffer = VK_NULL_HANDLE;
}

void VulkanCommandBuffer::Begin()
{
    if (m_state == State::eNeedReset)
    {
        vkResetCommandBuffer(m_cmdBuffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
    }
    else
    {
        VERIFY_EXPR(m_state == State::eReadyForBegin);
        // if (m_state != State::eReadyForBegin)
        // {
        //     int a = 1;
        // }
    }
    m_state = State::eHasBegun;
    VkCommandBufferBeginInfo beginInfo;
    InitVkStruct(beginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(m_cmdBuffer, &beginInfo));
}

void VulkanCommandBuffer::End()
{
    vkEndCommandBuffer(m_cmdBuffer);
    m_state = State::eHasEnded;
}

void VulkanCommandBuffer::EndRenderPass()
{
    vkCmdEndRenderPass(m_cmdBuffer);
}

VulkanCommandBufferManager::VulkanCommandBufferManager(VulkanDevice* pDevice, VulkanQueue* pQueue) :
    m_pDevice(pDevice), m_pQueue(pQueue), m_pool(pDevice, this)
{
    m_pool.Init(pQueue->GetFamilyIndex());
    // allocate active cmd buffer
    m_pActiveCmdBuffer = m_pool.CreateCmdBuffer(false);
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        m_pActiveCmdBufferSemaphore = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
    }
}

VulkanCommandBuffer* VulkanCommandBufferManager::GetActiveCommandBuffer()
{
    // if (m_uploadCmdBuffer != nullptr)
    // {
    //     SubmitUploadCmdBuffer();
    // }
    m_pActiveCmdBuffer->Begin();
    return m_pActiveCmdBuffer;
}

VulkanCommandBuffer* VulkanCommandBufferManager::GetUploadCommandBuffer()
{
    if (!m_pUploadCmdBuffer)
    {
        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            m_pUploadCmdBufferSemaphore = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
        }
        for (auto i = 0; i < m_pool.m_usedCmdBuffers.size(); i++)
        {
            VulkanCommandBuffer* pCmdBuffer = m_pool.m_usedCmdBuffers[i];
            pCmdBuffer->RefreshFenceStatus();
            if (pCmdBuffer->m_isUploadOnly)
            {
                if (pCmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
                    pCmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
                {
                    m_pUploadCmdBuffer = pCmdBuffer;
                    pCmdBuffer->Begin();
                    return m_pUploadCmdBuffer;
                }
            }
        }
        // create a new one
        m_pUploadCmdBuffer = m_pool.CreateCmdBuffer(true);
        m_pUploadCmdBuffer->Begin();
    }
    return m_pUploadCmdBuffer;
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer(
    const HeapVector<VulkanSemaphore*>& signalSemaphores)
{
    HeapVector<VkSemaphore> semaphoreHandles;
    semaphoreHandles.reserve(signalSemaphores.size());
    for (VulkanSemaphore* pSem : signalSemaphores)
    {
        semaphoreHandles.push_back(pSem->GetVkHandle());
    }
    if (!m_pActiveCmdBuffer->IsSubmitted() && m_pActiveCmdBuffer->HasBegun())
    {
        m_pActiveCmdBuffer->End();

        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            for (VulkanSemaphore* pSem : m_uploadCompleteSemaphores)
            {
                m_pActiveCmdBuffer->AddWaitSemaphore(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pSem);
                m_pDevice->GetSemaphoreManager()->ReleaseSemaphore(pSem);
            }
            semaphoreHandles.push_back(m_pActiveCmdBufferSemaphore->GetVkHandle());
            // submit via VulkanQueue
            m_pQueue->Submit(m_pActiveCmdBuffer, semaphoreHandles.size(), semaphoreHandles.data());

            m_renderCompleteSemaphores.push_back(m_pActiveCmdBufferSemaphore);
            m_pActiveCmdBufferSemaphore = nullptr;

            m_uploadCompleteSemaphores.clear();
        }
        else
        {
            m_pQueue->Submit(m_pActiveCmdBuffer, semaphoreHandles.size(), semaphoreHandles.data());
        }
    }

    m_pActiveCmdBuffer = nullptr;
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer(VulkanSemaphore* pSignalSemaphore)
{
    HeapVector<VulkanSemaphore*> vec = {pSignalSemaphore};
    SubmitActiveCmdBuffer(vec);
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer()
{
    HeapVector<VulkanSemaphore*> vec = {};
    SubmitActiveCmdBuffer(vec);
}

void VulkanCommandBufferManager::SubmitActiveCmdBufferForPresent(VulkanSemaphore* pSignalSemaphore)
{
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        if (pSignalSemaphore)
        {
            VkSemaphore singalSems[2] = {pSignalSemaphore->GetVkHandle(),
                                         m_pActiveCmdBufferSemaphore->GetVkHandle()};
            m_pQueue->Submit(m_pActiveCmdBuffer, 2, singalSems);
        }
        else
        {
            m_pQueue->Submit(m_pActiveCmdBuffer, m_pActiveCmdBufferSemaphore->GetVkHandle());
        }

        m_renderCompleteSemaphores.push_back(m_pActiveCmdBufferSemaphore);
        m_pActiveCmdBufferSemaphore = nullptr;
    }
    else
    {
        if (pSignalSemaphore)
        {
            m_pQueue->Submit(m_pActiveCmdBuffer, pSignalSemaphore->GetVkHandle());
        }
        else
        {
            m_pQueue->Submit(m_pActiveCmdBuffer);
        }
    }
}

void VulkanCommandBufferManager::SubmitUploadCmdBuffer()
{
    if (!m_pUploadCmdBuffer->IsSubmitted() && m_pUploadCmdBuffer->HasBegun())
    {
        m_pUploadCmdBuffer->End();
        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            for (VulkanSemaphore* pSem : m_renderCompleteSemaphores)
            {
                m_pUploadCmdBuffer->AddWaitSemaphore(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pSem);
                m_pDevice->GetSemaphoreManager()->ReleaseSemaphore(pSem);
            }
            VkSemaphore semaphore = m_pUploadCmdBufferSemaphore->GetVkHandle();
            m_pQueue->Submit(m_pUploadCmdBuffer, 1, &semaphore);

            m_uploadCompleteSemaphores.push_back(m_pUploadCmdBufferSemaphore);
            m_pUploadCmdBufferSemaphore = nullptr;

            m_renderCompleteSemaphores.clear();
        }
        else
        {
            m_pQueue->Submit(m_pUploadCmdBuffer, 0, nullptr);
        }
    }
    m_pUploadCmdBuffer = nullptr;
}

void VulkanCommandBufferManager::SetupNewActiveCmdBuffer()
{
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        m_pActiveCmdBufferSemaphore = m_pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
    }
    for (auto i = 0; i < m_pool.m_usedCmdBuffers.size(); i++)
    {
        VulkanCommandBuffer* pCmdBuffer = m_pool.m_usedCmdBuffers[i];
        pCmdBuffer->RefreshFenceStatus();
        if (!pCmdBuffer->m_isUploadOnly)
        {
            if (pCmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
                pCmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
            {
                m_pActiveCmdBuffer = pCmdBuffer;
                return;
            }
            else
            {
                VERIFY_EXPR(pCmdBuffer->m_state == VulkanCommandBuffer::State::eSubmitted);
            }
        }
    }
    // create a new one
    m_pActiveCmdBuffer = m_pool.CreateCmdBuffer(false);
}

void VulkanCommandBufferManager::FreeUnusedCmdBuffers()
{
    m_pool.FreeUnusedCmdBuffers(m_pQueue);
}

void VulkanCommandBufferManager::WaitForCmdBuffer(VulkanCommandBuffer* pCmdBuffer,
                                                  float timeInSecondsToWait)
{
    bool success = m_pDevice->GetFenceManager()->WaitForFence(pCmdBuffer->m_pFence,
                                                             (uint64_t)(timeInSecondsToWait * 1e9));
    VERIFY_EXPR(success);
    pCmdBuffer->RefreshFenceStatus();
}
} // namespace zen
