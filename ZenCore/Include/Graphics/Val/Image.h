#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"
#include "Common/SharedPtr.h"

namespace zen::val
{
enum class ImageUsage : uint32_t
{
    Undefined              = 0,
    TransferSrc            = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    TransferDst            = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    Sampled                = VK_IMAGE_USAGE_SAMPLED_BIT,
    Storage                = VK_IMAGE_USAGE_STORAGE_BIT,
    ColorAttachment        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    DepthStencilAttachment = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
    InputAttachment        = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
    MaxEnum                = VK_IMAGE_USAGE_FLAG_BITS_MAX_ENUM,
};

inline ImageUsage operator|(ImageUsage lhs, ImageUsage rhs)
{
    return static_cast<ImageUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}
// Bitwise OR assignment (|=) operator overload as a member function
inline ImageUsage& operator|=(ImageUsage& lhs, ImageUsage rhs)
{
    lhs = static_cast<ImageUsage>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
    return lhs;
}

struct ImageCreateInfo
{
    VkFormat format{};
    VkExtent3D extent3D{};
    ImageUsage usage{};
    VmaAllocationCreateFlags vmaFlags{};
    uint32_t mipLevels{1};
    uint32_t arrayLayers{1};
    VkImageTiling tiling{VK_IMAGE_TILING_OPTIMAL};
    VkImageCreateFlags flags{0};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

class Image : public DeviceObject<VkImage, VK_OBJECT_TYPE_IMAGE>
{
public:
    static SharedPtr<Image> Create(const Device& device, const ImageCreateInfo& info);
    static UniquePtr<Image> CreateUnique(const Device& device, const ImageCreateInfo& info);

    Image(const Device& device,
          VkFormat format,
          VkExtent3D extent3D,
          VkImageUsageFlags usage,
          VmaAllocationCreateFlags vmaFlags,
          uint32_t mipLevels,
          uint32_t arrayLayers,
          VkImageTiling tiling,
          VkImageCreateFlags flags,
          VkSampleCountFlagBits samples);

    Image(const Device& device, VkImage handle, VkExtent3D extent3D, VkFormat format);

    ~Image();

    Image(const Image& other);

    Image(Image&& other) noexcept;

    VkImageView GetView() const
    {
        return m_view;
    }

    const VkExtent3D& GetExtent3D() const
    {
        return m_extent3D;
    };

    VkImageSubresourceRange GetSubResourceRange() const
    {
        return m_subResourceRange;
    }

    static VkImageLayout UsageToImageLayout(ImageUsage usage);
    static VkAccessFlags UsageToAccessFlags(ImageUsage usage);
    static VkPipelineStageFlags UsageToPipelineStage(ImageUsage usage);

    VkImageSubresourceLayers GetSubresourceLayers() const;

private:
    VmaAllocation m_allocation{VK_NULL_HANDLE};
    VkImageView m_view{VK_NULL_HANDLE};
    VkFormat m_format{};
    VkExtent3D m_extent3D{};
    VkImageSubresourceRange m_subResourceRange{};

    static VkImageType GetImageType(VkExtent3D& extent);
};
} // namespace zen::val