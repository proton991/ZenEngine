#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"

namespace zen::val
{
class Image : public DeviceObject<VkImage, VK_OBJECT_TYPE_IMAGE>
{
public:
    Image(Device& device, VkFormat format, VkExtent3D extent3D, VkImageUsageFlags usage, VmaAllocationCreateFlags vmaFlags, uint32_t mipLevels = 1, uint32_t arrayLayers = 1, VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL, VkImageCreateFlags flags = 0);

    VkImageView GetView() const { return m_view; }

private:
    VmaAllocation           m_allocation{VK_NULL_HANDLE};
    VkImageView             m_view{VK_NULL_HANDLE};
    VkFormat                m_format;
    VkImageSubresourceRange m_subResourceRange{};

    static VkImageType GetImageType(VkExtent3D& extent);
};
} // namespace zen::val