#include "Graphics/VulkanRHI/VulkanResourceSharing.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen
{
RHIBuffer* VulkanRHI::CreateBuffer(const RHIBufferCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateBuffer(createInfo);
}

void VulkanRHI::DestroyBuffer(RHIBuffer* pBuffer)
{
    pBuffer->ReleaseReference();
}

RHIBuffer* VulkanResourceFactory::CreateBuffer(const RHIBufferCreateInfo& createInfo)
{
    RHIBuffer* pBuffer = VulkanBuffer::CreateObject(createInfo);

    return pBuffer;
}

// RHIBuffer* RHIBuffer::Create(const RHIBufferCreateInfo& createInfo)
// {
//     // RHIBuffer* pBuffer = VulkanBuffer::CreateObject(createInfo);
//     //
//     // return pBuffer;
//     return GVulkanRHI->GetResourceFactory()->CreateBuffer(createInfo);
// }

VulkanBuffer* VulkanBuffer::CreateObject(const RHIBufferCreateInfo& createInfo)
{
    VulkanBuffer* pBuffer =
        VersatileResource::AllocMem<VulkanBuffer>(GVulkanRHI->GetResourceAllocator());

    new (pBuffer) VulkanBuffer(createInfo);

    pBuffer->Init();

    return pBuffer;
}

void VulkanBuffer::Init()
{
    VkBufferCreateInfo bufferCI;
    InitVkStruct(bufferCI, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bufferCI.size        = static_cast<VkDeviceSize>(m_requiredSize);
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    bufferCI.usage       = ToVkBufferUsageFlags(m_usageFlags);

    const uint32_t graphicsQueueFamily = GVulkanRHI->GetDevice()->GetGfxQueue()->GetFamilyIndex();
    const uint32_t computeQueueFamily =
        GVulkanRHI->GetDevice()->GetComputeQueue()->GetFamilyIndex();
    const uint32_t transferQueueFamily =
        GVulkanRHI->GetDevice()->GetTransferQueue()->GetFamilyIndex();

    AllocateWithQueueSharing(
        bufferCI, graphicsQueueFamily, computeQueueFamily, transferQueueFamily,
        (bufferCI.usage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) !=
            0,
        [this, &bufferCI] {
            GVkMemAllocator->AllocBuffer(m_requiredSize, &bufferCI, m_allocateType, &m_vkBuffer,
                                         &m_memAlloc);
        });
}

void VulkanBuffer::Destroy()
{
    if (m_bufferView != VK_NULL_HANDLE)
    {
        vkDestroyBufferView(GVulkanRHI->GetVkDevice(), m_bufferView, nullptr);
    }

    GVkMemAllocator->FreeBuffer(m_vkBuffer, m_memAlloc);
    this->~VulkanBuffer();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

uint8_t* VulkanBuffer::Map()
{
    return GVkMemAllocator->MapBuffer(m_memAlloc);
}

void VulkanBuffer::Unmap()
{
    GVkMemAllocator->UnmapBuffer(m_memAlloc);
}

void VulkanBuffer::SetTexelFormat(DataFormat format)
{
    if (m_bufferView == VK_NULL_HANDLE)
    {
        VkBufferViewCreateInfo bufferViewCI;
        InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
        bufferViewCI.buffer = m_vkBuffer;
        bufferViewCI.format = ToVkFormat(format);
        bufferViewCI.range  = m_requiredSize;
        bufferViewCI.offset = 0;

        VkBufferView bufferView{VK_NULL_HANDLE};
        const VkResult result =
            vkCreateBufferView(GVulkanRHI->GetVkDevice(), &bufferViewCI, nullptr, &bufferView);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR_AND_THROW(
                fmt::format("vkCreateBufferView failed: {}", GetResultString(result)));
        }

        // Descriptors can retain this handle from recording through GPU completion.
        // Publish one immutable view only after creation succeeds.
        m_bufferView  = bufferView;
        m_texelFormat = format;
    }
    else if (format != m_texelFormat)
    {
        LOG_ERROR_AND_THROW("Cannot change an existing buffer's texel format");
    }
}

void VulkanUniformBufferAllocator::Init(uint32_t numSlots,
                                        uint32_t blockSize,
                                        uint32_t reservedBlocksPerSlot)
{
    VERIFY_EXPR(numSlots > 0);
    VERIFY_EXPR(blockSize > 0);
    VERIFY_EXPR(reservedBlocksPerSlot > 0);

    if (numSlots == 0 || blockSize == 0 || reservedBlocksPerSlot == 0)
    {
        return;
    }

    const size_t uniformBufferAlignment = GVulkanRHI->QueryGPUInfo().uniformBufferAlignment;
    VERIFY_EXPR(uniformBufferAlignment > 0 && uniformBufferAlignment <= UINT32_MAX);

    m_blockSize      = blockSize;
    m_alignment      = uniformBufferAlignment > 0 && uniformBufferAlignment <= UINT32_MAX ?
        static_cast<uint32_t>(uniformBufferAlignment) :
        1;
    m_currentSlotIdx = 0;

    m_slots.resize(numSlots);

    for (Slot& slot : m_slots)
    {
        slot.blocks.reserve(reservedBlocksPerSlot);
    }
}

void VulkanUniformBufferAllocator::Destroy()
{
    for (Slot& slot : m_slots)
    {
        for (Block& block : slot.blocks)
        {
            DestroyBlock(block);
        }

        slot.blocks.clear();
        slot.currentBlockIdx = 0;
    }

    m_slots.clear();
    m_blockSize      = 0;
    m_currentSlotIdx = 0;
    m_alignment      = 1;
}

void VulkanUniformBufferAllocator::BeginFrame(uint32_t frameNum)
{
    if (m_slots.empty())
    {
        return;
    }

    m_currentSlotIdx = frameNum % static_cast<uint32_t>(m_slots.size());

    Slot& slot = m_slots[m_currentSlotIdx];
    ++slot.reuseCount;

    for (uint32_t i = 0; i < slot.blocks.size(); ++i)
    {
        Block& block = slot.blocks[i];
        // Keep recent demand plus one spare. Age each slot only when it is reused.
        if (i <= slot.usedBlocks)
        {
            block.lastNeededReuseCount = slot.reuseCount;
        }
        // Invalidate borrowed cached values before any block can be reset by Alloc.
        // Recorded commands remain protected by pending counts / queue serials.
        block.memory.generation = ++m_nextGeneration;
        block.resetPending      = true;
    }

    while (!slot.blocks.empty())
    {
        Block& block = slot.blocks.back();
        if (slot.reuseCount - block.lastNeededReuseCount < kTrimDelay || !block.CanReuse())
        {
            break;
        }
        DestroyBlock(block);
        slot.blocks.pop_back();
    }

    slot.currentBlockIdx = 0;
    slot.usedBlocks      = 0;
}

bool VulkanUniformBufferAllocator::Block::CanReuse() const
{
    return memory.pBuffer->GetRefCount() == 1 &&
        GVulkanRHI->GetLifetimeTracker().IsComplete(lifetimeId);
}

uint64_t VulkanUniformBufferAllocator::GetBlockGeneration(uint64_t blockId) const
{
    const uint32_t slotIndex  = static_cast<uint32_t>(blockId >> 32);
    const uint32_t blockIndex = static_cast<uint32_t>(blockId) - 1;
    return slotIndex < m_slots.size() && blockIndex < m_slots[slotIndex].blocks.size() ?
        m_slots[slotIndex].blocks[blockIndex].memory.generation : 0;
}

uint64_t VulkanUniformBufferAllocator::GetBlockLifetime(uint64_t blockId) const
{
    const uint32_t slotIndex  = static_cast<uint32_t>(blockId >> 32);
    const uint32_t blockIndex = static_cast<uint32_t>(blockId) - 1;
    return slotIndex < m_slots.size() && blockIndex < m_slots[slotIndex].blocks.size() ?
        m_slots[slotIndex].blocks[blockIndex].lifetimeId :
        0;
}

uint32_t VulkanUniformBufferAllocator::GetAllocatedBlockCount(uint32_t slotIndex) const
{
    return slotIndex < m_slots.size() ? static_cast<uint32_t>(m_slots[slotIndex].blocks.size()) : 0;
}

VulkanUniformBufferBlock VulkanUniformBufferAllocator::Alloc(uint32_t size)
{
    VERIFY_EXPR(size > 0);
    VERIFY_EXPR(!m_slots.empty());
    VERIFY_EXPR(size <= m_blockSize);
    VulkanUniformBufferBlock allocation{};

    if (size > 0 && !m_slots.empty() && size <= m_blockSize)
    {
        Slot& slot = m_slots[m_currentSlotIdx];

        while (true)
        {
            if (slot.currentBlockIdx == slot.blocks.size())
            {
                VulkanUniformBufferBlock block = CreateBlock();

                if (!block.IsValid())
                {
                    break;
                }

                block.blockId = (static_cast<uint64_t>(m_currentSlotIdx) << 32) |
                    (static_cast<uint64_t>(slot.currentBlockIdx) + 1);
                block.generation              = ++m_nextGeneration;
                Block& storage                = slot.blocks.emplace_back();
                storage.memory                = block;
                storage.lifetimeId            = GVulkanRHI->GetLifetimeTracker().Create();
                storage.lastNeededReuseCount  = slot.reuseCount;
            }

            Block& storage = slot.blocks[slot.currentBlockIdx];
            if (storage.resetPending)
            {
                // Unsubmitted and unfinished GPU work can outlive a slot reuse.
                if (!storage.CanReuse())
                {
                    ++slot.currentBlockIdx;
                    continue;
                }
                storage.memory.offset = 0;
                storage.resetPending  = false;
            }
            VulkanUniformBufferBlock& block = storage.memory;
            const uint64_t alignedOffset =
                ((static_cast<uint64_t>(block.offset) + m_alignment - 1) / m_alignment) *
                m_alignment;
            const uint64_t allocationEnd = alignedOffset + size;

            if (allocationEnd <= block.size)
            {
                allocation.pBuffer    = block.pBuffer;
                allocation.offset     = static_cast<uint32_t>(alignedOffset);
                allocation.size       = size;
                allocation.pMapped    = block.pMapped + alignedOffset;
                allocation.blockId    = block.blockId;
                allocation.generation = block.generation;

                block.offset    = static_cast<uint32_t>(allocationEnd);
                slot.usedBlocks = slot.currentBlockIdx + 1;

                break;
            }

            ++slot.currentBlockIdx;
        }
    }

    return allocation;
}

VulkanUniformBufferBlock VulkanUniformBufferAllocator::CreateBlock() const
{
    RHIBufferCreateInfo createInfo{};
    createInfo.size         = m_blockSize;
    createInfo.allocateType = RHIBufferAllocateType::eCPUWrite;
    createInfo.usageFlags.SetFlag(RHIBufferUsageFlagBits::eUniformBuffer);
    createInfo.tag = "VulkanUniformBufferAllocator";

    VulkanUniformBufferBlock block{};
    block.pBuffer = GVulkanRHI->CreateBuffer(createInfo);

    if (block.pBuffer != nullptr)
    {
        block.size    = m_blockSize;
        block.pMapped = block.pBuffer->Map();

        if (block.pMapped == nullptr)
        {
            GVulkanRHI->DestroyBuffer(block.pBuffer);
            block = {};
        }
    }

    return block;
}

void VulkanUniformBufferAllocator::DestroyBlock(Block& block) const
{
    if (block.memory.pBuffer != nullptr)
    {
        GVulkanRHI->GetLifetimeTracker().Retire(
            block.lifetimeId, block.memory.pBuffer, [](void* resource) {
                auto* buffer = static_cast<RHIBuffer*>(resource);
                buffer->Unmap();
                GVulkanRHI->DestroyBuffer(buffer);
            });
    }
    block = {};
}

// BufferHandle VulkanRHI::CreateBuffer(uint32_t size,
//                                      BitField<RHIBufferUsageFlagBits> usageFlags,
//                                      RHIBufferAllocateType allocateType)
// {
//     VulkanBuffer* vulkanBuffer = VersatileResource::Alloc<VulkanBuffer>(m_resourceAllocator);
//
//     VkBufferCreateInfo bufferCI;
//     InitVkStruct(bufferCI, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
//     bufferCI.size        = static_cast<VkDeviceSize>(size);
//     bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
//     bufferCI.usage       = ToVkBufferUsageFlags(usageFlags);
//
//     m_vkMemAllocator->AllocBuffer(size, &bufferCI, allocateType, &vulkanBuffer->buffer,
//                                   &vulkanBuffer->memAlloc);
//     vulkanBuffer->requiredSize  = size;
//     vulkanBuffer->allocatedSize = vulkanBuffer->memAlloc.info.size;
//     return BufferHandle(vulkanBuffer);
// }
//
// uint8_t* VulkanRHI::MapBuffer(BufferHandle bufferHandle)
// {
//     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
//     return m_vkMemAllocator->MapBuffer(vulkanBuffer->memAlloc);
// }
//
// void VulkanRHI::UnmapBuffer(BufferHandle bufferHandle)
// {
//     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
//     m_vkMemAllocator->UnmapBuffer(vulkanBuffer->memAlloc);
// }
//
//
// void VulkanRHI::DestroyBuffer(BufferHandle bufferHandle)
// {
//     VulkanBuffer* vulkanBuffer = TO_VK_BUFFER(bufferHandle);
//     if (vulkanBuffer->bufferView != VK_NULL_HANDLE)
//     {
//         vkDestroyBufferView(m_device->GetVkHandle(), vulkanBuffer->bufferView, nullptr);
//     }
//     m_vkMemAllocator->FreeBuffer(vulkanBuffer->buffer, vulkanBuffer->memAlloc);
//     VersatileResource::Free(m_resourceAllocator, vulkanBuffer);
// }
//
// void VulkanRHI::SetBufferTexelFormat(BufferHandle bufferHandle, DataFormat format)
// {
//     VulkanBuffer* buffer = TO_VK_BUFFER(bufferHandle);
//     VERIFY_EXPR(buffer->bufferView == VK_NULL_HANDLE);
//
//     VkBufferViewCreateInfo bufferViewCI;
//     InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
//     bufferViewCI.buffer = buffer->buffer;
//     bufferViewCI.format = ToVkFormat(format);
//     bufferViewCI.range  = buffer->allocatedSize;
//     bufferViewCI.offset = 0;
//
//     VKCHECK(
//         vkCreateBufferView(m_device->GetVkHandle(), &bufferViewCI, nullptr, &buffer->bufferView));
// }

} // namespace zen
