#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
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