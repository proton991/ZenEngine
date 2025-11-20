#pragma once
#include "VulkanHeaders.h"
#include "VulkanMemory.h"
#include "Graphics/RHI/RHIResource.h"

namespace zen::rhi
{
// struct VulkanTexture
// {
//     static VkImageSubresourceRange GetSubresourceRange(
//         VkImageAspectFlags aspect,
//         uint32_t baseMipLevel   = 0,
//         uint32_t levelCount     = VK_REMAINING_MIP_LEVELS,
//         uint32_t baseArrayLayer = 0,
//         uint32_t layerCount     = VK_REMAINING_ARRAY_LAYERS)
//     {
//         VkImageSubresourceRange range{};
//         range.aspectMask     = aspect;
//         range.baseMipLevel   = baseMipLevel;
//         range.levelCount     = levelCount;
//         range.baseArrayLayer = baseArrayLayer;
//         range.layerCount     = layerCount;
//         return range;
//     }
//
//     VkImage image{VK_NULL_HANDLE};
//     VkImageView imageView{VK_NULL_HANDLE};
//     VkImageCreateInfo imageCI{};
//     VulkanMemoryAllocation memAlloc{};
//     bool isProxy{false};
// };

class VulkanSampler : public RHISampler
{
public:
    static VulkanSampler* CreateObject(const RHISamplerCreateInfo& createInfo);

    VkSampler GetVkSampler() const
    {
        return m_vkSampler;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    VulkanSampler(const RHISamplerCreateInfo& createInfo) : RHISampler(createInfo) {}

    VkSampler m_vkSampler{VK_NULL_HANDLE};
};

class VulkanTexture : public RHITexture
{
public:
    static VulkanTexture* CreateObject(const RHITextureCreateInfo& createInfo);

    static VulkanTexture* CreateProxyObject(const VulkanTexture* pBaseTexture,
                                            const RHITextureProxyCreateInfo& proxyInfo);

    VkImageSubresourceRange GetVkSubresourceRange(uint32_t baseMipLevel,
                                                  uint32_t levelCount,
                                                  uint32_t baseArrayLayer,
                                                  uint32_t layerCount) const
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = m_vkAspectFlags;
        range.baseMipLevel   = baseMipLevel;
        range.levelCount     = levelCount;
        range.baseArrayLayer = baseArrayLayer;
        range.layerCount     = layerCount;
        return range;
    }

    VkImageSubresourceRange GetVkSubresourceRange() const
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = m_vkAspectFlags;
        range.baseMipLevel   = 0;
        range.levelCount     = m_vkImageCI.mipLevels;
        range.baseArrayLayer = 0;
        range.layerCount     = m_vkImageCI.arrayLayers;
        return range;
    }

    static VkImageSubresourceRange GetVkSubresourceRange(
        VkImageAspectFlags aspect,
        uint32_t baseMipLevel   = 0,
        uint32_t levelCount     = VK_REMAINING_MIP_LEVELS,
        uint32_t baseArrayLayer = 0,
        uint32_t layerCount     = VK_REMAINING_ARRAY_LAYERS)
    {
        VkImageSubresourceRange range{};
        range.aspectMask     = aspect;
        range.baseMipLevel   = baseMipLevel;
        range.levelCount     = levelCount;
        range.baseArrayLayer = baseArrayLayer;
        range.layerCount     = layerCount;
        return range;
    }

    VkImage GetVkImage() const
    {
        return m_vkImage;
    }

    VkImageView GetVkImageView() const
    {
        return m_vkImageView;
    }

    const VulkanMemoryAllocation& GetMemoryAllocation() const
    {
        return m_memAlloc;
    }

    const VkImageCreateInfo& GetVkImageCreateInfo() const
    {
        return m_vkImageCI;
    }

    VkImageUsageFlags GetVkImageUsage() const
    {
        return m_vkImageCI.usage;
    }

protected:
    void Init() override;

    void Destroy() override;

private:
    explicit VulkanTexture(const RHITextureCreateInfo& createInfo) : RHITexture(createInfo) {}

    VulkanTexture(const VulkanTexture* pBaseTexture, const RHITextureProxyCreateInfo& proxyInfo);

    void InitProxy();

    void CreateImageViewHelper();

    VkImage m_vkImage{VK_NULL_HANDLE};
    VkImageView m_vkImageView{VK_NULL_HANDLE};
    VkImageCreateInfo m_vkImageCI{};
    VulkanMemoryAllocation m_memAlloc{};

    VkImageAspectFlags m_vkAspectFlags{};

    // VulkanTexture* m_pBaseTexture{nullptr};
};
} // namespace zen::rhi