#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
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

    return BufferHandle(vulkanBuffer);
}


void VulkanRHI::DestroyBuffer(BufferHandle bufferHandle)
{
    VulkanBuffer* vulkanBuffer = reinterpret_cast<VulkanBuffer*>(bufferHandle.value);
    m_vkMemAllocator->FreeBuffer(vulkanBuffer->buffer, vulkanBuffer->memAlloc);
    VersatileResource::Free(m_resourceAllocator, vulkanBuffer);
}

} // namespace zen::rhi