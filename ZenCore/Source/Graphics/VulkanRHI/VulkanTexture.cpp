#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommandBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommands.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen::rhi
{
// SamplerHandle VulkanRHI::CreateSampler(const SamplerInfo& samplerInfo)
// {
//     VkSamplerCreateInfo samplerCI;
//     InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
//     samplerCI.magFilter        = ToVkFilter(samplerInfo.magFilter);
//     samplerCI.minFilter        = ToVkFilter(samplerInfo.minFilter);
//     samplerCI.mipmapMode       = samplerInfo.mipFilter == SamplerFilter::eLinear ?
//               VK_SAMPLER_MIPMAP_MODE_LINEAR :
//               VK_SAMPLER_MIPMAP_MODE_NEAREST;
//     samplerCI.addressModeU     = ToVkSamplerAddressMode(samplerInfo.repeatU);
//     samplerCI.addressModeV     = ToVkSamplerAddressMode(samplerInfo.repeatV);
//     samplerCI.addressModeW     = ToVkSamplerAddressMode(samplerInfo.repeatW);
//     samplerCI.mipLodBias       = samplerInfo.lodBias;
//     samplerCI.anisotropyEnable = samplerInfo.useAnisotropy &&
//         (m_device->GetPhysicalDeviceFeatures().samplerAnisotropy == VK_TRUE);
//     samplerCI.maxAnisotropy           = samplerInfo.maxAnisotropy;
//     samplerCI.compareEnable           = samplerInfo.enableCompare;
//     samplerCI.compareOp               = ToVkCompareOp(samplerInfo.compareOp);
//     samplerCI.minLod                  = samplerInfo.minLod;
//     samplerCI.maxLod                  = samplerInfo.maxLod;
//     samplerCI.borderColor             = ToVkBorderColor(samplerInfo.borderColor);
//     samplerCI.unnormalizedCoordinates = samplerInfo.unnormalizedUVW;
//
//     VkSampler sampler{VK_NULL_HANDLE};
//     VKCHECK(vkCreateSampler(m_device->GetVkHandle(), &samplerCI, nullptr, &sampler));
//
//     return SamplerHandle(sampler);
// }

RHISampler* VulkanResourceFactory::CreateSampler(const RHISamplerCreateInfo& createInfo)
{
    RHISampler* pSampler = VulkanSampler::CreateObject(createInfo);

    return pSampler;
}

RHISampler* VulkanRHI::CreateSampler(const RHISamplerCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateSampler(createInfo);
}

void VulkanRHI::DestroySampler(RHISampler* sampler)
{
    sampler->ReleaseReference();
}

// RHISampler* RHISampler::Create(const RHISamplerCreateInfo& createInfo)
// {
//     // RHISampler* pSampler = VulkanSampler::CreateObject(createInfo);
//     //
//     // return pSampler;
//     return GVulkanRHI->GetResourceFactory()->CreateSampler(createInfo);
// }

VulkanSampler* VulkanSampler::CreateObject(const RHISamplerCreateInfo& createInfo)
{
    VulkanSampler* pSampler =
        VersatileResource::AllocMem<VulkanSampler>(GVulkanRHI->GetResourceAllocator());

    new (pSampler) VulkanSampler(createInfo);

    pSampler->Init();

    return pSampler;
}

void VulkanSampler::Init()
{
    VkSamplerCreateInfo samplerCI;
    InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    samplerCI.magFilter        = ToVkFilter(m_baseInfo.magFilter);
    samplerCI.minFilter        = ToVkFilter(m_baseInfo.minFilter);
    samplerCI.mipmapMode       = m_baseInfo.mipFilter == SamplerFilter::eLinear ?
              VK_SAMPLER_MIPMAP_MODE_LINEAR :
              VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerCI.addressModeU     = ToVkSamplerAddressMode(m_baseInfo.repeatU);
    samplerCI.addressModeV     = ToVkSamplerAddressMode(m_baseInfo.repeatV);
    samplerCI.addressModeW     = ToVkSamplerAddressMode(m_baseInfo.repeatW);
    samplerCI.mipLodBias       = m_baseInfo.lodBias;
    samplerCI.anisotropyEnable = m_baseInfo.useAnisotropy &&
        (GVulkanRHI->GetDevice()->GetPhysicalDeviceFeatures().samplerAnisotropy == VK_TRUE);
    samplerCI.maxAnisotropy           = m_baseInfo.maxAnisotropy;
    samplerCI.compareEnable           = m_baseInfo.enableCompare;
    samplerCI.compareOp               = ToVkCompareOp(m_baseInfo.compareOp);
    samplerCI.minLod                  = m_baseInfo.minLod;
    samplerCI.maxLod                  = m_baseInfo.maxLod;
    samplerCI.borderColor             = ToVkBorderColor(m_baseInfo.borderColor);
    samplerCI.unnormalizedCoordinates = m_baseInfo.unnormalizedUVW;

    VKCHECK(vkCreateSampler(GVulkanRHI->GetVkDevice(), &samplerCI, nullptr, &m_vkSampler));
}

void VulkanSampler::Destroy()
{
    vkDestroySampler(GVulkanRHI->GetVkDevice(), m_vkSampler, nullptr);
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

RHITexture* VulkanResourceFactory::CreateTexture(const RHITextureCreateInfo& createInfo)
{
    RHITexture* pTexture = VulkanTexture::CreateObject(createInfo);

    return pTexture;
}

RHITexture* VulkanRHI::CreateTexture(const RHITextureCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateTexture(createInfo);
}

void VulkanRHI::DestroyTexture(RHITexture* texture)
{
    texture->ReleaseReference();
}

// RHITexture* RHITexture::Create(const RHITextureCreateInfo& createInfo)
// {
//     // RHITexture* pTexture = VulkanTexture::CreateObject(createInfo);
//     //
//     // return pTexture;
//     return GVulkanRHI->GetResourceFactory()->CreateTexture(createInfo);
// }

void VulkanTexture::CreateImageViewHelper()
{
    VkImageViewCreateInfo imageViewCI;
    InitVkStruct(imageViewCI, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    imageViewCI.components.r                = VK_COMPONENT_SWIZZLE_R;
    imageViewCI.components.g                = VK_COMPONENT_SWIZZLE_G;
    imageViewCI.components.b                = VK_COMPONENT_SWIZZLE_B;
    imageViewCI.components.a                = VK_COMPONENT_SWIZZLE_A;
    imageViewCI.viewType                    = ToVkImageViewType(m_baseInfo.type);
    imageViewCI.format                      = m_vkImageCI.format;
    imageViewCI.image                       = m_vkImage;
    imageViewCI.subresourceRange.layerCount = m_vkImageCI.arrayLayers;
    imageViewCI.subresourceRange.levelCount = m_vkImageCI.mipLevels;
    if (m_baseInfo.usageFlags.HasFlag(TextureUsageFlagBits::eDepthStencilAttachment))
    {
        imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    else
    {
        imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    if (vkCreateImageView(GVulkanRHI->GetVkDevice(), &imageViewCI, nullptr, &m_vkImageView) !=
        VK_SUCCESS)
    {
        GVkMemAllocator->FreeImage(m_vkImage, m_memAlloc);
        LOGE("vkCreateImageView failed with error");
    }
}

VulkanTexture* VulkanTexture::CreateObject(const RHITextureCreateInfo& createInfo)
{
    VulkanTexture* pTexture =
        VersatileResource::AllocMem<VulkanTexture>(GVulkanRHI->GetResourceAllocator());

    new (pTexture) VulkanTexture(createInfo);

    pTexture->Init();

    return pTexture;
}

VulkanTexture* VulkanTexture::CreateProxyObject(const VulkanTexture* pBaseTexture,
                                                const RHITextureProxyCreateInfo& proxyInfo)
{
    VulkanTexture* pProxyTexture =
        VersatileResource::AllocMem<VulkanTexture>(GVulkanRHI->GetResourceAllocator());

    new (pProxyTexture) VulkanTexture(pBaseTexture, proxyInfo);

    pProxyTexture->Init();

    return pProxyTexture;
}

VulkanTexture::VulkanTexture(const VulkanTexture* pBaseTexture,
                             const RHITextureProxyCreateInfo& proxyInfo) :
    RHITexture(dynamic_cast<const RHITexture*>(pBaseTexture), proxyInfo)
{}

void VulkanTexture::Init()
{
    if (m_isProxy)
    {
        InitProxy();
    }
    else
    {
        VkImageCreateInfo imageCI;
        InitVkStruct(imageCI, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        imageCI.extent.width  = m_baseInfo.width;
        imageCI.extent.height = m_baseInfo.height;
        imageCI.extent.depth  = m_baseInfo.depth;
        imageCI.samples       = ToVkSampleCountFlagBits(m_baseInfo.samples);
        imageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
        imageCI.arrayLayers   = m_baseInfo.arrayLayers;
        imageCI.mipLevels     = m_baseInfo.mipmaps;
        imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        if (m_baseInfo.type == TextureType::eCube)
        {
            imageCI.imageType = VK_IMAGE_TYPE_2D;
            imageCI.flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        }
        else
        {
            imageCI.imageType = ToVkImageType(m_baseInfo.type);
        }
        imageCI.usage  = ToVkImageUsageFlags(m_baseInfo.usageFlags);
        imageCI.format = ToVkFormat(m_baseInfo.format);
        if (m_baseInfo.mutableFormat != false)
        {
            imageCI.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        }

        const auto textureSize = CalculateTextureSize(m_baseInfo);

        GVkMemAllocator->AllocImage(&imageCI, m_baseInfo.cpuReadable, &m_vkImage, &m_memAlloc,
                                    textureSize);
        m_vkImageCI = imageCI;

        // create image view
        CreateImageViewHelper();

        if (!m_baseInfo.tag.empty())
        {
            GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE,
                                                   reinterpret_cast<uint64_t>(m_vkImage),
                                                   m_baseInfo.tag.c_str());
        }
        // set layout as undefined when first created
        GVulkanRHI->UpdateImageLayout(m_vkImage, VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // set aspect flags
    m_vkAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

    if (m_vkImageCI.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
    {
        m_vkAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    };
}

void VulkanTexture::Destroy()
{
    vkDestroyImageView(GVulkanRHI->GetVkDevice(), m_vkImageView, nullptr);
    GVulkanRHI->RemoveImageLayout(m_vkImage);
    if (m_isProxy != true)
    {
        GVkMemAllocator->FreeImage(m_vkImage, m_memAlloc);
    }
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

// TextureHandle VulkanRHI::CreateTexture(const TextureInfo& info)
// {
//     VulkanTexture* texture = VersatileResource::Alloc<VulkanTexture>(m_resourceAllocator);
//     VkImageCreateInfo imageCI;
//     InitVkStruct(imageCI, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
//     imageCI.extent.width  = info.width;
//     imageCI.extent.height = info.height;
//     imageCI.extent.depth  = info.depth;
//     imageCI.samples       = ToVkSampleCountFlagBits(info.samples);
//     imageCI.tiling        = VK_IMAGE_TILING_OPTIMAL;
//     imageCI.arrayLayers   = info.arrayLayers;
//     imageCI.mipLevels     = info.mipmaps;
//     imageCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
//     imageCI.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
//     if (info.type == TextureType::eCube)
//     {
//         imageCI.imageType = VK_IMAGE_TYPE_2D;
//         imageCI.flags     = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
//     }
//     else
//     {
//         imageCI.imageType = ToVkImageType(info.type);
//     }
//     imageCI.usage  = ToVkImageUsageFlags(info.usageFlags);
//     imageCI.format = ToVkFormat(info.format);
//     if (info.mutableFormat != false)
//     {
//         imageCI.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
//     }
//
//     const auto textureSize = CalculateTextureSize(info);
//
//     m_vkMemAllocator->AllocImage(&imageCI, info.cpuReadable, &texture->image, &texture->memAlloc,
//                                  textureSize);
//
//     // create image view
//     VkImageViewCreateInfo imageViewCI;
//     InitVkStruct(imageViewCI, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
//     imageViewCI.components.r                = VK_COMPONENT_SWIZZLE_R;
//     imageViewCI.components.g                = VK_COMPONENT_SWIZZLE_G;
//     imageViewCI.components.b                = VK_COMPONENT_SWIZZLE_B;
//     imageViewCI.components.a                = VK_COMPONENT_SWIZZLE_A;
//     imageViewCI.viewType                    = ToVkImageViewType(info.type);
//     imageViewCI.format                      = imageCI.format;
//     imageViewCI.image                       = texture->image;
//     imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
//     imageViewCI.subresourceRange.levelCount = imageCI.mipLevels;
//     if (info.usageFlags.HasFlag(TextureUsageFlagBits::eDepthStencilAttachment))
//     {
//         imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
//     }
//     else
//     {
//         imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
//     }
//
//     if (vkCreateImageView(GetVkDevice(), &imageViewCI, nullptr, &texture->imageView) != VK_SUCCESS)
//     {
//         m_vkMemAllocator->FreeImage(texture->image, texture->memAlloc);
//         LOGE("vkCreateImageView failed with error");
//     }
//
//     texture->imageCI = imageCI;
//
//     if (!info.name.empty())
//     {
//         m_device->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture->image),
//                                 info.name.c_str());
//     }
//     // set layout as undefined when first created
//     m_imageLayoutCache[texture->image] = VK_IMAGE_LAYOUT_UNDEFINED;
//     return TextureHandle(texture);
// }

RHITexture* RHITexture::CreateProxy(const RHITextureProxyCreateInfo& proxyInfo)
{
    RHITexture* pProxyTexture =
        VulkanTexture::CreateProxyObject(dynamic_cast<const VulkanTexture*>(this), proxyInfo);

    return pProxyTexture;
}

void VulkanTexture::InitProxy()
{
    const VulkanTexture* pBaseTexture = dynamic_cast<const VulkanTexture*>(m_pBaseTexture);

    m_vkImage  = pBaseTexture->GetVkImage();
    m_baseInfo = pBaseTexture->GetBaseInfo();
    m_memAlloc = pBaseTexture->GetMemoryAllocation();

    m_vkImageCI = pBaseTexture->GetVkImageCreateInfo();
    // overwrite
    m_vkImageCI.format      = ToVkFormat(m_proxyInfo.format);
    m_vkImageCI.imageType   = ToVkImageType(m_proxyInfo.type);
    m_vkImageCI.arrayLayers = m_proxyInfo.arrayLayers;
    m_vkImageCI.mipLevels   = m_proxyInfo.mipmaps;

    CreateImageViewHelper();

    if (!m_proxyInfo.tag.empty())
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
                                               reinterpret_cast<uint64_t>(m_vkImageView),
                                               m_proxyInfo.tag.c_str());
    }
}

// TextureHandle VulkanRHI::CreateTextureProxy(const TextureHandle& baseTexture,
//                                             const TextureProxyInfo& textureProxyInfo)
// {
//     VulkanTexture* baseTextureVk  = TO_VK_TEXTURE(baseTexture);
//     VulkanTexture* proxyTextureVk = VersatileResource::Alloc<VulkanTexture>(m_resourceAllocator);
//     // copy from base
//     proxyTextureVk->image    = baseTextureVk->image;
//     proxyTextureVk->memAlloc = baseTextureVk->memAlloc;
//     proxyTextureVk->isProxy  = true;
//
//     VkImageCreateInfo imageCI = baseTextureVk->imageCI;
//
//     // overwrite
//     imageCI.format      = ToVkFormat(textureProxyInfo.format);
//     imageCI.imageType   = ToVkImageType(textureProxyInfo.type);
//     imageCI.arrayLayers = textureProxyInfo.arrayLayers;
//     imageCI.mipLevels   = textureProxyInfo.mipmaps;
//
//     // create image view
//     VkImageViewCreateInfo imageViewCI;
//     InitVkStruct(imageViewCI, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
//     imageViewCI.components.r                = VK_COMPONENT_SWIZZLE_R;
//     imageViewCI.components.g                = VK_COMPONENT_SWIZZLE_G;
//     imageViewCI.components.b                = VK_COMPONENT_SWIZZLE_B;
//     imageViewCI.components.a                = VK_COMPONENT_SWIZZLE_A;
//     imageViewCI.viewType                    = ToVkImageViewType(textureProxyInfo.type);
//     imageViewCI.format                      = imageCI.format;
//     imageViewCI.image                       = proxyTextureVk->image;
//     imageViewCI.subresourceRange.layerCount = imageCI.arrayLayers;
//     imageViewCI.subresourceRange.levelCount = imageCI.mipLevels;
//     if (imageCI.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
//     {
//         imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
//     }
//     else
//     {
//         imageViewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
//     }
//
//     if (vkCreateImageView(GetVkDevice(), &imageViewCI, nullptr, &proxyTextureVk->imageView) !=
//         VK_SUCCESS)
//     {
//         LOGE("vkCreateImageView for TextureProxy failed with error");
//     }
//
//     proxyTextureVk->imageCI = imageCI;
//     if (!textureProxyInfo.name.empty())
//     {
//         m_device->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW,
//                                 reinterpret_cast<uint64_t>(proxyTextureVk->imageView),
//                                 textureProxyInfo.name.c_str());
//     }
//     return TextureHandle(proxyTextureVk);
// }

// DataFormat VulkanRHI::GetTextureFormat(TextureHandle textureHandle)
// {
//     VulkanTexture* texture = TO_VK_TEXTURE(textureHandle);
//     return static_cast<DataFormat>(texture->imageCI.format);
// }
//
// TextureSubResourceRange VulkanRHI::GetTextureSubResourceRange(TextureHandle textureHandle)
// {
//     TextureSubResourceRange range;
//     DataFormat dataFormat = GetTextureFormat(textureHandle);
//
//     if (FormatIsDepthOnly(dataFormat))
//     {
//         range = TextureSubResourceRange::Depth();
//     }
//     else if (FormatIsStencilOnly(dataFormat))
//     {
//         range = TextureSubResourceRange::Stencil();
//     }
//     else if (FormatIsDepthStencil(dataFormat))
//     {
//         range = TextureSubResourceRange::DepthStencil();
//     }
//     else
//     {
//         range = TextureSubResourceRange::Color();
//     }
//
//     VulkanTexture* texture = TO_VK_TEXTURE(textureHandle);
//     range.layerCount       = texture->imageCI.arrayLayers;
//     range.levelCount       = texture->imageCI.mipLevels;
//     return range;
// }
//
// void VulkanRHI::DestroyTexture(TextureHandle textureHandle)
// {
//     VulkanTexture* texture = TO_VK_TEXTURE(textureHandle);
//     vkDestroyImageView(m_device->GetVkHandle(), texture->imageView, nullptr);
//     m_imageLayoutCache.erase(texture->image);
//     if (texture->isProxy != true)
//     {
//         m_vkMemAllocator->FreeImage(texture->image, texture->memAlloc);
//     }
//     VersatileResource::Free(m_resourceAllocator, texture);
// }

void VulkanRHI::UpdateImageLayout(VkImage image, VkImageLayout newLayout)
{
    // if (m_imageLayoutCache.contains(image))
    // {
    m_imageLayoutCache[image] = newLayout;
    // }
}

void VulkanRHI::RemoveImageLayout(VkImage image)
{
    if (m_imageLayoutCache.contains(image))
    {
        m_imageLayoutCache.erase(image);
    }
}


VkImageLayout VulkanRHI::GetImageCurrentLayout(VkImage image)
{
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_imageLayoutCache.contains(image))
    {
        layout = m_imageLayoutCache[image];
    }
    return layout;
}

} // namespace zen::rhi