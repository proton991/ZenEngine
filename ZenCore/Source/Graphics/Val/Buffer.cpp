#include "Graphics/Val/Buffer.h"
#include "Common/Errors.h"
#include "Graphics/Val/VulkanDebug.h"

namespace zen::val
{
Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags vmaFlags, std::string debugName) :
    DeviceObject(device, debugName), m_size(size)
{
    VkBufferCreateInfo bufferCI{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferCI.usage       = usage;
    bufferCI.size        = size;
    bufferCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;


    VmaAllocationCreateInfo vmaAllocCI{};
    vmaAllocCI.usage = VMA_MEMORY_USAGE_AUTO;
    vmaAllocCI.flags = vmaFlags;

    CHECK_VK_ERROR_AND_THROW(vmaCreateBuffer(m_device.GetAllocator(), &bufferCI,
                                             &vmaAllocCI, &m_handle, &m_allocation,
                                             nullptr),
                             "vmaCreateBuffer failed");
    SetBufferName(m_device.GetHandle(), m_handle, debugName.data());
}

Buffer::~Buffer()
{
    if (m_handle != VK_NULL_HANDLE && m_allocation != nullptr)
    {
        vmaDestroyBuffer(m_device.GetAllocator(), m_handle, m_allocation);
    }
}
} // namespace zen::val