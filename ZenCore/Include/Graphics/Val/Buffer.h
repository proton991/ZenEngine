#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"

namespace zen::val
{
class Buffer : public DeviceObject<VkBuffer, VK_OBJECT_TYPE_BUFFER>
{
public:
    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags, std::string debugName);

    ~Buffer();
    
private:
    VmaAllocation m_allocation{nullptr};
    VkDeviceSize  m_size{0};
};
} // namespace zen::val