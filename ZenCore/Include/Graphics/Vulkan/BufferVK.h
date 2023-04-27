#pragma once
#include "DeviceResource.h"

namespace zen::vulkan {
class Buffer : public DeviceResource<vk::Buffer> {
public:
  ZEN_MOVE_CONSTRUCTOR_ONLY(Buffer);
  Buffer(const Device& device, vk::DeviceSize size, vk::BufferUsageFlags usage,
         VmaAllocationCreateFlags vmaFlags, std::string debugName,
         const std::vector<uint32_t>& queueFamilies = {});
  Buffer(Buffer&& other) noexcept;

  ~Buffer();

  uint8_t* Map();
  void Unmap();

  vk::DeviceSize GetSize() const { return m_size; }

  void Update(void* data, size_t size, size_t offset = 0);

  void Flush();

private:
  VmaAllocation m_allocation{nullptr};
  VmaAllocationInfo m_allocInfo{};
  vk::DeviceSize m_size{0};
  uint8_t* m_mappedData{nullptr};
  bool m_mapped{false};
};

}  // namespace zen::vulkan