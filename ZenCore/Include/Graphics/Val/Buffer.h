#pragma once
#include <string>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace zen::val
{
class Device;

class Buffer
{
public:
    Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags, const std::string& debugName);

    ~Buffer();

    VkBuffer GetHandle() const { return m_handle; }

private:
    Device&       m_device;
    VkBuffer      m_handle{VK_NULL_HANDLE};
    VmaAllocation m_allocation{nullptr};
    VkDeviceSize  m_size{0};
};
} // namespace zen::val