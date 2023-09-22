#include "Graphics/Val/Image.h"
#include "Common/Errors.h"

namespace zen::val
{
SharedPtr<Image> Image::Create(Device& device, const ImageCreateInfo& CI)
{
    return MakeShared<Image>(device, CI.format, CI.extent3D, CI.usage, CI.vmaFlags, CI.mipLevels, CI.arrayLayers, CI.tiling, CI.flags, CI.samples);
}

UniquePtr<Image> Image::CreateUnique(Device& device, const ImageCreateInfo& CI)
{
    return MakeUnique<Image>(device, CI.format, CI.extent3D, CI.usage, CI.vmaFlags, CI.mipLevels, CI.arrayLayers, CI.tiling, CI.flags, CI.samples);
}

Image::Image(Device& device, VkFormat format, VkExtent3D extent3D, VkImageUsageFlags usage, VmaAllocationCreateFlags vmaFlags, uint32_t mipLevels, uint32_t arrayLayers, VkImageTiling tiling, VkImageCreateFlags flags, VkSampleCountFlagBits samples) :
    DeviceObject{device}, m_format{format}, m_extent3D(extent3D)
{
    if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        m_subResourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    else
    {
        m_subResourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    m_subResourceRange.layerCount     = arrayLayers;
    m_subResourceRange.baseArrayLayer = 0;
    m_subResourceRange.levelCount     = mipLevels;
    m_subResourceRange.baseMipLevel   = 0;

    VkImageCreateInfo imageCI{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageCI.format      = format;
    imageCI.mipLevels   = mipLevels;
    imageCI.arrayLayers = arrayLayers;
    imageCI.usage       = usage;
    imageCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.extent      = extent3D;
    imageCI.imageType   = GetImageType(extent3D);
    imageCI.flags       = flags;
    imageCI.tiling      = tiling;
    imageCI.samples     = samples;

    VmaAllocationCreateInfo vmaCI{};
    vmaCI.usage = VMA_MEMORY_USAGE_AUTO;
    vmaCI.flags = vmaFlags;

    CHECK_VK_ERROR_AND_THROW(vmaCreateImage(m_device.GetAllocator(), &imageCI, &vmaCI, &m_handle, &m_allocation, nullptr), "vmaCreateImage failed");

    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.flags            = 0;
    viewCI.pNext            = nullptr;
    viewCI.format           = format;
    viewCI.image            = m_handle;
    viewCI.subresourceRange = m_subResourceRange;
    viewCI.viewType         = VK_IMAGE_VIEW_TYPE_2D;

    CHECK_VK_ERROR_AND_THROW(vkCreateImageView(m_device.GetHandle(), &viewCI, nullptr, &m_view), "failed create image view");
}

VkImageType Image::GetImageType(VkExtent3D& extent)
{
    VkImageType result{};
    uint32_t    dimensions{0};

    if (extent.width >= 1)
    {
        dimensions++;
    }
    if (extent.height >= 1)
    {
        dimensions++;
    }
    if (extent.depth > 1)
    {
        dimensions++;
    }
    switch (dimensions)
    {
        case 1:
            result = VK_IMAGE_TYPE_1D;
            break;
        case 2:
            result = VK_IMAGE_TYPE_2D;
            break;
        case 3:
            result = VK_IMAGE_TYPE_3D;
            break;
        default:
            LOG_ERROR_AND_THROW("No image type found.");
            break;
    }

    return result;
}

Image::~Image()
{
    if (m_view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device.GetHandle(), m_view, nullptr);
    }
    if (m_allocation != VK_NULL_HANDLE)
    {
        vmaDestroyImage(m_device.GetAllocator(), m_handle, m_allocation);
    }
}
} // namespace zen::val