#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace zen::val
{
class Device;

class Image
{
public:
    Image(Device& device, VkFormat format, VkExtent3D extent3D, VkImageUsageFlags usage, VmaAllocationCreateFlags vmaFlags, uint32_t mipLevels = 1, uint32_t arrayLayers = 1, VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL, VkImageCreateFlags flags = 0);

    VkImage GetHandle() const { return m_handle; }

    VkImageView GetView() const { return m_view; }

private:
    Device&                 m_device;
    VkImage                 m_handle{VK_NULL_HANDLE};
    VmaAllocation           m_allocation{VK_NULL_HANDLE};
    VkImageView             m_view{VK_NULL_HANDLE};
    VkFormat                m_format;
    VkImageSubresourceRange m_subResourceRange{};

    static VkImageType GetImageType(VkExtent3D& extent);
};
} // namespace zen::val