#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"

namespace zen::val
{
struct BufferCreateInfo
{
    VkDeviceSize             size;
    VkBufferUsageFlags       usage;
    VmaAllocationCreateFlags vmaFlags;
};

class Buffer : public DeviceObject<VkBuffer, VK_OBJECT_TYPE_BUFFER>
{
public:
    static SharedPtr<Buffer> Create(Device& device, const BufferCreateInfo& CI);
    static UniquePtr<Buffer> CreateUnique(Device& device, const BufferCreateInfo& CI);

    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags, std::string debugName);

    ~Buffer();

    static VkAccessFlags        UsageToAccessFlags(VkBufferUsageFlags usage);
    static VkPipelineStageFlags UsageToPipelineStage(VkBufferUsageFlags usage);

    auto GetSize() const { return m_size; }

private:
    VmaAllocation m_allocation{nullptr};
    VkDeviceSize  m_size{0};
};
} // namespace zen::val