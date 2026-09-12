#include <algorithm>
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"

namespace zen
{
VulkanFence::VulkanFence(VulkanFenceManager* pOwner, bool createSignaled) :
    m_pOwner(pOwner), m_state(createSignaled ? State::eSignaled : State::eInitial)
{
    VkFenceCreateInfo fenceCI;
    InitVkStruct(fenceCI, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    fenceCI.flags = createSignaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
    VKCHECK(vkCreateFence(m_pOwner->GetDevice()->GetVkHandle(), &fenceCI, nullptr, &m_fence));
}

void VulkanFenceManager::Destroy()
{
    VERIFY_EXPR(m_usedFences.empty());

    while (!m_freeFences.empty())
    {
        VulkanFence* pFence = m_freeFences.front();
        m_freeFences.pop();
        DestroyFence(pFence);
        ZEN_DELETE(pFence);
    }
}

VulkanFence* VulkanFenceManager::CreateFence(bool createSignaled)
{
    VulkanFence* result{};

    if (!m_freeFences.empty())
    {
        VulkanFence* pFence = m_freeFences.front();
        m_freeFences.pop();
        pFence->m_state =
            createSignaled ? VulkanFence::State::eSignaled : VulkanFence::State::eInitial;
        m_usedFences.push_back(pFence);
        result = pFence;
    }
    else
    {
        VulkanFence* pNewFence = ZEN_NEW() VulkanFence(this, createSignaled);
        m_usedFences.push_back(pNewFence);
        result = pNewFence;
    }

    return result;
}

void VulkanFenceManager::ReleaseFence(VulkanFence*& fence)
{
    ResetFence(fence);
    int size = m_usedFences.size();

    if (!m_usedFences.empty())
    {
        HeapVector<VulkanFence*>::iterator it = m_usedFences.begin();

        while (it != m_usedFences.end())
        {
            if (*it == fence)
            {
                m_usedFences.erase(it);
                break;
            }

            ++it;
        }
    }

    m_freeFences.push(fence);
    fence = nullptr;
}

bool VulkanFenceManager::IsFenceSignaled(VulkanFence* pFence)
{
    bool returnValue{};

    if (pFence->IsSignaled())
    {
        returnValue = true;
    }
    else
    {
        // double check
        VkResult result = vkGetFenceStatus(m_pDevice->GetVkHandle(), pFence->m_fence);

        if (result == VK_SUCCESS)
        {
            pFence->m_state = VulkanFence::State::eSignaled;
            returnValue     = true;
        }
        else
        {
            returnValue = false;
        }
    }

    return returnValue;
}

bool VulkanFenceManager::WaitForFence(VulkanFence* pFence, uint64_t timeNS)
{
    bool returnValue{};

    if (IsFenceSignaled(pFence))
    {
        pFence->m_state = VulkanFence::State::eSignaled;
        returnValue     = true;
    }
    else
    {
        VkResult result =
            vkWaitForFences(m_pDevice->GetVkHandle(), 1, &pFence->m_fence, true, timeNS);

        if (result == VK_SUCCESS)
        {
            pFence->m_state = VulkanFence::State::eSignaled;
            returnValue     = true;
        }
        else if (result == VK_TIMEOUT)
        {
            LOGI("vkWaitForFences timeout");

            returnValue = false;
        }
        else
        {
            LOGE("vkWaitForFences failed: {}", int32_t(result));

            returnValue = false;
        }
    }

    return returnValue;
}

void VulkanFenceManager::ResetFence(VulkanFence* pFence)
{
    if (pFence->m_state != VulkanFence::State::eInitial)
    {
        VKCHECK(vkResetFences(m_pDevice->GetVkHandle(), 1, &pFence->m_fence));
        pFence->m_state = VulkanFence::State::eInitial;
    }
}

void VulkanFenceManager::WaitAndReleaseFence(VulkanFence*& fence, uint64_t timeNS)
{
    if (!fence->IsSignaled())
    {
        WaitForFence(fence, timeNS);
    }

    ResetFence(fence);
    ReleaseFence(fence);
    fence = nullptr;
}

void VulkanFenceManager::DestroyFence(VulkanFence* pFence)
{
    vkDestroyFence(m_pDevice->GetVkHandle(), pFence->GetVkHandle(), nullptr);
    pFence->m_fence = VK_NULL_HANDLE;
    pFence->m_state = VulkanFence::State::eInitial;
}

VulkanSemaphore::VulkanSemaphore(VulkanDevice* pDevice,
                                 VkSemaphoreType semaphoreType,
                                 uint64_t initialValue) :
    m_pDevice(pDevice), m_type(semaphoreType)
{
    VkSemaphoreCreateInfo semaphoreCI;
    InitVkStruct(semaphoreCI, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    VkSemaphoreTypeCreateInfo semaphoreTypeCI;

    if (semaphoreType != VK_SEMAPHORE_TYPE_BINARY)
    {
        InitVkStruct(semaphoreTypeCI, VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        semaphoreTypeCI.semaphoreType = semaphoreType;
        semaphoreTypeCI.initialValue  = initialValue;
        semaphoreCI.pNext             = &semaphoreTypeCI;
    }

    const VkResult result =
        vkCreateSemaphore(m_pDevice->GetVkHandle(), &semaphoreCI, nullptr, &m_semaphore);
    if (result != VK_SUCCESS)
    {
        if (result == VK_ERROR_DEVICE_LOST && GVulkanRHI && GVulkanRHI->GetDevice() == m_pDevice)
        {
            GVulkanRHI->BlockSubmissions();
        }
        LOG_ERROR_AND_THROW("vkCreateSemaphore failed: {}", int32_t(result));
    }
}

void VulkanSemaphore::SetDebugName(NameID name)
{
    m_pDevice->SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(m_semaphore),
                             name);
}

uint64_t VulkanSemaphore::GetCounterValue() const
{
    uint64_t returnValue{};

    VERIFY_EXPR(IsTimeline());

    uint64_t value = 0;
    const VkResult result =
        vkGetSemaphoreCounterValue(m_pDevice->GetVkHandle(), m_semaphore, &value);

    if (result != VK_SUCCESS)
    {
        LOGE("Vulkan timeline counter query failed: {}", int32_t(result));

        returnValue = 0;
    }
    else
    {
        returnValue = value;
    }

    return returnValue;
}

bool VulkanSemaphore::Wait(uint64_t value, uint64_t timeNS) const
{
    bool returnValue{};

    VERIFY_EXPR(IsTimeline());

    VkSemaphoreWaitInfo waitInfo;
    InitVkStruct(waitInfo, VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO);
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores    = &m_semaphore;
    waitInfo.pValues        = &value;

    VkResult result = vkWaitSemaphores(m_pDevice->GetVkHandle(), &waitInfo, timeNS);

    if (result == VK_SUCCESS)
    {
        returnValue = true;
    }
    else if (result == VK_TIMEOUT)
    {
        returnValue = false;
    }
    else
    {
        LOGE("vkWaitSemaphores failed: {}", int32_t(result));

        returnValue = false;
    }

    return returnValue;
}

VulkanSemaphore::~VulkanSemaphore()
{
    vkDestroySemaphore(m_pDevice->GetVkHandle(), m_semaphore, nullptr);
    m_semaphore = VK_NULL_HANDLE;
}

void VulkanSemaphoreManager::Destroy()
{
#if defined(ZEN_DEBUG)
    VERIFY_EXPR(m_allocatedSemaphoreCount == m_usedSemaphores.size() + m_freeSemaphores.size());
#endif

    while (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* pSem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        ZEN_DELETE(pSem);
    }

    for (VulkanSemaphore* pSem : m_usedSemaphores)
    {
        ZEN_DELETE(pSem);
    }

    m_usedSemaphores.clear();
#if defined(ZEN_DEBUG)
    m_allocatedSemaphoreCount = 0;
#endif
}

VulkanSemaphore* VulkanSemaphoreManager::GetOrCreateSemaphore()
{
    VulkanSemaphore* result{};

    if (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* pSem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        m_usedSemaphores.push_back(pSem);
        result = pSem;
    }
    else
    {
        VulkanSemaphore* pNewSem = ZEN_NEW() VulkanSemaphore(m_pDevice);
        m_usedSemaphores.push_back(pNewSem);
#if defined(ZEN_DEBUG)
        m_allocatedSemaphoreCount++;
#endif
        result = pNewSem;
    }

    return result;
}

void VulkanSemaphoreManager::DestroySemaphore(VulkanSemaphore*& sem)
{
    if (sem == nullptr)
    {
        return;
    }
    auto* it = std::find(m_usedSemaphores.begin(), m_usedSemaphores.end(), sem);
    if (it == m_usedSemaphores.end())
    {
        LOG_ERROR_AND_THROW("Cannot destroy a semaphore not owned by this manager");
    }
    m_usedSemaphores.erase(it);
    ZEN_DELETE(sem);
    sem = nullptr;
#if defined(ZEN_DEBUG)
    --m_allocatedSemaphoreCount;
#endif
}

void VulkanSemaphoreManager::ReleaseSemaphore(VulkanSemaphore*& sem)
{
    if (sem != nullptr)
    {
        VulkanSemaphore** it = std::find(m_usedSemaphores.begin(), m_usedSemaphores.end(), sem);

        if (it != m_usedSemaphores.end())
        {
            // The next owner must not inherit acceptance of an earlier signal.
            // Keep the generation monotonic across reuse of the same object.
            sem->m_pSignalQueue           = nullptr;
            sem->m_signalSubmissionSerial = 0;
            m_usedSemaphores.erase(it);
            m_freeSemaphores.push(sem);
            sem = nullptr;
        }
        else
        {
            VERIFY_EXPR_MSG(false, "Cannot release a semaphore that is not in use");
        }
    }
}

void VulkanPipelineBarrier::AddImageBarrier(VkImage image,
                                            VkImageLayout srcLayout,
                                            VkImageLayout dstLayout,
                                            const VkImageSubresourceRange& range)
{
    const VkAccessFlags srcAccessFlags = VkLayoutToAccessFlags(srcLayout);
    const VkAccessFlags dstAccessFlags = VkLayoutToAccessFlags(dstLayout);

    VkImageMemoryBarrier barrier;
    InitVkStruct(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.image               = image;
    barrier.srcAccessMask       = srcAccessFlags;
    barrier.dstAccessMask       = dstAccessFlags;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout           = srcLayout;
    barrier.newLayout           = dstLayout;
    barrier.subresourceRange    = range;
    m_imageBarriers.emplace_back(barrier);
}

void VulkanPipelineBarrier::AddImageBarrier(VkImage image,
                                            VkImageLayout srcLayout,
                                            VkImageLayout dstLayout,
                                            const VkImageSubresourceRange& range,
                                            VkAccessFlags srcAccess,
                                            VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier;
    InitVkStruct(barrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.image               = image;
    barrier.srcAccessMask       = srcAccess;
    barrier.dstAccessMask       = dstAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout           = srcLayout;
    barrier.newLayout           = dstLayout;
    barrier.subresourceRange    = range;
    m_imageBarriers.emplace_back(barrier);
}

void VulkanPipelineBarrier::AddBufferBarrier(VkBuffer buffer,
                                             uint64_t offset,
                                             uint64_t size,
                                             VkAccessFlags srcAccess,
                                             VkAccessFlags dstAccess)
{
    VkBufferMemoryBarrier bufferBarrier;
    InitVkStruct(bufferBarrier, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
    bufferBarrier.buffer              = buffer;
    bufferBarrier.offset              = offset;
    bufferBarrier.size                = size;
    bufferBarrier.srcAccessMask       = srcAccess;
    bufferBarrier.dstAccessMask       = dstAccess;
    bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    m_bufferBarriers.emplace_back(bufferBarrier);
}

void VulkanPipelineBarrier::AddMemoryBarrier(VkAccessFlags srcAccess, VkAccessFlags dstAccess)
{
    VkMemoryBarrier memoryBarrier;
    InitVkStruct(memoryBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    memoryBarrier.srcAccessMask = srcAccess;
    memoryBarrier.dstAccessMask = dstAccess;
    m_memoryBarriers.emplace_back(memoryBarrier);
}

void VulkanPipelineBarrier::ExecuteImageBarriersOnly(VkCommandBuffer cmdBuffer)
{
    VkPipelineStageFlags srcStageFlags = 0;
    VkPipelineStageFlags dstStageFlags = 0;

    for (VkImageMemoryBarrier const& imageBarrier : m_imageBarriers)
    {
        srcStageFlags |= VkLayoutToPipelineStageFlags(imageBarrier.oldLayout);
        dstStageFlags |= VkLayoutToPipelineStageFlags(imageBarrier.newLayout);
    }

    if (!m_imageBarriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer, srcStageFlags, dstStageFlags, 0, 0, nullptr, 0, nullptr,
                             m_imageBarriers.size(), m_imageBarriers.data());
        m_imageBarriers.clear();
    }
}

void VulkanPipelineBarrier::Execute(VkCommandBuffer cmdBuffer,
                                    VkPipelineStageFlags srcStageFlags,
                                    VkPipelineStageFlags dstStageFlags)
{
    if (!m_memoryBarriers.empty() || !m_bufferBarriers.empty() || !m_imageBarriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer, srcStageFlags, dstStageFlags, 0, m_memoryBarriers.size(),
                             m_memoryBarriers.data(), m_bufferBarriers.size(),
                             m_bufferBarriers.data(), m_imageBarriers.size(),
                             m_imageBarriers.data());

        m_memoryBarriers.clear();
        m_bufferBarriers.clear();
        m_imageBarriers.clear();
    }
}

VkPipelineStageFlags VulkanPipelineBarrier::VkLayoutToPipelineStageFlags(VkImageLayout layout)
{
    VkPipelineStageFlags flags = 0;

    switch (layout)
    {
        case VK_IMAGE_LAYOUT_UNDEFINED:;
        case VK_IMAGE_LAYOUT_GENERAL: flags = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; break;

        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: flags = VK_PIPELINE_STAGE_TRANSFER_BIT; break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            flags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
            flags = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            break;

        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            flags = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            flags = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;

        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT; break;

        default: LOGE("Invalid Vulkan Image Layout") break;
    }

    return flags;
}

VkAccessFlags VulkanPipelineBarrier::VkLayoutToAccessFlags(VkImageLayout layout)
{
    VkAccessFlags flags = 0;

    switch (layout)
    {
        case VK_IMAGE_LAYOUT_UNDEFINED:;
        case VK_IMAGE_LAYOUT_GENERAL: flags = 0; break;

        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: flags = VK_ACCESS_TRANSFER_READ_BIT; break;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: flags = VK_ACCESS_TRANSFER_WRITE_BIT; break;

        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            flags = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
            flags = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
            flags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            break;

        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: flags = VK_ACCESS_SHADER_READ_BIT; break;

        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
            flags = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            break;

        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: flags = 0; break;

        default: LOGE("Invalid Vulkan Image Layout") break;
    }

    return flags;
}
} // namespace zen
