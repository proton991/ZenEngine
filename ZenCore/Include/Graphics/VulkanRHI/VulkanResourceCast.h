#pragma once

#include "VulkanBuffer.h"
#include "VulkanTexture.h"
#include "VulkanPipeline.h"

namespace zen
{
inline VulkanTexture* TO_VK_TEXTURE(RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eTexture);
#endif
    return static_cast<VulkanTexture*>(resource);
}

inline const VulkanTexture* TO_CVK_TEXTURE(const RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eTexture);
#endif
    return static_cast<const VulkanTexture*>(resource);
}

inline const VulkanTextureView* TO_VK_TEXTURE_VIEW(const RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eTextureView);
#endif
    return static_cast<const VulkanTextureView*>(resource);
}

inline VulkanBuffer* TO_VK_BUFFER(RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eBuffer);
#endif
    return static_cast<VulkanBuffer*>(resource);
}

inline VulkanPipeline* TO_VK_PIPELINE(RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::ePipeline);
#endif
    return static_cast<VulkanPipeline*>(resource);
}

inline VulkanShader* TO_VK_SHADER(RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eShader);
#endif
    return static_cast<VulkanShader*>(resource);
}

inline const VulkanShader* TO_CVK_SHADER(const RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eShader);
#endif
    return static_cast<const VulkanShader*>(resource);
}

inline VulkanSampler* TO_VK_SAMPLER(RHIResource* resource)
{
#ifndef NDEBUG
    ASSERT(resource == nullptr || resource->GetResourceType() == RHIResourceType::eSampler);
#endif
    return static_cast<VulkanSampler*>(resource);
}

inline VulkanBuffer* TryVulkanBuffer(RHIResource* resource)
{
    VulkanBuffer* result = nullptr;

    if (resource != nullptr && resource->GetResourceType() == RHIResourceType::eBuffer)
    {
        result = static_cast<VulkanBuffer*>(resource);
    }

    return result;
}

inline const VulkanBuffer* TryVulkanBuffer(const RHIResource* resource)
{
    const VulkanBuffer* result = nullptr;

    if (resource != nullptr && resource->GetResourceType() == RHIResourceType::eBuffer)
    {
        result = static_cast<const VulkanBuffer*>(resource);
    }

    return result;
}

inline VulkanSampler* TryVulkanSampler(RHIResource* resource)
{
    VulkanSampler* result = nullptr;

    if (resource != nullptr && resource->GetResourceType() == RHIResourceType::eSampler)
    {
        result = static_cast<VulkanSampler*>(resource);
    }

    return result;
}

inline const VulkanSampler* TryVulkanSampler(const RHIResource* resource)
{
    const VulkanSampler* result = nullptr;

    if (resource != nullptr && resource->GetResourceType() == RHIResourceType::eSampler)
    {
        result = static_cast<const VulkanSampler*>(resource);
    }

    return result;
}

} // namespace zen
