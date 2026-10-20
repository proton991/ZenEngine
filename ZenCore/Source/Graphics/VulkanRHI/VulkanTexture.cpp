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
bool IsTextureShapeValid(const RHITextureCreateInfo& info)
{
    const bool extents = info.width > 0 && info.height > 0 && info.depth > 0 && info.arrayLayers > 0 && info.mipmaps > 0;
    bool       shape   = false;
    switch (info.type)
    {
        case RHITextureType::e1D: shape = info.height == 1 && info.depth == 1; break;
        case RHITextureType::e2D: shape = info.depth == 1; break;
        case RHITextureType::e3D: shape = info.arrayLayers == 1; break;
        case RHITextureType::eCube: shape = info.width == info.height && info.depth == 1 && info.arrayLayers % 6 == 0; break;
        default: break;
    }
    const bool samples = info.samples < SampleCount::eMax
                      && (info.samples == SampleCount::e1 || (info.type == RHITextureType::e2D && info.mipmaps == 1));
    return extents && shape && samples
        && info.mipmaps <= RHITexture::CalculateTextureMipLevels(info.width, info.height, info.depth);
}

VkImageCreateInfo BuildImageCreateInfo(const RHITextureCreateInfo& info)
{
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

    if (info.type == RHITextureType::eCube)
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

    if (info.mutableFormat != false)
    {
        imageCI.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }

    return imageCI;
}

float ClampSamplerAnisotropy(float requested, float limit)
{
    return std::isnan(requested) ? 1.0f : std::clamp(requested, 1.0f, limit);
}

namespace
{
bool ImageUsageAllowsViews(VkImageUsageFlags usage)
{
    constexpr VkImageUsageFlags viewUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                          | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    return (usage & viewUsage) != 0;
}

bool IsTextureViewFormatSupported(const RHITextureCreateInfo& image, DataFormat format)
{
    const uint32_t pixelSize = GetTextureFormatPixelSize(format);
    // Current RHI color formats are uncompressed: their Vulkan compatibility class is their texel size.
    // Depth/stencil formats are only compatible with themselves.
    bool supported = pixelSize != 0
                  && (format == image.format
                      || (image.mutableFormat && GetTextureFormatAspects(image.format).HasFlag(RHITextureAspectFlagBits::eColor)
                          && GetTextureFormatAspects(format).HasFlag(RHITextureAspectFlagBits::eColor)
                          && pixelSize == GetTextureFormatPixelSize(image.format)));
    if (supported && format != image.format)
    {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(GVulkanRHI->GetDevice()->GetPhysicalDeviceHandle(), ToVkFormat(format),
                                            &properties);

        VkFormatFeatureFlags required = 0;
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eSampled))
        {
            required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        }
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eStorage))
        {
            required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        }
        if (image.usageFlags.HasFlag(RHITextureUsageFlagBits::eColorAttachment)
            || image.usageFlags.HasFlag(RHITextureUsageFlagBits::eInputAttachment))
        {
            required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        }
        // Only color formats can differ from the image format. Identical formats were checked at image creation.
        supported = properties.optimalTilingFeatures != 0 && (properties.optimalTilingFeatures & required) == required;
    }
    return supported;
}

bool IsClampAddressMode(RHISamplerRepeatMode mode)
{
    return mode == RHISamplerRepeatMode::eClampToEdge || mode == RHISamplerRepeatMode::eClampToBorder;
}

const char* ValidateTextureDescription(const RHITextureCreateInfo& info)
{
    const char* error = nullptr;
    if (info.format == DataFormat::eUndefined || GetTextureFormatPixelSize(info.format) == 0 || info.usageFlags.IsEmpty())
    {
        error = "format and usage must be specified";
    }
    else if (!IsTextureShapeValid(info))
    {
        error = "invalid extent, type, sample count or mip count";
    }
    else if (info.type == RHITextureType::eCube && info.arrayLayers > 6 && !GVulkanRHI->QueryGPUInfo().supportImageCubeArray)
    {
        error = "cube arrays are unsupported";
    }
    else
    {
        const VkImageCreateInfo native = BuildImageCreateInfo(info);
        VkImageFormatProperties properties{};
        const VkResult          result =
            vkGetPhysicalDeviceImageFormatProperties(GVulkanRHI->GetDevice()->GetPhysicalDeviceHandle(), native.format,
                                                     native.imageType, native.tiling, native.usage, native.flags, &properties);
        if (result != VK_SUCCESS)
        {
            error = "format/type/usage combination is unsupported";
        }
        else if (info.width > properties.maxExtent.width || info.height > properties.maxExtent.height
                 || info.depth > properties.maxExtent.depth || info.arrayLayers > properties.maxArrayLayers
                 || info.mipmaps > properties.maxMipLevels || (properties.sampleCounts & native.samples) == 0)
        {
            error = "image exceeds device format limits";
        }
    }
    return error;
}
} // namespace

