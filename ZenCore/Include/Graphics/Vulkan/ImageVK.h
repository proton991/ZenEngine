#pragma once
#include "DeviceResource.h"
namespace zen::vulkan {
struct ImageSpec {
  vk::ImageType type;
  vk::Extent3D extent;
  vk::Format format;
  vk::ImageUsageFlags usage;
  vk::SampleCountFlagBits samples{vk::SampleCountFlagBits::e1};
  uint32_t mipLevels{1};
  uint32_t layerCount{1};
  vk::ImageTiling tiling{vk::ImageTiling::eOptimal};
  vk::ImageCreateFlags flags;
  std::vector<uint32_t> queueFamilies;
};
class Image : public DeviceResource<vk::Image> {
public:
  ZEN_MOVE_CONSTRUCTOR_ONLY(Image);
  Image(const Device& device, const ImageSpec& spec);
  ~Image();
  Image(Image&& other) noexcept;

  vk::ImageView GetView() const { return m_view; }

private:
  VmaAllocation m_allocation{nullptr};
  vk::ImageType m_type;
  vk::Extent3D m_extent;
  vk::Format m_format;
  vk::ImageUsageFlags m_usage;
  vk::SampleCountFlagBits m_samples;
  vk::ImageView m_view;
};
}  // namespace zen::vulkan