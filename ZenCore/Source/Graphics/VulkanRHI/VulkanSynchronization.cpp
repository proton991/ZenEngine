#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"

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
        if (createSignaled)
        {
            fence->m_state = VulkanFence::State::eSignaled;
        }
        return fence;
    }
    VulkanFence* newFence = new VulkanFence(this, createSignaled);
    m_usedFences.push_back(newFence);
    return newFence;
}

void VulkanFenceManager::ReleaseFence(VulkanFence*& fence)
{
    ResetFence(fence);
    int size = m_usedFences.size();
    if (!m_usedFences.empty())
    {
        auto it = m_usedFences.begin();
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

bool VulkanFenceManager::IsFenceSignaled(VulkanFence* fence)
{
    if (fence->IsSignaled())
    {
        return true;
    }
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
    if (IsFenceSignaled(fence))
    {
        fence->m_state = VulkanFence::State::eSignaled;
        return true;
    }
    VkResult result = vkWaitForFences(m_device->GetVkHandle(), 1, &fence->m_fence, true, timeNS);
    if (result == VK_SUCCESS)
    {
        fence->m_state = VulkanFence::State::eSignaled;
        return true;
    }
    LOGE("vkWaitForFences Failed with error {}", result);
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
    if (!fence->IsSignaled())
    {
        WaitForFence(fence, timeNS);
    }
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

VulkanSemaphore::VulkanSemaphore(VulkanDevice* device) : m_device(device)
{
    VkSemaphoreCreateInfo semaphoreCI;
    InitVkStruct(semaphoreCI, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    VKCHECK(vkCreateSemaphore(m_device->GetVkHandle(), &semaphoreCI, nullptr, &m_semaphore));
}

void VulkanSemaphore::SetDebugName(const char* pName)
{
    m_device->SetObjectName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(m_semaphore),
                            pName);
}

VulkanSemaphore::~VulkanSemaphore()
{
    vkDestroySemaphore(m_device->GetVkHandle(), m_semaphore, nullptr);
    m_semaphore = VK_NULL_HANDLE;
}

void VulkanSemaphoreManager::Destroy()
{
#if defined(ZEN_DEBUG)
    VERIFY_EXPR(m_allocatedSemaphoreCount == m_usedSemaphores.size() + m_freeSemaphores.size());
#endif
    while (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* sem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        delete sem;
    }
    for (VulkanSemaphore* sem : m_usedSemaphores)
    {
        delete sem;
    }
}

VulkanSemaphore* VulkanSemaphoreManager::GetOrCreateSemaphore()
{
    if (!m_freeSemaphores.empty())
    {
        VulkanSemaphore* sem = m_freeSemaphores.front();
        m_freeSemaphores.pop();
        m_usedSemaphores.push_back(sem);
        return sem;
    }
    VulkanSemaphore* newSem = new VulkanSemaphore(m_device);
    m_usedSemaphores.push_back(newSem);
    m_allocatedSemaphoreCount++;
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
        ++it;
    }
    m_freeSemaphores.push(sem);
    // sem = nullptr;
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

void VulkanPipelineBarrier::Execute(VulkanCommandBuffer* cmdBuffer)
{
    VkPipelineStageFlags srcStageFlags = 0;
    VkPipelineStageFlags dstStageFlags = 0;
    for (const auto& imageBarrier : m_imageBarriers)
    {
        srcStageFlags |= VkLayoutToPipelineStageFlags(imageBarrier.oldLayout);
        dstStageFlags |= VkLayoutToPipelineStageFlags(imageBarrier.newLayout);
    }
    if (!m_imageBarriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer->GetVkHandle(), srcStageFlags, dstStageFlags, 0, 0, nullptr,
                             0, nullptr, m_imageBarriers.size(), m_imageBarriers.data());
    }
}

void VulkanPipelineBarrier::Execute(VulkanCommandBuffer* cmdBuffer,
                                    BitField<PipelineStageBits> srcStages,
                                    BitField<PipelineStageBits> dstStages)
{
    if (!m_memoryBarriers.empty() || !m_bufferBarriers.empty() || !m_imageBarriers.empty())
    {
        vkCmdPipelineBarrier(cmdBuffer->GetVkHandle(), srcStages, dstStages, 0,
                             m_memoryBarriers.size(), m_memoryBarriers.data(),
                             m_bufferBarriers.size(), m_bufferBarriers.data(),
                             m_imageBarriers.size(), m_imageBarriers.data());
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
} // namespace zen::rhi