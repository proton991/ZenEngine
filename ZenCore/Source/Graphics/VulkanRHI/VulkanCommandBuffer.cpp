#include "Graphics/RHI/RHIOptions.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"

namespace zen::rhi
{
VulkanCommandBufferPool::~VulkanCommandBufferPool()
{
    for (VulkanCommandBuffer* cmdBuffer : m_usedCmdBuffers)
    {
        delete cmdBuffer;
    }
    for (VulkanCommandBuffer* cmdBuffer : m_freeCmdBuffers)
    {
        delete cmdBuffer;
    }
    vkDestroyCommandPool(m_device->GetVkHandle(), m_cmdPool, nullptr);
}

void VulkanCommandBufferPool::Init(uint32_t queueFamilyIndex)
{
    VkCommandPoolCreateInfo cmdPoolCI;
    InitVkStruct(cmdPoolCI, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    cmdPoolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // reset cmd buffer
    cmdPoolCI.queueFamilyIndex = queueFamilyIndex;
    VKCHECK(vkCreateCommandPool(m_device->GetVkHandle(), &cmdPoolCI, nullptr, &m_cmdPool));
}

void VulkanCommandBufferPool::RefreshFenceStatus(VulkanCommandBuffer* skipCmdBuffer)
{
    for (VulkanCommandBuffer* cmdBuffer : m_usedCmdBuffers)
    {
        if (cmdBuffer != skipCmdBuffer)
        {
            cmdBuffer->RefreshFenceStatus();
        }
    }
}

VulkanCommandBuffer* VulkanCommandBufferPool::CreateCmdBuffer(bool isUploadOnly)
{
    auto it = m_freeCmdBuffers.end();
    while (it != m_freeCmdBuffers.begin())
    {
        --it;
        VulkanCommandBuffer* cmdBuffer = *it;
        if (cmdBuffer->m_isUploadOnly == isUploadOnly)
        {
            m_freeCmdBuffers.erase(it);
            cmdBuffer->AllocMemory();
            m_usedCmdBuffers.push_back(cmdBuffer);
            return cmdBuffer;
        }
    }
    VulkanCommandBuffer* newCmdBuffer = new VulkanCommandBuffer(this, isUploadOnly);
    m_usedCmdBuffers.push_back(newCmdBuffer);
    return newCmdBuffer;
}

void VulkanCommandBufferPool::FreeUnusedCmdBuffers(VulkanQueue* queue)
{
    VulkanCommandBuffer* lastSubmittedCommandBuffer = queue->GetLastSubmittedCmdBuffer();

    auto it = m_usedCmdBuffers.end();
    while (it != m_usedCmdBuffers.begin())
    {
        --it;
        VulkanCommandBuffer* cmdBuffer = *it;
        if (cmdBuffer != lastSubmittedCommandBuffer &&
                cmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
            cmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
        {
            cmdBuffer->FreeMemory();
            it = m_usedCmdBuffers.erase(it);
            m_freeCmdBuffers.push_back(cmdBuffer);
        }
    }
}

VulkanCommandBuffer::VulkanCommandBuffer(VulkanCommandBufferPool* pool, bool isUploadOnly) :
    m_cmdBufferPool(pool), m_isUploadOnly(isUploadOnly)
{
    AllocMemory();

    m_fence = m_cmdBufferPool->GetDevice()->GetFenceManager()->CreateFence();
}

VulkanCommandBuffer::~VulkanCommandBuffer()
{
    VulkanFenceManager* fenceManager = m_cmdBufferPool->GetDevice()->GetFenceManager();

    if (m_state == State::eSubmitted)
    {
        // wait 33ms
        uint64_t timeNS = 33 * 1000 * 1000LL;
        fenceManager->WaitAndReleaseFence(m_fence, timeNS);
    }
    else
    {
        fenceManager->ReleaseFence(m_fence);
    }

    if (m_state != State::eNotAllocated)
    {
        FreeMemory();
    }
}

void VulkanCommandBuffer::AddWaitSemaphore(VkPipelineStageFlags waitFlags, VulkanSemaphore* sem)
{
    std::vector<VulkanSemaphore*> vec = {sem};
    AddWaitSemaphore(waitFlags, vec);
}

void VulkanCommandBuffer::AddWaitSemaphore(VkPipelineStageFlags waitFlags,
                                           const std::vector<VulkanSemaphore*>& sems)
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
    allocInfo.commandPool        = m_cmdBufferPool->GetVkCmdPool();
    allocInfo.commandBufferCount = 1;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    VKCHECK(vkAllocateCommandBuffers(m_cmdBufferPool->GetDevice()->GetVkHandle(), &allocInfo,
                                     &m_cmdBuffer));
    m_state = State::eReadyForBegin;
}

void VulkanCommandBuffer::RefreshFenceStatus()
{
    if (m_state == State::eSubmitted)
    {
        VulkanFenceManager* fenceManager = m_cmdBufferPool->GetDevice()->GetFenceManager();
        if (fenceManager->IsFenceSignaled(m_fence))
        {
            m_submittedWaitSemaphores.clear();
            m_fence->GetOwner()->ResetFence(m_fence);
            m_state = State::eNeedReset;
            m_fenceSignaledCounter++;
        }
    }
}

void VulkanCommandBuffer::FreeMemory()
{
    vkFreeCommandBuffers(m_cmdBufferPool->GetDevice()->GetVkHandle(),
                         m_cmdBufferPool->GetVkCmdPool(), 1, &m_cmdBuffer);
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
        if (m_state != State::eReadyForBegin)
        {
            int a = 1;
        }
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

VulkanCommandBufferManager::VulkanCommandBufferManager(VulkanDevice* device, VulkanQueue* queue) :
    m_device(device), m_queue(queue), m_pool(device, this)
{
    m_pool.Init(queue->GetFamilyIndex());
    // allocate active cmd buffer
    m_activeCmdBuffer = m_pool.CreateCmdBuffer(false);
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        m_activeCmdBufferSemaphore = m_device->GetSemaphoreManager()->GetOrCreateSemaphore();
    }
}

VulkanCommandBuffer* VulkanCommandBufferManager::GetActiveCommandBuffer()
{
    // if (m_uploadCmdBuffer != nullptr)
    // {
    //     SubmitUploadCmdBuffer();
    // }
    m_activeCmdBuffer->Begin();
    return m_activeCmdBuffer;
}

VulkanCommandBuffer* VulkanCommandBufferManager::GetUploadCommandBuffer()
{
    if (!m_uploadCmdBuffer)
    {
        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            m_uploadCmdBufferSemaphore = m_device->GetSemaphoreManager()->GetOrCreateSemaphore();
        }
        for (auto i = 0; i < m_pool.m_usedCmdBuffers.size(); i++)
        {
            VulkanCommandBuffer* cmdBuffer = m_pool.m_usedCmdBuffers[i];
            cmdBuffer->RefreshFenceStatus();
            if (cmdBuffer->m_isUploadOnly)
            {
                if (cmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
                    cmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
                {
                    m_uploadCmdBuffer = cmdBuffer;
                    cmdBuffer->Begin();
                    return m_uploadCmdBuffer;
                }
            }
        }
        // create a new one
        m_uploadCmdBuffer = m_pool.CreateCmdBuffer(true);
        m_uploadCmdBuffer->Begin();
    }
    return m_uploadCmdBuffer;
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer(
    const std::vector<VulkanSemaphore*>& signalSemaphores)
{
    std::vector<VkSemaphore> semaphoreHandles;
    semaphoreHandles.reserve(signalSemaphores.size());
    for (VulkanSemaphore* sem : signalSemaphores)
    {
        semaphoreHandles.push_back(sem->GetVkHandle());
    }
    if (!m_activeCmdBuffer->IsSubmitted() && m_activeCmdBuffer->HasBegun())
    {
        m_activeCmdBuffer->End();

        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            for (VulkanSemaphore* sem : m_uploadCompleteSemaphores)
            {
                m_activeCmdBuffer->AddWaitSemaphore(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sem);
                m_device->GetSemaphoreManager()->ReleaseSemaphore(sem);
            }
            semaphoreHandles.push_back(m_activeCmdBufferSemaphore->GetVkHandle());
            // submit via VulkanQueue
            m_queue->Submit(m_activeCmdBuffer, semaphoreHandles.size(), semaphoreHandles.data());

            m_renderCompleteSemaphores.push_back(m_activeCmdBufferSemaphore);
            m_activeCmdBufferSemaphore = nullptr;

            m_uploadCompleteSemaphores.clear();
        }
        else
        {
            m_queue->Submit(m_activeCmdBuffer, semaphoreHandles.size(), semaphoreHandles.data());
        }
    }

    m_activeCmdBuffer = nullptr;
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer(VulkanSemaphore* signalSemaphore)
{
    std::vector<VulkanSemaphore*> vec = {signalSemaphore};
    SubmitActiveCmdBuffer(vec);
}

void VulkanCommandBufferManager::SubmitActiveCmdBuffer()
{
    std::vector<VulkanSemaphore*> vec = {};
    SubmitActiveCmdBuffer(vec);
}

void VulkanCommandBufferManager::SubmitActiveCmdBufferForPresent(VulkanSemaphore* signalSemaphore)
{
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        if (signalSemaphore)
        {
            VkSemaphore singalSems[2] = {signalSemaphore->GetVkHandle(),
                                         m_activeCmdBufferSemaphore->GetVkHandle()};
            m_queue->Submit(m_activeCmdBuffer, 2, singalSems);
        }
        else
        {
            m_queue->Submit(m_activeCmdBuffer, m_activeCmdBufferSemaphore->GetVkHandle());
        }

        m_renderCompleteSemaphores.push_back(m_activeCmdBufferSemaphore);
        m_activeCmdBufferSemaphore = nullptr;
    }
    else
    {
        if (signalSemaphore)
        {
            m_queue->Submit(m_activeCmdBuffer, signalSemaphore->GetVkHandle());
        }
        else
        {
            m_queue->Submit(m_activeCmdBuffer);
        }
    }
}

void VulkanCommandBufferManager::SubmitUploadCmdBuffer()
{
    if (!m_uploadCmdBuffer->IsSubmitted() && m_uploadCmdBuffer->HasBegun())
    {
        m_uploadCmdBuffer->End();
        if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
        {
            for (VulkanSemaphore* sem : m_renderCompleteSemaphores)
            {
                m_uploadCmdBuffer->AddWaitSemaphore(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sem);
                m_device->GetSemaphoreManager()->ReleaseSemaphore(sem);
            }
            VkSemaphore semaphore = m_uploadCmdBufferSemaphore->GetVkHandle();
            m_queue->Submit(m_uploadCmdBuffer, 1, &semaphore);

            m_uploadCompleteSemaphores.push_back(m_uploadCmdBufferSemaphore);
            m_uploadCmdBufferSemaphore = nullptr;

            m_renderCompleteSemaphores.clear();
        }
        else
        {
            m_queue->Submit(m_uploadCmdBuffer, 0, nullptr);
        }
    }
    m_uploadCmdBuffer = nullptr;
}

void VulkanCommandBufferManager::SetupNewActiveCmdBuffer()
{
    if (RHIOptions::GetInstance().VKUploadCmdBufferSemaphore())
    {
        m_activeCmdBufferSemaphore = m_device->GetSemaphoreManager()->GetOrCreateSemaphore();
    }
    for (auto i = 0; i < m_pool.m_usedCmdBuffers.size(); i++)
    {
        VulkanCommandBuffer* cmdBuffer = m_pool.m_usedCmdBuffers[i];
        cmdBuffer->RefreshFenceStatus();
        if (!cmdBuffer->m_isUploadOnly)
        {
            if (cmdBuffer->m_state == VulkanCommandBuffer::State::eReadyForBegin ||
                cmdBuffer->m_state == VulkanCommandBuffer::State::eNeedReset)
            {
                m_activeCmdBuffer = cmdBuffer;
                return;
            }
            else
            {
                VERIFY_EXPR(cmdBuffer->m_state == VulkanCommandBuffer::State::eSubmitted);
            }
        }
    }
    // create a new one
    m_activeCmdBuffer = m_pool.CreateCmdBuffer(false);
}

void VulkanCommandBufferManager::FreeUnusedCmdBuffers()
{
    m_pool.FreeUnusedCmdBuffers(m_queue);
}

void VulkanCommandBufferManager::WaitForCmdBuffer(VulkanCommandBuffer* cmdBuffer,
                                                  float timeInSecondsToWait)
{
    bool success = m_device->GetFenceManager()->WaitForFence(cmdBuffer->m_fence,
                                                             (uint64_t)(timeInSecondsToWait * 1e9));
    VERIFY_EXPR(success);
    cmdBuffer->RefreshFenceStatus();
}
} // namespace zen::rhi