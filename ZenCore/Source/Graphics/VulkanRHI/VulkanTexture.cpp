#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
SamplerHandle VulkanRHI::CreateSampler(const SamplerInfo& samplerInfo)
{
    VkSamplerCreateInfo samplerCI;
    InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    samplerCI.magFilter        = ToVkFilter(samplerInfo.magFilter);
    samplerCI.minFilter        = ToVkFilter(samplerInfo.minFilter);
    samplerCI.mipmapMode       = samplerInfo.mipFilter == SamplerFilter::eLinear ?
              VK_SAMPLER_MIPMAP_MODE_LINEAR :
              VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCI.addressModeU     = ToVkSamplerAddressMode(samplerInfo.repeatU);
    samplerCI.addressModeV     = ToVkSamplerAddressMode(samplerInfo.repeatV);
    samplerCI.addressModeW     = ToVkSamplerAddressMode(samplerInfo.repeatW);
    samplerCI.mipLodBias       = samplerInfo.lodBias;
    samplerCI.anisotropyEnable = samplerInfo.useAnisotropy &&
        (m_device->GetPhysicalDeviceFeatures().samplerAnisotropy == VK_TRUE);
    samplerCI.maxAnisotropy           = samplerInfo.maxAnisotropy;
    samplerCI.compareEnable           = samplerInfo.enableCompare;
    samplerCI.compareOp               = ToVkCompareOp(samplerInfo.compareOp);
    samplerCI.minLod                  = samplerInfo.minLod;
    samplerCI.maxLod                  = samplerInfo.maxLod;
    samplerCI.borderColor             = ToVkBorderColor(samplerInfo.borderColor);
    samplerCI.unnormalizedCoordinates = samplerInfo.unnormalizedUVW;

    VkSampler sampler{VK_NULL_HANDLE};
    VKCHECK(vkCreateSampler(m_device->GetVkHandle(), &samplerCI, nullptr, &sampler));

    return SamplerHandle(sampler);
}

void VulkanRHI::DestroySampler(SamplerHandle samplerHandle)
{
    VkSampler sampler = reinterpret_cast<VkSampler>(samplerHandle.value);
    vkDestroySampler(m_device->GetVkHandle(), sampler, nullptr);
}

TextureHandle VulkanRHI::CreateTexture(const TextureInfo& info)
{
    VulkanTexture* texture = VersatileResource::Alloc<VulkanTexture>(m_resourceAllocator);
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
    if (info.type == TextureType::eCube)
    {
        imageCI.imageType = VK_IMAGE_TYPE_2D;
        imageCI.flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    else
    {
        imageCI.imageType = ToVkImageType(info.type);
    }
    imageCI.usage  = ToVkImageUsageFlags(info.usageFlags);
    imageCI.format = ToVkFormat(info.format);

    const auto textureSize = CalculateTextureSize(info);

    m_vkMemAllocator->AllocImage(&imageCI, info.cpuReadable, &texture->image, &texture->memAlloc,
                                 textureSize);

    // create image view
    VkImageViewCreateInfo imageViewCI;
    InitVkStruct(imageViewCI, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    imageViewCI.components.r                = VK_COMPONENT_SWIZZLE_R;
    imageViewCI.components.g                = VK_COMPONENT_SWIZZLE_G;
    imageViewCI.components.b                = VK_COMPONENT_SWIZZLE_B;
    imageViewCI.components.a                = VK_COMPONENT_SWIZZLE_A;
    imageViewCI.viewType                    = ToVkImageViewType(info.type);
    imageViewCI.format                      = imageCI.format;
    imageViewCI.image                       = texture->image;
    imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
    imageViewCI.subresourceRange.levelCount = imageCI.mipLevels;
    if (info.usageFlags.HasFlag(TextureUsageFlagBits::eDepthStencilAttachment))
    {
        imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else
    {
        imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    if (vkCreateImageView(GetVkDevice(), &imageViewCI, nullptr, &texture->imageView) != VK_SUCCESS)
    {
        m_vkMemAllocator->FreeImage(texture->image, texture->memAlloc);
        LOGE("vkCreateImageView failed with error");
    }

    texture->imageCI = imageCI;

    return TextureHandle(texture);
}

DataFormat VulkanRHI::GetTextureFormat(TextureHandle textureHandle)
{
    VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    return static_cast<DataFormat>(texture->imageCI.format);
}

TextureSubResourceRange VulkanRHI::GetTextureSubResourceRange(TextureHandle textureHandle)
{
    TextureSubResourceRange range;
    DataFormat dataFormat = GetTextureFormat(textureHandle);

    if (FormatIsDepthOnly(dataFormat))
    {
        range = TextureSubResourceRange::Depth();
    }
    else if (FormatIsStencilOnly(dataFormat))
    {
        range = TextureSubResourceRange::Stencil();
    }
    else if (FormatIsDepthStencil(dataFormat))
    {
        range = TextureSubResourceRange::DepthStencil();
    }
    else
    {
        range = TextureSubResourceRange::Color();
    }

    VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    range.layerCount       = texture->imageCI.arrayLayers;
    range.levelCount       = texture->imageCI.mipLevels;
    return range;
}

void VulkanRHI::DestroyTexture(TextureHandle textureHandle)
{
    VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(textureHandle.value);
    vkDestroyImageView(m_device->GetVkHandle(), texture->imageView, nullptr);
    m_vkMemAllocator->FreeImage(texture->image, texture->memAlloc);
    VersatileResource::Free(m_resourceAllocator, texture);
}

} // namespace zen::rhi