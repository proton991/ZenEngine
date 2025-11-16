#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
void VulkanRHI::DestroyBuffer(RHIBuffer* pBuffer)
{
    pBuffer->ReleaseReference();
}
// todo: refactor all rhi::BufferHandle related functions, use RHIBuffer* instead of BufferHandle
RHIBuffer* RHIBuffer::Create(const RHIBufferCreateInfo& createInfo)
{
    RHIBuffer* pBuffer =
        VersatileResource::Alloc<FVulkanBuffer>(GVulkanRHI->GetResourceAllocator(), createInfo);

    pBuffer->Init();

    return pBuffer;
}

void FVulkanBuffer::Init()
{
    VkBufferCreateInfo bufferCI;
    InitVkStruct(bufferCI, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bufferCI.size        = static_cast<VkDeviceSize>(m_requiredSize);
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferCI.usage       = ToVkBufferUsageFlags(m_usageFlags);

    GVkMemAllocator->AllocBuffer(m_requiredSize, &bufferCI, m_allocateType, &m_vkBuffer,
                                 &m_memAlloc);
    m_allocatedSize = m_memAlloc.info.size;
}

void FVulkanBuffer::Destroy()
{
    if (m_bufferView != VK_NULL_HANDLE)
    {
        vkDestroyBufferView(GVulkanRHI->GetVkDevice(), m_bufferView, nullptr);
    }

    GVkMemAllocator->FreeBuffer(m_vkBuffer, m_memAlloc);
    this->~FVulkanBuffer();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

uint8_t* FVulkanBuffer::Map()
{
    return GVkMemAllocator->MapBuffer(m_memAlloc);
}

void FVulkanBuffer::Unmap()
{
    GVkMemAllocator->UnmapBuffer(m_memAlloc);
}

void FVulkanBuffer::SetTexelFormat(DataFormat format)
{
    VkBufferViewCreateInfo bufferViewCI;
    InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
    bufferViewCI.buffer = m_vkBuffer;
    bufferViewCI.format = ToVkFormat(format);
    bufferViewCI.range  = m_allocatedSize;
    bufferViewCI.offset = 0;

    VKCHECK(vkCreateBufferView(GVulkanRHI->GetVkDevice(), &bufferViewCI, nullptr, &m_bufferView));
}


BufferHandle VulkanRHI::CreateBuffer(uint32_t size,
                                     BitField<BufferUsageFlagBits> usageFlags,
                                     BufferAllocateType allocateType)
{
    VulkanBuffer* vulkanBuffer = VersatileResource::Alloc<VulkanBuffer>(m_resourceAllocator);

    VkBufferCreateInfo bufferCI;
    InitVkStruct(bufferCI, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bufferCI.size        = static_cast<VkDeviceSize>(size);
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferCI.usage       = ToVkBufferUsageFlags(usageFlags);

    m_vkMemAllocator->AllocBuffer(size, &bufferCI, allocateType, &vulkanBuffer->buffer,
                                  &vulkanBuffer->memAlloc);
    vulkanBuffer->requiredSize  = size;
    vulkanBuffer->allocatedSize = vulkanBuffer->memAlloc.info.size;
    return BufferHandle(vulkanBuffer);
}

uint8_t* VulkanRHI::MapBuffer(BufferHandle bufferHandle)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
    return m_vkMemAllocator->MapBuffer(vulkanBuffer->memAlloc);
}

void VulkanRHI::UnmapBuffer(BufferHandle bufferHandle)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
    m_vkMemAllocator->UnmapBuffer(vulkanBuffer->memAlloc);
}


void VulkanRHI::DestroyBuffer(BufferHandle bufferHandle)
{
    VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
    if (vulkanBuffer->bufferView != VK_NULL_HANDLE)
    {
        vkDestroyBufferView(m_device->GetVkHandle(), vulkanBuffer->bufferView, nullptr);
    }
    m_vkMemAllocator->FreeBuffer(vulkanBuffer->buffer, vulkanBuffer->memAlloc);
    VersatileResource::Free(m_resourceAllocator, vulkanBuffer);
}

void VulkanRHI::SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format)
{
    VulkanBuffer* buffer = TO_VK_BUFFER(bufferHandle);
    VERIFY_EXPR(buffer->bufferView == VK_NULL_HANDLE);

    VkBufferViewCreateInfo bufferViewCI;
    InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
    bufferViewCI.buffer = buffer->buffer;
    bufferViewCI.format = ToVkFormat(format);
    bufferViewCI.range  = buffer->allocatedSize;
    bufferViewCI.offset = 0;

    VKCHECK(
        vkCreateBufferView(m_device->GetVkHandle(), &bufferViewCI, nullptr, &buffer->bufferView));
}

} // namespace zen::rhi