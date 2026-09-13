#pragma once
#include "Graphics/RHI/RHIResource.h"
#include "VulkanHeaders.h"
#include "VulkanMemory.h"
#include "Templates/SmallVector.h"

namespace zen
{
class VulkanQueue;

class VulkanBuffer : public RHIBuffer
{
public:
    static VulkanBuffer* CreateObject(const RHIBufferCreateInfo& createInfo);

    uint8_t* Map() override;

    void Unmap() override;

    void SetTexelFormat(DataFormat format) override;

    uint32_t GetOffset() const
    {
        return m_memAlloc.info.offset;
    }

    VkBuffer GetVkBuffer() const
    {
        return m_vkBuffer;
    }

    VkBufferView GetVkBufferView() const
    {
        return m_bufferView;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    explicit VulkanBuffer(const RHIBufferCreateInfo& createInfo) : RHIBuffer(createInfo) {}

    VkBuffer m_vkBuffer{VK_NULL_HANDLE};
    VkBufferView m_bufferView{VK_NULL_HANDLE};
    DataFormat m_texelFormat{DataFormat::eUndefined};
    VulkanMemoryAllocation m_memAlloc{};
};

// uniform buffer allocator
struct VulkanUniformBufferBlock
{
    RHIBuffer* pBuffer{nullptr};
    uint32_t offset{0};
    uint32_t size{0};
    uint8_t* pMapped{nullptr};
    uint64_t blockId{0};
    uint64_t generation{0};

    bool IsValid() const
    {
        return pBuffer != nullptr && pMapped != nullptr;
    }
};

class VulkanUniformBufferAllocator
{
public:
    VulkanUniformBufferAllocator() = default;

    // Reserve block metadata up front; each slot can grow without invalidating earlier allocations.
    void Init(uint32_t numSlots, uint32_t blockSize, uint32_t reservedBlocksPerSlot);

    void Destroy();

    void BeginFrame(uint32_t frameNum);

    VulkanUniformBufferBlock Alloc(uint32_t size);

    static constexpr uint32_t kTrimDelay = 120;

    uint32_t GetAllocatedBlockCount(uint32_t slotIndex) const;

    // Allocation views are borrowed. Native workloads register each referenced block
    // once, then transfer that recording to an accepted queue serial or discard it.
    uint64_t GetBlockGeneration(uint64_t blockId) const;
    void RecordBlock(uint64_t blockId);
    void SubmitBlock(uint64_t blockId, const VulkanQueue* pQueue, uint64_t serial);
    void DiscardBlock(uint64_t blockId);

private:
    struct QueueSerial
    {
        const VulkanQueue* pQueue{nullptr};
        uint64_t serial{0};
    };

    struct Block
    {
        VulkanUniformBufferBlock memory;
        SmallVector<QueueSerial, 3> submissions;
        uint64_t lastNeededReuseSerial{0};
        uint32_t pendingRecordings{0};
        bool resetPending{false};

        bool CanReuse() const;
    };

    struct Slot
    {
        HeapVector<Block> blocks;
        uint32_t currentBlockIdx{0};
        uint32_t usedBlocks{0};
        uint64_t reuseSerial{0};
    };

    VulkanUniformBufferBlock CreateBlock() const;

    void DestroyBlock(VulkanUniformBufferBlock& block) const;

    Block* FindBlock(uint64_t blockId);

    HeapVector<Slot> m_slots;

    uint32_t m_blockSize{0};

    uint32_t m_currentSlotIdx{0};

    uint32_t m_alignment{256};

    // Never reset on Destroy/Init: an old cached location must not match new storage.
    uint64_t m_nextGeneration{0};
};
} // namespace zen
