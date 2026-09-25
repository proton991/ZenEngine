#include "Graphics/VulkanRHI/VulkanResourceSharing.h"
#include "Graphics/VulkanRHI/VulkanTexture.h"
#include "Graphics/VulkanRHI/VulkanCommon.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanMemory.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Graphics/VulkanRHI/VulkanResourceAllocator.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"

namespace zen
{
static uint32_t CalculateTextureSize(const RHITextureCreateInfo& info)
{
    // TODO: Support compressed texture format
    const uint32_t pixelSize = GetTextureFormatPixelSize(info.format);

    uint32_t w = info.width;
    uint32_t h = info.height;
    uint32_t d = info.depth;

    uint32_t size = 0;

    for (uint32_t i = 0; i < info.mipmaps; i++)
    {
        size += w * h * d * pixelSize;
        w >>= 1;
        h >>= 1;
        d >>= 1;
    }

    return size * info.arrayLayers;
}

// SamplerHandle VulkanRHI::CreateSampler(const RHISamplerInfo& samplerInfo)
// {
//     VkSamplerCreateInfo samplerCI;
//     InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
//     samplerCI.magFilter        = ToVkFilter(samplerInfo.magFilter);
//     samplerCI.minFilter        = ToVkFilter(samplerInfo.minFilter);
//     samplerCI.mipmapMode       = samplerInfo.mipFilter == RHISamplerFilter::eLinear ?
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

void VulkanRHI::DestroySampler(RHISampler* pSampler)
{
    pSampler->ReleaseReference();
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
    samplerCI.mipmapMode       = m_baseInfo.mipFilter == RHISamplerFilter::eLinear ?
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
    this->~VulkanSampler();
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

RHITextureView* VulkanRHI::CreateTextureView(RHITexture* pBaseTexture,
                                             const RHITextureViewCreateInfo& createInfo)
{
    RHITextureView* view = nullptr;
    if (pBaseTexture == nullptr)
    {
        LOGE("Cannot create a view of a null texture");
    }
    else
    {
        view = pBaseTexture->CreateView(createInfo);
    }
    return view;
}

void VulkanRHI::DestroyTexture(RHITexture* pTexture)
{
    pTexture->ReleaseReference();
}

VulkanTexture* VulkanTexture::CreateObject(const RHITextureCreateInfo& createInfo)
{
    VulkanTexture* pTexture =
        VersatileResource::AllocMem<VulkanTexture>(GVulkanRHI->GetResourceAllocator());

    new (pTexture) VulkanTexture(createInfo);

    pTexture->Init();
    const bool needsAttachmentView = createInfo.type != RHITextureType::e3D &&
        (createInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eColorAttachment) ||
         createInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eDepthStencilAttachment));
    if (pTexture->GetDefaultView() == nullptr ||
        (needsAttachmentView && pTexture->GetAttachmentView() == nullptr))
    {
        pTexture->ReleaseReference();
        pTexture = nullptr;
    }

    return pTexture;
}

RHITextureView* VulkanTexture::CreateView(const RHITextureViewCreateInfo& createInfo)
{
    return GetRHIThread().Invoke(&VulkanTexture::CreateViewOnRHIThread, this,
                                 std::cref(createInfo));
}

RHITextureView* VulkanTexture::CreateViewOnRHIThread(const RHITextureViewCreateInfo& createInfo)
{
    const char* error = nullptr;
    if (createInfo.format == DataFormat::eUndefined ||
        (createInfo.format != m_baseInfo.format && !m_baseInfo.mutableFormat))
    {
        error = "Texture view format requires a compatible mutable-format image";
    }
    else if (createInfo.mipLevels == 0 || createInfo.baseMipLevel >= m_baseInfo.mipmaps ||
             createInfo.mipLevels > m_baseInfo.mipmaps - createInfo.baseMipLevel ||
             createInfo.arrayLayers == 0 || createInfo.baseArrayLayer >= m_baseInfo.arrayLayers ||
             createInfo.arrayLayers > m_baseInfo.arrayLayers - createInfo.baseArrayLayer)
    {
        error = "Texture view mip or array-layer range is outside its image";
    }
    else if (createInfo.type != m_baseInfo.type &&
             !(m_baseInfo.type == RHITextureType::eCube && createInfo.type == RHITextureType::e2D))
    {
        error = "Texture view type is incompatible with its image";
    }
    else if (createInfo.type == RHITextureType::eCube && createInfo.arrayLayers % 6 != 0)
    {
        error = "Cube views require a multiple of six array layers";
    }
    else if ((int64_t(createInfo.aspect) & ~int64_t(GetTextureFormatAspects(createInfo.format))) !=
             0)
    {
        error = "Texture view aspects are incompatible with its format";
    }

    VulkanTextureView* pView = nullptr;
    if (error != nullptr)
    {
        LOGE("{}", error);
    }
    else
    {
        pView = VulkanTextureView::CreateObject(this, createInfo);
        if (pView != nullptr)
        {
            RegisterOwnedView(pView);
        }
    }
    return pView;
}

RHITextureView* VulkanTexture::GetAttachmentView()
{
    if (m_baseInfo.type == RHITextureType::e3D)
    {
        LOG_ERROR_AND_THROW("Rendering to 3D texture slices is not supported");
    }
    if (m_pAttachmentView == nullptr)
    {
        if (m_baseInfo.mipmaps == 1 && m_baseInfo.type != RHITextureType::eCube)
        {
            m_pAttachmentView = m_pDefaultView;
        }
        else
        {
            RHITextureViewCreateInfo info{};
            info.format = m_baseInfo.format;
            info.type =
                m_baseInfo.type == RHITextureType::eCube ? RHITextureType::e2D : m_baseInfo.type;
            info.arrayLayers  = m_baseInfo.arrayLayers;
            m_pAttachmentView = CreateView(info);
        }
    }
    return m_pAttachmentView;
}

