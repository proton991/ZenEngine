#include "Graphics/Val/Image.h"
#include "Common/Errors.h"

namespace zen::val
{
SharedPtr<Image> Image::Create(const Device& device, const ImageCreateInfo& CI)
{
    return MakeShared<Image>(device, CI.format, CI.extent3D, (VkImageUsageFlags)CI.usage,
                             CI.vmaFlags, CI.mipLevels, CI.arrayLayers, CI.tiling, CI.flags,
                             CI.samples);
}

UniquePtr<Image> Image::CreateUnique(const Device& device, const ImageCreateInfo& CI)
{
    return MakeUnique<Image>(device, CI.format, CI.extent3D, (VkImageUsageFlags)CI.usage,
                             CI.vmaFlags, CI.mipLevels, CI.arrayLayers, CI.tiling, CI.flags,
                             CI.samples);
}

Image::Image(const Device& device,
             VkFormat format,
             VkExtent3D extent3D,
             VkImageUsageFlags usage,
             VmaAllocationCreateFlags vmaFlags,
             uint32_t mipLevels,
             uint32_t arrayLayers,
             VkImageTiling tiling,
             VkImageCreateFlags flags,
             VkSampleCountFlagBits samples) :
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

    CHECK_VK_ERROR_AND_THROW(vmaCreateImage(m_device.GetAllocator(), &imageCI, &vmaCI, &m_handle,
                                            &m_allocation, nullptr),
                             "vmaCreateImage failed");

    VkImageViewCreateInfo viewCI{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewCI.flags            = 0;
    viewCI.pNext            = nullptr;
    viewCI.format           = format;
    viewCI.image            = m_handle;
    viewCI.subresourceRange = m_subResourceRange;
    viewCI.viewType         = VK_IMAGE_VIEW_TYPE_2D;

    CHECK_VK_ERROR_AND_THROW(vkCreateImageView(m_device.GetHandle(), &viewCI, nullptr, &m_view),
                             "failed create image view");
}

static inline bool FormatHasDepthStencilAspect(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_S8_UINT: return true;

        default: return false;
    }
}

VkImageType Image::GetImageType(VkExtent3D& extent)
{
    VkImageType result{};
    uint32_t dimensions{0};

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
        case 1: result = VK_IMAGE_TYPE_1D; break;
        case 2: result = VK_IMAGE_TYPE_2D; break;
        case 3: result = VK_IMAGE_TYPE_3D; break;
        default: LOG_ERROR_AND_THROW("No image type found."); break;
    }

    return result;
}

Image::Image(const Device& device, VkImage handle, VkExtent3D extent3D, VkFormat format) :
    DeviceObject(device), m_extent3D(extent3D), m_format(format)
{
    m_handle = handle;
    if (FormatHasDepthStencilAspect(m_format))
    {
        m_subResourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    else
    {
        m_subResourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }
    m_subResourceRange.layerCount     = 1;
    m_subResourceRange.baseArrayLayer = 0;
    m_subResourceRange.levelCount     = 1;
    m_subResourceRange.baseMipLevel   = 0;
}

Image::Image(Image&& other) noexcept : DeviceObject(std::move(other))
{
    m_subResourceRange = std::move(other.m_subResourceRange);
    m_allocation       = std::move(other.m_allocation);
    m_format           = std::move(other.m_format);
    m_view             = std::move(other.m_view);
    m_allocation       = std::move(other.m_allocation);
}

Image::~Image()
{
    if (m_view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(m_device.GetHandle(), m_view, nullptr);
        m_view = VK_NULL_HANDLE;
    }

    if (m_allocation != VK_NULL_HANDLE)
    {
        vmaDestroyImage(m_device.GetAllocator(), m_handle, m_allocation);
        m_allocation = VK_NULL_HANDLE;
    }
}

Image::Image(const Image& other) : DeviceObject(other)
{
    m_subResourceRange = other.m_subResourceRange;
    m_allocation       = other.m_allocation;
    m_format           = other.m_format;
    m_view             = other.m_view;
    m_allocation       = other.m_allocation;
}

VkImageSubresourceLayers Image::GetSubresourceLayers() const
{
    VkImageSubresourceLayers subresourceLayers{};
    subresourceLayers.aspectMask     = m_subResourceRange.aspectMask;
    subresourceLayers.baseArrayLayer = m_subResourceRange.baseArrayLayer;
    subresourceLayers.layerCount     = m_subResourceRange.layerCount;
    subresourceLayers.mipLevel       = m_subResourceRange.baseMipLevel;

    return subresourceLayers;
}

VkImageLayout Image::UsageToImageLayout(ImageUsage usage)
{
    switch (usage)
    {
        case ImageUsage::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
        case ImageUsage::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ImageUsage::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ImageUsage::Sampled:
        case ImageUsage::InputAttachment: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ImageUsage::Storage: return VK_IMAGE_LAYOUT_GENERAL;
        case ImageUsage::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        case ImageUsage::DepthStencilAttachment:
            return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        default: break;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkAccessFlags Image::UsageToAccessFlags(ImageUsage usage)
{
    switch (usage)
    {
        case ImageUsage::Undefined: return 0;
        case ImageUsage::TransferSrc: return VK_ACCESS_TRANSFER_READ_BIT;
        case ImageUsage::TransferDst: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case ImageUsage::Sampled: return VK_ACCESS_SHADER_READ_BIT;
        case ImageUsage::InputAttachment: return VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        case ImageUsage::Storage: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT; ;
        case ImageUsage::ColorAttachment: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case ImageUsage::DepthStencilAttachment:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        default: break;
    }
    return 0;
}

VkPipelineStageFlags Image::UsageToPipelineStage(ImageUsage usage)
{
    switch (usage)
    {
        case ImageUsage::Undefined: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

        case ImageUsage::TransferSrc:
        case ImageUsage::TransferDst: return VK_PIPELINE_STAGE_TRANSFER_BIT;

        case ImageUsage::Sampled:
        case ImageUsage::InputAttachment:
        case ImageUsage::Storage: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

        case ImageUsage::ColorAttachment: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        case ImageUsage::DepthStencilAttachment:
            return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        default: break;
    }
    return 0;
}
} // namespace zen::val