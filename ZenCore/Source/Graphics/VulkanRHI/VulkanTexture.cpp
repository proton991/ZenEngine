#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
TextureHandle VulkanRHI::CreateTexture(const TextureInfo& info)
{
    VulkanTexture* texture = m_textureAllocator.Alloc();
    VkImageCreateInfo imageCI;
    InitVkStruct(imageCI, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    imageCI.extent.width  = info.width;
    imageCI.extent.height = info.height;
    imageCI.extent.depth  = info.depth;
    imageCI.samples       = ToVkSampleCountFlagBits(info.samples);
    imageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
    imageCI.arrayLayers   = info.arrayLayers;
    imageCI.mipLevels     = info.mipmaps;
    imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageCI.imageType     = ToVkImageType(info.type);
    imageCI.usage         = ToVkImageUsageFlags(info.usageFlags);
    imageCI.format        = ToVkFormat(info.fomrat);

    const auto textureSize = CalculateTextureSize(info);

    m_vkMemAllocator->CreateImage(&imageCI, info.cpuReadable, &texture->image, &texture->memAlloc,
                                  textureSize);

    // create image view
    VkImageViewCreateInfo imageViewCI;
    InitVkStruct(imageViewCI, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    imageViewCI.components.r                = VK_COMPONENT_SWIZZLE_R;
    imageViewCI.components.g                = VK_COMPONENT_SWIZZLE_G;
    imageViewCI.components.b                = VK_COMPONENT_SWIZZLE_B;
    imageViewCI.components.a                = VK_COMPONENT_SWIZZLE_A;
    imageViewCI.format                      = imageCI.format;
    imageViewCI.image                       = texture->image;
    imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
    imageViewCI.subresourceRange.levelCount = imageCI.mipLevels;
    if (info.usageFlags.HasFlag(TextureUsageFlagBits::eDepthStencilAttachment))
    {
        imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else { imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; }

    if (vkCreateImageView(GetVkDevice(), &imageViewCI, nullptr, &texture->imageView) != VK_SUCCESS)
    {
        m_vkMemAllocator->DestroyImage(texture->image, texture->memAlloc);
        LOGE("vkCreateImageView failed with error");
    }

    texture->imageCI = imageCI;

    return TextureHandle(texture);
}

void VulkanRHI::DestroyTexture(TextureHandle textureHandle)
{
    VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    m_vkMemAllocator->DestroyImage(texture->image, texture->memAlloc);
    m_textureAllocator.Free(texture);
}

} // namespace zen::rhi