VkImageView VulkanTexture::GetVkImageView() const
{
    VERIFY_EXPR(m_pDefaultView != nullptr);
    const VulkanTextureView* pDefaultView = TO_VK_TEXTURE_VIEW(m_pDefaultView);
    const VkImageView imageView =
        pDefaultView != nullptr ? pDefaultView->GetVkImageView() : VK_NULL_HANDLE;

    return imageView;
}

void VulkanTexture::Init()
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

    if (m_baseInfo.type == RHITextureType::eCube)
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

    const uint32_t graphicsQueueFamily = GVulkanRHI->GetDevice()->GetGfxQueue()->GetFamilyIndex();
    const uint32_t computeQueueFamily =
        GVulkanRHI->GetDevice()->GetComputeQueue()->GetFamilyIndex();
    const uint32_t transferQueueFamily =
        GVulkanRHI->GetDevice()->GetTransferQueue()->GetFamilyIndex();

    const uint32_t textureSize = CalculateTextureSize(m_baseInfo);
    bool allocated             = false;

    AllocateWithQueueSharing(
        imageCI, graphicsQueueFamily, computeQueueFamily, transferQueueFamily,
        (imageCI.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != 0,
        [this, &imageCI, textureSize, &allocated] {
            allocated = GVkMemAllocator->AllocImage(&imageCI, m_baseInfo.cpuReadable, &m_vkImage,
                                                    &m_memAlloc, textureSize);
        });
    if (allocated)
    {
        m_vkImageCI = imageCI;

        if (!m_baseInfo.tag.IsNone())
        {
            GVulkanRHI->GetDevice()->SetObjectName(
                VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_vkImage), m_baseInfo.tag);
        }

        // Only successfully allocated images enter layout tracking or acquire views.
        GVulkanRHI->UpdateImageLayout(m_vkImage, VK_IMAGE_LAYOUT_UNDEFINED);
        m_vkAspectFlags = VK_IMAGE_ASPECT_COLOR_BIT;

        if (FormatIsDepthStencil(m_baseInfo.format))
        {
            m_vkAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        else if (FormatIsDepthOnly(m_baseInfo.format))
        {
            m_vkAspectFlags = VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        else if (FormatIsStencilOnly(m_baseInfo.format))
        {
            m_vkAspectFlags = VK_IMAGE_ASPECT_STENCIL_BIT;
        }

        RHITextureViewCreateInfo viewCI{};
        viewCI.format       = m_baseInfo.format;
        viewCI.type         = m_baseInfo.type;
        viewCI.arrayLayers  = m_baseInfo.arrayLayers;
        viewCI.mipLevels    = m_baseInfo.mipmaps;
        viewCI.baseMipLevel = 0;
        viewCI.tag          = m_baseInfo.tag;

        m_pDefaultView = CreateView(viewCI);
    }
    else
    {
        LOGE("Texture '{}' allocation failed", m_baseInfo.tag.CStr());
    }
}

void VulkanTexture::Destroy()
{
    DestroyOwnedViews();
    if (m_vkImage != VK_NULL_HANDLE)
    {
        GVulkanRHI->RemoveImageLayout(m_vkImage);
        GVkMemAllocator->FreeImage(m_vkImage, m_memAlloc);
    }
    this->~VulkanTexture();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

VulkanTextureView* VulkanTextureView::CreateObject(VulkanTexture* pTexture,
                                                   const RHITextureViewCreateInfo& createInfo)
{
    VulkanTextureView* pView =
        VersatileResource::AllocMem<VulkanTextureView>(GVulkanRHI->GetResourceAllocator());

    new (pView) VulkanTextureView(pTexture, createInfo);

    pView->Init();
    if (pView->m_vkImageView == VK_NULL_HANDLE)
    {
        pView->ReleaseReference();
        pView = nullptr;
    }

    return pView;
}

void VulkanTextureView::Init()
{
    VulkanTexture* pVkTexture = TO_VK_TEXTURE(m_pTexture);

    RHITextureSubResourceRange range = m_subResourceRange;

    // Combined depth/stencil formats use the existing depth-sampling default.
    if (FormatIsDepthStencil(m_viewInfo.format) && m_viewInfo.aspect.IsEmpty())
    {
        range.aspect = int64_t(RHITextureAspectFlagBits::eDepth);
    }

    const VkImageViewCreateInfo imageViewCI = MakeVkImageViewCreateInfo(
        m_viewInfo.type, m_viewInfo.format, pVkTexture->GetVkImage(), range);

    const VkResult result =
        vkCreateImageView(GVulkanRHI->GetVkDevice(), &imageViewCI, nullptr, &m_vkImageView);
    if (result != VK_SUCCESS)
    {
        m_vkImageView = VK_NULL_HANDLE;
        LOGE("Texture view '{}' creation failed: {}", m_viewInfo.tag.CStr(),
             GetResultString(result));
    }
    else if (!m_viewInfo.tag.IsNone())
    {
        GVulkanRHI->GetDevice()->SetObjectName(
            VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(m_vkImageView), m_viewInfo.tag);
    }
}

void VulkanTextureView::Destroy()
{
    if (m_vkImageView != nullptr)
    {
        vkDestroyImageView(GVulkanRHI->GetVkDevice(), m_vkImageView, nullptr);
        m_vkImageView = nullptr;
    }

    this->~VulkanTextureView();
    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

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

} // namespace zen
