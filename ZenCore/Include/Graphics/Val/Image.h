#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "DeviceObject.h"
#include "Common/SharedPtr.h"

namespace zen::val
{
struct ImageCreateInfo
{
    VkFormat                 format{};
    VkExtent3D               extent3D{};
    VkImageUsageFlags        usage{};
    VmaAllocationCreateFlags vmaFlags{};
    uint32_t                 mipLevels{1};
    uint32_t                 arrayLayers{1};
    VkImageTiling            tiling{VK_IMAGE_TILING_OPTIMAL};
    VkImageCreateFlags       flags{0};
    VkSampleCountFlagBits    samples{VK_SAMPLE_COUNT_1_BIT};
};

class Image : public DeviceObject<VkImage, VK_OBJECT_TYPE_IMAGE>
{
public:
    static SharedPtr<Image> Create(Device& device, const ImageCreateInfo& info);
    static UniquePtr<Image> CreateUnique(Device& device, const ImageCreateInfo& info);

    Image(Device& device, VkFormat format, VkExtent3D extent3D, VkImageUsageFlags usage, VmaAllocationCreateFlags vmaFlags, uint32_t mipLevels, uint32_t arrayLayers, VkImageTiling tiling, VkImageCreateFlags flags, VkSampleCountFlagBits samples);
    ~Image();

    VkImageView GetView() const { return m_view; }

private:
    VmaAllocation           m_allocation{VK_NULL_HANDLE};
    VkImageView             m_view{VK_NULL_HANDLE};
    VkFormat                m_format;
    VkImageSubresourceRange m_subResourceRange{};

    static VkImageType GetImageType(VkExtent3D& extent);
};
} // namespace zen::val