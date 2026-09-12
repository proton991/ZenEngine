#pragma once
#include "Graphics/RHI/RHIResource.h"
#include "VulkanHeaders.h"
#include "VulkanMemory.h"

namespace zen
{
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
    uint32_t m_allocatedSize{0};
    VkBufferView m_bufferView{VK_NULL_HANDLE};
    VulkanMemoryAllocation m_memAlloc{};
};

// uniform buffer allocator
struct VulkanUniformBufferBlock
{
    RHIBuffer* pBuffer{nullptr};
    uint32_t offset{0};
    uint32_t size{0};
    uint8_t* pMapped{nullptr};

    bool IsValid() const
    {
        return pBuffer != nullptr && pMapped != nullptr;
    }
};

class VulkanUniformBufferAllocator
{
public:
    VulkanUniformBufferAllocator() = default;

    void Init(uint32_t numSlots, uint32_t blockSize, uint32_t maxBlocksPerSlot);

    void Destroy();

    void BeginFrame(uint32_t frameNum);

    VulkanUniformBufferBlock Alloc(uint32_t size);

private:
    struct Slot
    {
        HeapVector<VulkanUniformBufferBlock> blocks;
        uint32_t currentBlockIdx{0};
    };

    VulkanUniformBufferBlock CreateBlock() const;

    HeapVector<Slot> m_slots;

    uint32_t m_blockSize{0};

    uint32_t m_currentSlotIdx{0};

    uint32_t m_maxBlocksPerSlot{0};

    uint32_t m_alignment{256};
};
} // namespace zen