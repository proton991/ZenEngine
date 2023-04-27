#include "Graphics/Vulkan/BufferVK.h"
#include "Common/Error.h"

namespace zen::vulkan {

Buffer::Buffer(const Device& device, vk::DeviceSize size, vk::BufferUsageFlags usage,
               VmaAllocationCreateFlags vmaFlags, std::string debugName,
               const std::vector<uint32_t>& queueFamilies /*= {}*/)
    : DeviceResource(device, nullptr), m_size(size) {

  auto bufferCI = vk::BufferCreateInfo().setUsage(usage).setSize(size);
  if (queueFamilies.size() >= 2) {
    bufferCI.setQueueFamilyIndices(queueFamilies);
    bufferCI.setSharingMode(vk::SharingMode::eConcurrent);
  } else {
    bufferCI.setSharingMode(vk::SharingMode::eExclusive);
  }

  VmaAllocationCreateInfo vmaAllocCI{};
  vmaAllocCI.usage = VMA_MEMORY_USAGE_AUTO;
  vmaAllocCI.flags = vmaFlags;

  VK_CHECK(vmaCreateBuffer(GetVmaAllocator(), reinterpret_cast<VkBufferCreateInfo*>(&bufferCI),
                           &vmaAllocCI, reinterpret_cast<VkBuffer*>(&GetHandle()), &m_allocation,
                           &m_allocInfo),
           "vma create buffer");
}

Buffer::Buffer(Buffer&& other) noexcept
    : DeviceResource(std::move(other)),
      m_allocation(std::exchange(other.m_allocation, {})),
      m_size(std::exchange(other.m_size, {})),
      m_mappedData(std::exchange(other.m_mappedData, {})),
      m_mapped(std::exchange(other.m_mapped, {})) {}

Buffer::~Buffer() {
  if (GetHandle() && m_allocation != nullptr) {
    Unmap();
    vmaDestroyBuffer(GetVmaAllocator(), GetHandle(), m_allocation);
  }
}

uint8_t* Buffer::Map() {
  if (!m_mapped && !m_mappedData) {
    VK_CHECK(vmaMapMemory(GetVmaAllocator(), m_allocation, reinterpret_cast<void**>(&m_mappedData)),
             "vma map memory");
    m_mapped = true;
  }
  return m_mappedData;
}

void Buffer::Unmap() {
  if (m_mapped) {
    vmaUnmapMemory(GetVmaAllocator(), m_allocation);
    m_mapped     = false;
    m_mappedData = nullptr;
  }
}

void Buffer::Update(void* data, size_t size, size_t offset) {
  Map();
  std::memcpy(m_mappedData + offset, data, size);
  Flush();
  Unmap();
}

void Buffer::Flush() {
  vmaFlushAllocation(GetVmaAllocator(), m_allocation, 0, m_size);
}

}  // namespace zen::vulkan