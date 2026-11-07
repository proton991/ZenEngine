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
namespace
{
bool ValidBufferCreateInfo(const RHIBufferCreateInfo& info)
{
    return info.allocateType > RHIBufferAllocateType::eNone && info.allocateType < RHIBufferAllocateType::eMax && info.size > 0
        && !info.usageFlags.IsEmpty()
        && (!info.usageFlags.HasFlag(RHIBufferUsageFlagBits::eDeviceAddress)
            || GVulkanRHI->QueryGPUInfo().rayQuery.bufferDeviceAddress);
}

template <class Operation>
void WithBufferCreateInfo(uint64_t size, BitField<RHIBufferUsageFlagBits> usage, Operation&& operation)
{
    VkBufferCreateInfo info;
    InitVkStruct(info, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size                  = static_cast<VkDeviceSize>(size);
    info.usage                 = usage;
    const VulkanDevice* device = GVulkanRHI->GetDevice();
    AllocateWithQueueSharing(info, device->GetGfxQueue()->GetFamilyIndex(), device->GetComputeQueue()->GetFamilyIndex(),
                             device->GetTransferQueue()->GetFamilyIndex(),
                             (info.usage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) != 0,
                             [&info, &operation] { operation(info); });
}

uint64_t QueryBufferAllocationSize(const VkBufferCreateInfo& info)
{
    uint64_t       size   = 0;
    VkBuffer       buffer = VK_NULL_HANDLE;
    const VkDevice device = GVulkanRHI->GetVkDevice();
    const VkResult result = vkCreateBuffer(device, &info, nullptr, &buffer);
    if (result == VK_SUCCESS)
    {
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        size = requirements.size;
        vkDestroyBuffer(device, buffer, nullptr);
    }
    else
    {
        ReportVulkanCreationFailure(result, "vkCreateBuffer allocation size query", __FILE__, __LINE__);
        LOGE("Buffer allocation size query failed: {}", GetResultString(result));
    }
    return size;
}
} // namespace

RHIBuffer* VulkanRHI::CreateBuffer(const RHIBufferCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateBuffer(createInfo);
}

uint64_t VulkanRHI::GetBufferAllocationSize(const RHIBufferCreateInfo& createInfo)
{
    uint64_t size = 0;
    if (!AreSubmissionsBlocked() && ValidBufferCreateInfo(createInfo))
    {
        WithBufferCreateInfo(createInfo.size, createInfo.usageFlags,
                             [&size](const VkBufferCreateInfo& info) { size = QueryBufferAllocationSize(info); });
    }
    return size;
}

void VulkanRHI::DestroyBuffer(RHIBuffer* pBuffer)
{
    pBuffer->ReleaseReference();
}

RHIBuffer* VulkanResourceFactory::CreateBuffer(const RHIBufferCreateInfo& createInfo)
{
    RHIBuffer* pBuffer = GVulkanRHI->AreSubmissionsBlocked() ? nullptr : VulkanBuffer::CreateObject(createInfo);

    return pBuffer;
}

VulkanBuffer* VulkanBuffer::CreateObject(const RHIBufferCreateInfo& createInfo)
{
    VulkanBuffer* pBuffer = nullptr;

    if (ValidBufferCreateInfo(createInfo))
    {
        pBuffer = VersatileResource::AllocMem<VulkanBuffer>(GVulkanRHI->GetResourceAllocator());

        new (pBuffer) VulkanBuffer(createInfo);

        pBuffer->Init();

        if (pBuffer->m_vkBuffer == VK_NULL_HANDLE)
        {
            pBuffer->ReleaseReference();

            pBuffer = nullptr;
        }
    }

    if (pBuffer == nullptr)
    {
        LOGE("Buffer creation failed: invalid description or allocation failure");
    }
    return pBuffer;
}

void VulkanBuffer::Init()
{
    WithBufferCreateInfo(m_requiredSize, m_usageFlags, [this](const VkBufferCreateInfo& info) {
        GVkMemAllocator->AllocBuffer(m_requiredSize, &info, m_allocateType, &m_vkBuffer, &m_memAlloc);
    });
    if (m_vkBuffer != VK_NULL_HANDLE && m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eDeviceAddress))
    {
        VkBufferDeviceAddressInfo addressInfo{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        addressInfo.buffer = m_vkBuffer;
        m_deviceAddress    = vkGetBufferDeviceAddress(GVulkanRHI->GetVkDevice(), &addressInfo);
    }

    if (m_vkBuffer != VK_NULL_HANDLE)
    {
        RHIExecutionCounterStorage& counters = GVulkanRHI->GetExecutionCounterStorage();

        counters.Increment(counters.buffersCreated);
    }
}

void VulkanBuffer::Destroy()
{
    if (m_bufferView != VK_NULL_HANDLE)
    {
        vkDestroyBufferView(GVulkanRHI->GetVkDevice(), m_bufferView, nullptr);
    }

    if (m_vkBuffer != VK_NULL_HANDLE || m_memAlloc.handle != VK_NULL_HANDLE)
    {
        if (m_vkBuffer != VK_NULL_HANDLE)
        {
            RHIExecutionCounterStorage& counters = GVulkanRHI->GetExecutionCounterStorage();

            counters.Increment(counters.buffersDestroyed);
        }

        GVkMemAllocator->FreeBuffer(m_vkBuffer, m_memAlloc);
    }

    this->~VulkanBuffer();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

uint8_t* VulkanBuffer::Map()
{
    return static_cast<uint8_t*>(m_memAlloc.info.pMappedData);
}

void VulkanBuffer::Unmap()
{
    // CPU allocations stay mapped until VMA destroys the buffer.
}

bool VulkanBuffer::SetTexelFormat(DataFormat format)
{
    return GetRHIThread().Invoke(&VulkanBuffer::SetTexelFormatOnRHIThread, this, format);
}

bool VulkanBuffer::SetTexelFormatOnRHIThread(DataFormat format)
{
    const bool         uniform   = m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eTextureBuffer);
    const bool         storage   = m_usageFlags.HasFlag(RHIBufferUsageFlagBits::eImageBuffer);
    const uint32_t     texelSize = GetTextureFormatPixelSize(format);
    VkFormatProperties properties{};
    if (texelSize != 0)
    {
        vkGetPhysicalDeviceFormatProperties(GVulkanRHI->GetDevice()->GetPhysicalDeviceHandle(), ToVkFormat(format),
                                            &properties);
    }
    bool ready =
        (uniform || storage) && texelSize != 0 && m_requiredSize % texelSize == 0
        && m_requiredSize / texelSize <= GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxTexelBufferElements
        && (!uniform || (properties.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0)
        && (!storage || (properties.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0);
    if (!ready)
    {
        LOGE("Invalid texel buffer view: usage, format support, range alignment or element limit");
    }
    else if (m_bufferView == VK_NULL_HANDLE)
    {
        VkBufferViewCreateInfo bufferViewCI;

        InitVkStruct(bufferViewCI, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);

        bufferViewCI.buffer = m_vkBuffer;

        bufferViewCI.format = ToVkFormat(format);

        bufferViewCI.range  = m_requiredSize;

        bufferViewCI.offset = 0;

        VkBufferView bufferView{VK_NULL_HANDLE};

        const VkResult result = vkCreateBufferView(GVulkanRHI->GetVkDevice(), &bufferViewCI, nullptr, &bufferView);

        if (result != VK_SUCCESS)
        {
            LOGE("vkCreateBufferView failed: {}", GetResultString(result));

            ready = false;
        }

        // Descriptors can retain this handle from recording through GPU completion.
        // Publish one immutable view only after creation succeeds.
        if (ready)
        {
            m_bufferView  = bufferView;

            m_texelFormat = format;
        }
    }
    else if (format != m_texelFormat)
    {
        LOGE("Cannot change an existing buffer's texel format");

        ready = false;
    }

    return ready;
}

void VulkanUniformBufferAllocator::Init(uint32_t numSlots, uint32_t blockSize, uint32_t reservedBlocksPerSlot)
{
    VERIFY_EXPR(numSlots > 0);

    VERIFY_EXPR(blockSize > 0);

    VERIFY_EXPR(reservedBlocksPerSlot > 0);

    if (((numSlots != 0) && (blockSize != 0)) && (reservedBlocksPerSlot != 0))
    {
        const size_t uniformBufferAlignment = GVulkanRHI->QueryGPUInfo().uniformBufferAlignment;

        VERIFY_EXPR(uniformBufferAlignment > 0 && uniformBufferAlignment <= UINT32_MAX);

        m_blockSize      = blockSize;

        m_alignment      = uniformBufferAlignment > 0 && uniformBufferAlignment <= UINT32_MAX
                             ? static_cast<uint32_t>(uniformBufferAlignment)
                             : 1;

        m_currentSlotIdx = 0;

        m_slots.resize(numSlots);

        for (Slot& slot : m_slots)
        {
            slot.blocks.reserve(reservedBlocksPerSlot);
        }
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
    if (!m_slots.empty())
    {
        m_currentSlotIdx = frameNum % static_cast<uint32_t>(m_slots.size());

        Slot& slot       = m_slots[m_currentSlotIdx];

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
}

bool VulkanUniformBufferAllocator::Block::CanReuse() const
{
    return memory.pBuffer->GetRefCount() == 1 && GVulkanRHI->GetLifetimeTracker().IsComplete(lifetimeId);
}

uint64_t VulkanUniformBufferAllocator::GetBlockGeneration(uint64_t blockId) const
{
    const uint32_t slotIndex  = static_cast<uint32_t>(blockId >> 32);

    const uint32_t blockIndex = static_cast<uint32_t>(blockId) - 1;

    return slotIndex < m_slots.size() && blockIndex < m_slots[slotIndex].blocks.size()
             ? m_slots[slotIndex].blocks[blockIndex].memory.generation
             : 0;
}

uint64_t VulkanUniformBufferAllocator::GetBlockLifetime(uint64_t blockId) const
{
    const uint32_t slotIndex  = static_cast<uint32_t>(blockId >> 32);

    const uint32_t blockIndex = static_cast<uint32_t>(blockId) - 1;

    return slotIndex < m_slots.size() && blockIndex < m_slots[slotIndex].blocks.size()
             ? m_slots[slotIndex].blocks[blockIndex].lifetimeId
             : 0;
}

uint32_t VulkanUniformBufferAllocator::GetAllocatedBlockCount(uint32_t slotIndex) const
{
    return slotIndex < m_slots.size() ? static_cast<uint32_t>(m_slots[slotIndex].blocks.size()) : 0;
}

VulkanUniformBufferBlock VulkanUniformBufferAllocator::Alloc(uint32_t size)
{
    VERIFY_EXPR(size > 0);

    VERIFY_EXPR(!m_slots.empty());

    VulkanUniformBufferBlock allocation{};

    if (size > m_blockSize)
    {
        GVulkanRHI->SetCreationError(
            MakeRHIError(RHIErrorCode::eUnsupported, "Allocate packed uniform buffer", __FILE__, __LINE__));
        LOGE("Packed uniform allocation {} exceeds block capacity {}", size, m_blockSize);
    }

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

                block.blockId =
                    (static_cast<uint64_t>(m_currentSlotIdx) << 32) | (static_cast<uint64_t>(slot.currentBlockIdx) + 1);

                block.generation             = ++m_nextGeneration;

                Block& storage               = slot.blocks.emplace_back();

                storage.memory               = block;

                storage.lifetimeId           = GVulkanRHI->GetLifetimeTracker().Create();

                storage.lastNeededReuseCount = slot.reuseCount;
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
                ((static_cast<uint64_t>(block.offset) + m_alignment - 1) / m_alignment) * m_alignment;

            const uint64_t allocationEnd = alignedOffset + size;

            if (allocationEnd <= block.size)
            {
                allocation.pBuffer    = block.pBuffer;

                allocation.offset     = static_cast<uint32_t>(alignedOffset);

                allocation.size       = size;

                allocation.pMapped    = block.pMapped + alignedOffset;

                allocation.blockId    = block.blockId;

                allocation.generation = block.generation;

                block.offset          = static_cast<uint32_t>(allocationEnd);

                slot.usedBlocks       = slot.currentBlockIdx + 1;

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

    createInfo.allocateType = RHIBufferAllocateType::eCPUWriteGPURead;

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
            GVulkanRHI->SetCreationError(
                MakeVulkanError(VK_ERROR_MEMORY_MAP_FAILED, "Map packed uniform buffer", __FILE__, __LINE__));
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
        GVulkanRHI->GetLifetimeTracker().Retire(block.lifetimeId, block.memory.pBuffer, [](void* resource) {
            RHIBuffer* buffer = static_cast<RHIBuffer*>(resource);

            buffer->Unmap();

            GVulkanRHI->DestroyBuffer(buffer);
        });
    }

    block = {};
}

} // namespace zen
