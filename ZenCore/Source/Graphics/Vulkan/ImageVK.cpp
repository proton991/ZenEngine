#include "Graphics/Vulkan/ImageVK.h"
#include "Common/Error.h"

namespace zen::vulkan {

Image::Image(const Device& device, const ImageSpec& spec)
    : DeviceResource(device, nullptr),
      m_type(spec.type),
      m_extent(spec.extent),
      m_format(spec.format),
      m_usage(spec.usage),
      m_samples(spec.samples) {
  VK_ASSERT(spec.mipLevels > 1 && "Vulkan Image should have at least one level");
  VK_ASSERT(spec.layerCount > 1 && "Vulkan Image should have at least one layer");
  vk::ImageSubresourceRange subResourceRange{};
  if (spec.usage & vk::ImageUsageFlagBits::eColorAttachment) {
    subResourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
  }
  if (spec.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
    subResourceRange.aspectMask =
        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
  }
  auto imageCI = vk::ImageCreateInfo()
                     .setImageType(m_type)
                     .setExtent(m_extent)
                     .setFormat(m_format)
                     .setUsage(m_usage)
                     .setMipLevels(spec.mipLevels)
                     .setArrayLayers(spec.layerCount)
                     .setSamples(m_samples)
                     .setTiling(spec.tiling)
                     .setFlags(spec.flags)
                     .setInitialLayout(vk::ImageLayout::eUndefined);
  if (!spec.queueFamilies.empty()) {
    imageCI.setSharingMode(vk::SharingMode::eConcurrent);
    imageCI.setQueueFamilyIndices(spec.queueFamilies);
  }
  VmaAllocationCreateInfo vmaAllocCI{};
  vmaAllocCI.usage    = VMA_MEMORY_USAGE_AUTO;
  vmaAllocCI.flags    = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  vmaAllocCI.priority = 1.0f;
  VK_CHECK(vmaCreateImage(GetVmaAllocator(), reinterpret_cast<VkImageCreateInfo const*>(&imageCI),
                          &vmaAllocCI,
                          const_cast<VkImage*>(reinterpret_cast<VkImage const*>(&GetHandle())),
                          &m_allocation, nullptr),
           "vmaCreateImage");
  // create image view
  auto imageViewCI =
      vk::ImageViewCreateInfo()
          .setImage(GetHandle())
          .setViewType(spec.layerCount == 1 ? vk::ImageViewType::e2D : vk::ImageViewType::e2DArray)
          .setFormat(m_format)
          .setSubresourceRange(subResourceRange);
  m_view = GetDeviceHandle().createImageView(imageViewCI);
}

Image::Image(Image&& other) noexcept
    : DeviceResource(std::move(other)),
      m_allocation(std::exchange(other.m_allocation, {})),
      m_type(std::exchange(other.m_type, {})),
      m_extent(std::exchange(other.m_extent, {})),
      m_format(std::exchange(other.m_format, {})),
      m_usage(std::exchange(other.m_usage, {})),
      m_samples(std::exchange(other.m_samples, {})),
      m_view(std::exchange(other.m_view, {})) {}

Image::~Image() {
  if (GetHandle() && m_allocation) {
    GetDeviceHandle().destroyImageView(m_view);
    vmaDestroyImage(GetVmaAllocator(), GetCHandle(), m_allocation);
  }
}

}  // namespace zen::vulkan