RHISampler* VulkanResourceFactory::CreateSampler(const RHISamplerCreateInfo& createInfo)
{
    RHISampler* pSampler = GVulkanRHI->AreSubmissionsBlocked() ? nullptr : VulkanSampler::CreateObject(createInfo);

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

VulkanSampler* VulkanSampler::CreateObject(const RHISamplerCreateInfo& createInfo)
{
    VulkanSampler* pSampler = nullptr;
    const bool     mirror   = createInfo.repeatU == RHISamplerRepeatMode::eMirrorClampToEdge
                           || createInfo.repeatV == RHISamplerRepeatMode::eMirrorClampToEdge
                           || createInfo.repeatW == RHISamplerRepeatMode::eMirrorClampToEdge;
    const bool     unnormalized =
        !createInfo.unnormalizedUVW
        || (createInfo.minFilter == createInfo.magFilter && createInfo.mipFilter == RHISamplerFilter::eNearest
            && createInfo.minLod == 0 && createInfo.maxLod == 0 && !createInfo.useAnisotropy && !createInfo.enableCompare
            && IsClampAddressMode(createInfo.repeatU) && IsClampAddressMode(createInfo.repeatV)
            && IsClampAddressMode(createInfo.repeatW));
    if ((!mirror || GVulkanRHI->QueryGPUInfo().supportSamplerMirrorClampToEdge) && unnormalized
        && createInfo.maxLod >= createInfo.minLod
        && std::abs(createInfo.lodBias) <= GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxSamplerLodBias)
    {
        pSampler = VersatileResource::AllocMem<VulkanSampler>(GVulkanRHI->GetResourceAllocator());
        new (pSampler) VulkanSampler(createInfo);

        pSampler->Init();

        if (pSampler->m_vkSampler == VK_NULL_HANDLE)
        {
            pSampler->ReleaseReference();

            pSampler = nullptr;
        }
    }
    else
    {
        LOGE("Invalid sampler description: address mode, unnormalized coordinates or LOD limits");
    }
    return pSampler;
}

void VulkanSampler::Init()
{
    VkSamplerCreateInfo samplerCI;

    InitVkStruct(samplerCI, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);

    samplerCI.magFilter = ToVkFilter(m_baseInfo.magFilter);

    samplerCI.minFilter = ToVkFilter(m_baseInfo.minFilter);

    samplerCI.mipmapMode =
        m_baseInfo.mipFilter == RHISamplerFilter::eLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;

    samplerCI.addressModeU = ToVkSamplerAddressMode(m_baseInfo.repeatU);

    samplerCI.addressModeV = ToVkSamplerAddressMode(m_baseInfo.repeatV);

    samplerCI.addressModeW = ToVkSamplerAddressMode(m_baseInfo.repeatW);

    samplerCI.mipLodBias   = m_baseInfo.lodBias;

    samplerCI.anisotropyEnable =
        m_baseInfo.useAnisotropy && (GVulkanRHI->GetDevice()->GetPhysicalDeviceFeatures().samplerAnisotropy == VK_TRUE);

    samplerCI.maxAnisotropy = ClampSamplerAnisotropy(
        m_baseInfo.maxAnisotropy, GVulkanRHI->GetDevice()->GetPhysicalDeviceProperties().limits.maxSamplerAnisotropy);

    samplerCI.compareEnable           = m_baseInfo.enableCompare;

    samplerCI.compareOp               = ToVkCompareOp(m_baseInfo.compareOp);

    samplerCI.minLod                  = m_baseInfo.minLod;

    samplerCI.maxLod                  = m_baseInfo.maxLod;

    samplerCI.borderColor             = ToVkBorderColor(m_baseInfo.borderColor);

    samplerCI.unnormalizedCoordinates = m_baseInfo.unnormalizedUVW;

    const VkResult result             = vkCreateSampler(GVulkanRHI->GetVkDevice(), &samplerCI, nullptr, &m_vkSampler);

    if (result != VK_SUCCESS)
    {
        m_vkSampler = VK_NULL_HANDLE;

        ReportVulkanCreationFailure(result, "vkCreateSampler", __FILE__, __LINE__);

        LOGE("vkCreateSampler failed: {}", GetResultString(result));
    }
}

void VulkanSampler::Destroy()
{
    vkDestroySampler(GVulkanRHI->GetVkDevice(), m_vkSampler, nullptr);

    this->~VulkanSampler();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

RHITexture* VulkanResourceFactory::CreateTexture(const RHITextureCreateInfo& createInfo)
{
    RHITexture* pTexture = GVulkanRHI->AreSubmissionsBlocked() ? nullptr : VulkanTexture::CreateObject(createInfo);

    return pTexture;
}

RHITexture* VulkanRHI::CreateTexture(const RHITextureCreateInfo& createInfo)
{
    return GVulkanRHI->GetResourceFactory()->CreateTexture(createInfo);
}

RHITextureView* VulkanRHI::CreateTextureView(RHITexture* pBaseTexture, const RHITextureViewCreateInfo& createInfo)
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
    VulkanTexture* pTexture = nullptr;
    const char*    error    = ValidateTextureDescription(createInfo);
    if (error == nullptr)
    {
        pTexture = VersatileResource::AllocMem<VulkanTexture>(GVulkanRHI->GetResourceAllocator());

        new (pTexture) VulkanTexture(createInfo);

        pTexture->Init();

        const bool needsAttachmentView = createInfo.type != RHITextureType::e3D
                                      && (createInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eColorAttachment)
                                          || createInfo.usageFlags.HasFlag(RHITextureUsageFlagBits::eDepthStencilAttachment));

        if (pTexture->GetVkImage() == VK_NULL_HANDLE
            || (ImageUsageAllowsViews(pTexture->GetVkImageUsage()) && pTexture->GetDefaultView() == nullptr)
            || (needsAttachmentView && pTexture->GetAttachmentView() == nullptr))
        {
            pTexture->ReleaseReference();

            pTexture = nullptr;
        }
    }
    else
    {
        GVulkanRHI->SetCreationError(MakeRHIError(RHIErrorCode::eInvalidArgument, error, __FILE__, __LINE__));

        LOGE("Texture '{}': {}", createInfo.tag.CStr(), error);
    }
    return pTexture;
}

