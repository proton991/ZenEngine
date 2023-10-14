#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"

namespace zen::val
{
struct BufferCreateInfo
{
    VkDeviceSize             byteSize;
    VkBufferUsageFlags       usage;
    VmaAllocationCreateFlags vmaFlags;
};

class Buffer : public DeviceObject<VkBuffer, VK_OBJECT_TYPE_BUFFER>
{
public:
    static SharedPtr<Buffer> Create(const Device& device, const BufferCreateInfo& CI);
    static UniquePtr<Buffer> CreateUnique(const Device& device, const BufferCreateInfo& CI);

    Buffer(const Device&            device,
           VkDeviceSize             byteSize,
           VkBufferUsageFlags       usage,
           VmaAllocationCreateFlags vmaFlags);

    ~Buffer();

    static VkAccessFlags        UsageToAccessFlags(VkBufferUsageFlags usage);
    static VkPipelineStageFlags UsageToPipelineStage(VkBufferUsageFlags usage);

    bool IsMemoryMapped() const;

    uint8_t* MapMemory();

    void UnmapMemory();

    void FlushMemory(size_t byteSize, size_t offset);

    void CopyData(const uint8_t* data, size_t byteSize, size_t offset);

    auto GetSize() const { return m_byteSize; }

private:
    VmaAllocation m_allocation{nullptr};
    VkDeviceSize  m_byteSize{0};
    uint8_t*      m_mappedMemory{nullptr};
};
} // namespace zen::val