RHITextureView* VulkanTexture::CreateView(const RHITextureViewCreateInfo& createInfo)
{
    return GetRHIThread().Invoke(&VulkanTexture::CreateViewOnRHIThread, this, std::cref(createInfo));
}

RHITextureView* VulkanTexture::CreateViewOnRHIThread(const RHITextureViewCreateInfo& createInfo)
{
    const char* error = nullptr;

    if (!ImageUsageAllowsViews(m_vkImageCI.usage))
    {
        error = "Texture usage does not permit image views";
    }
    else if (!IsTextureViewFormatSupported(m_baseInfo, createInfo.format))
    {
        error = "Texture view format is incompatible or unsupported for the image usage";
    }
    else if (createInfo.mipLevels == 0 || createInfo.baseMipLevel >= m_baseInfo.mipmaps
             || createInfo.mipLevels > m_baseInfo.mipmaps - createInfo.baseMipLevel || createInfo.arrayLayers == 0
             || createInfo.baseArrayLayer >= m_baseInfo.arrayLayers
             || createInfo.arrayLayers > m_baseInfo.arrayLayers - createInfo.baseArrayLayer)
    {
        error = "Texture view mip or array-layer range is outside its image";
    }
    else if (createInfo.type != m_baseInfo.type
             && !(m_baseInfo.type == RHITextureType::eCube && createInfo.type == RHITextureType::e2D))
    {
        error = "Texture view type is incompatible with its image";
    }
    else if (createInfo.type == RHITextureType::eCube && createInfo.arrayLayers % 6 != 0)
    {
        error = "Cube views require a multiple of six array layers";
    }
    else if (createInfo.type == RHITextureType::eCube && createInfo.arrayLayers > 6
             && !GVulkanRHI->QueryGPUInfo().supportImageCubeArray)
    {
        error = "Cube-array views require imageCubeArray";
    }
    else if ((int64_t(createInfo.aspect) & ~int64_t(GetTextureFormatAspects(createInfo.format))) != 0)
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
        LOGE("Rendering to 3D texture slices is not supported");
    }
    else if (m_pAttachmentView == nullptr)
    {
        if (m_baseInfo.mipmaps == 1 && m_baseInfo.type != RHITextureType::eCube)
        {
            m_pAttachmentView = m_pDefaultView;
        }
        else
        {
            RHITextureViewCreateInfo info{};

            info.format       = m_baseInfo.format;

            info.type         = m_baseInfo.type == RHITextureType::eCube ? RHITextureType::e2D : m_baseInfo.type;

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

    const VkImageView imageView           = pDefaultView != nullptr ? pDefaultView->GetVkImageView() : VK_NULL_HANDLE;

    return imageView;
}

void VulkanTexture::Init()
{
    VkImageCreateInfo imageCI          = BuildImageCreateInfo(m_baseInfo);

    const uint32_t graphicsQueueFamily = GVulkanRHI->GetDevice()->GetGfxQueue()->GetFamilyIndex();

    const uint32_t computeQueueFamily  = GVulkanRHI->GetDevice()->GetComputeQueue()->GetFamilyIndex();

    const uint32_t transferQueueFamily = GVulkanRHI->GetDevice()->GetTransferQueue()->GetFamilyIndex();

    bool allocated                     = false;

    AllocateWithQueueSharing(imageCI, graphicsQueueFamily, computeQueueFamily, transferQueueFamily,
                             (imageCI.usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) != 0,
                             [this, &imageCI, &allocated] {
                                 allocated =
                                     GVkMemAllocator->AllocImage(&imageCI, m_baseInfo.cpuReadable, &m_vkImage, &m_memAlloc);
                             });

    if (allocated)
    {
        m_vkImageCI = imageCI;

        if (!m_baseInfo.tag.IsNone())
        {
            GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(m_vkImage), m_baseInfo.tag);
        }

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

        if (ImageUsageAllowsViews(imageCI.usage))
        {
            RHITextureViewCreateInfo viewCI{};

            viewCI.format       = m_baseInfo.format;

            viewCI.type         = m_baseInfo.type;

            viewCI.arrayLayers  = m_baseInfo.arrayLayers;

            viewCI.mipLevels    = m_baseInfo.mipmaps;

            viewCI.baseMipLevel = 0;

            viewCI.tag          = m_baseInfo.tag;

            m_pDefaultView      = CreateView(viewCI);
        }
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
        GVkMemAllocator->FreeImage(m_vkImage, m_memAlloc);
    }

    this->~VulkanTexture();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

VulkanTextureView* VulkanTextureView::CreateObject(VulkanTexture* pTexture, const RHITextureViewCreateInfo& createInfo)
{
    VulkanTextureView* pView = VersatileResource::AllocMem<VulkanTextureView>(GVulkanRHI->GetResourceAllocator());

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
    VulkanTexture* pVkTexture        = TO_VK_TEXTURE(m_pTexture);

    RHITextureSubResourceRange range = m_subResourceRange;

    // Combined depth/stencil formats use the existing depth-sampling default.
    if (FormatIsDepthStencil(m_viewInfo.format) && m_viewInfo.aspect.IsEmpty())
    {
        range.aspect = int64_t(RHITextureAspectFlagBits::eDepth);
    }

    const VkImageViewCreateInfo imageViewCI =
        MakeVkImageViewCreateInfo(m_viewInfo.type, m_viewInfo.format, pVkTexture->GetVkImage(), range);

    const VkResult result = vkCreateImageView(GVulkanRHI->GetVkDevice(), &imageViewCI, nullptr, &m_vkImageView);

    if (result != VK_SUCCESS)
    {
        m_vkImageView = VK_NULL_HANDLE;

        ReportVulkanCreationFailure(result, "vkCreateImageView", __FILE__, __LINE__);

        LOGE("Texture view '{}' creation failed: {}", m_viewInfo.tag.CStr(), GetResultString(result));
    }
    else if (!m_viewInfo.tag.IsNone())
    {
        GVulkanRHI->GetDevice()->SetObjectName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(m_vkImageView),
                                               m_viewInfo.tag);
    }
}

void VulkanTextureView::Destroy()
{
    if (m_vkImageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(GVulkanRHI->GetVkDevice(), m_vkImageView, nullptr);

        m_vkImageView = nullptr;
    }

    this->~VulkanTextureView();

    VersatileResource::Free(GVulkanRHI->GetResourceAllocator(), this);
}

} // namespace zen
