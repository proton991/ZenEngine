#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanResourceSharing.h"
#include "Graphics/VulkanRHI/VulkanCopyCapabilities.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "ScopedVulkanCall.h"
#include <gtest/gtest.h>
#include <array>

namespace
{
template <typename Info> void CheckAllocationStorage()
{
    for (uint32_t transferFamily : {2u, 7u})
    {
        for (bool transferUsage : {false, true})
        {
            Info info{};
            bool called = false;
            std::array<uint32_t, 2> consumed{};
            zen::AllocateWithQueueSharing(info, 2, transferFamily, transferUsage, [&] {
                called = true;

                if (transferUsage && transferFamily == 7)
                {
                    EXPECT_EQ(info.sharingMode, VK_SHARING_MODE_CONCURRENT);
                    ASSERT_EQ(info.queueFamilyIndexCount, 2u);
                    ASSERT_NE(info.pQueueFamilyIndices, nullptr);
                    // Read at the allocation boundary, after sharing-mode setup has completed.
                    consumed = {info.pQueueFamilyIndices[0], info.pQueueFamilyIndices[1]};
                }
                else
                {
                    EXPECT_EQ(info.sharingMode, VK_SHARING_MODE_EXCLUSIVE);
                    EXPECT_EQ(info.queueFamilyIndexCount, 0u);
                    EXPECT_EQ(info.pQueueFamilyIndices, nullptr);
                }
            });

            EXPECT_TRUE(called);

            if (transferUsage && transferFamily == 7)
            {
                EXPECT_EQ(consumed, (std::array<uint32_t, 2>{2, 7}));
            }

            EXPECT_EQ(info.queueFamilyIndexCount, 0u);
            EXPECT_EQ(info.pQueueFamilyIndices, nullptr);
        }
    }
}
} // namespace

TEST(VulkanResourceCreationTests, BufferAllocationConsumesLiveQueueFamilyStorage)
{
    CheckAllocationStorage<VkBufferCreateInfo>();
}

TEST(VulkanResourceCreationTests, ImageAllocationConsumesLiveQueueFamilyStorage)
{
    CheckAllocationStorage<VkImageCreateInfo>();
}

TEST(VulkanResourceCreationTests, CopyCapabilitiesUseOptimalTilingFeaturesOnly)
{
    VkFormatProperties properties{};
    properties.linearTilingFeatures = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    properties.bufferFeatures            = properties.linearTilingFeatures;
    zen::RHITextureCopyCapabilities caps = zen::MakeTextureCopyCapabilities(properties);
    EXPECT_FALSE(caps.transferSrc || caps.transferDst || caps.blitSrc || caps.blitDst ||
                 caps.linearFilter);
    properties.optimalTilingFeatures =
        properties.linearTilingFeatures & ~VK_FORMAT_FEATURE_BLIT_DST_BIT;
    caps = zen::MakeTextureCopyCapabilities(properties);
    EXPECT_TRUE(caps.transferSrc && caps.transferDst && caps.blitSrc && caps.linearFilter);
    EXPECT_FALSE(caps.blitDst);
}

TEST(VulkanResourceCreationTests,
     QueueCopyCapabilitiesPreserveGranularityAndImplicitTransferSupport)
{
    VkQueueFamilyProperties properties{};
    properties.queueFlags                  = VK_QUEUE_TRANSFER_BIT;
    properties.minImageTransferGranularity = {4, 8, 16};
    zen::RHIQueueCopyCapabilities caps     = zen::MakeQueueCopyCapabilities(properties);
    EXPECT_TRUE(caps.transfer);
    EXPECT_FALSE(caps.graphics || caps.compute);
    EXPECT_EQ(caps.minImageTransferGranularity, (std::array<uint32_t, 3>{4, 8, 16}));

    for (VkQueueFlagBits const flags : {VK_QUEUE_GRAPHICS_BIT, VK_QUEUE_COMPUTE_BIT})
    {
        properties.queueFlags = flags;
        caps                  = zen::MakeQueueCopyCapabilities(properties);
        EXPECT_TRUE(caps.transfer);
        EXPECT_EQ(caps.graphics, flags == VK_QUEUE_GRAPHICS_BIT);
        EXPECT_EQ(caps.compute, flags == VK_QUEUE_COMPUTE_BIT);
    }

    properties.queueFlags                  = VK_QUEUE_SPARSE_BINDING_BIT;
    properties.minImageTransferGranularity = {};
    caps                                   = zen::MakeQueueCopyCapabilities(properties);

    EXPECT_FALSE(caps.transfer);
    EXPECT_EQ(caps.minImageTransferGranularity, (std::array<uint32_t, 3>{0, 0, 0}));
}

TEST(VulkanResourceCreationTests, ViewCreateInfoPreservesSelectedMipsAndArrayShape)
{
    using namespace zen;
    const RHITextureSubResourceRange range = RHITextureSubResourceRange::Color(2, 0, 3, 4);
    const VkImageViewCreateInfo info       = MakeVkImageViewCreateInfo(
        RHITextureType::e2D, DataFormat::eR8G8B8A8UNORM, VK_NULL_HANDLE, range);
    EXPECT_EQ(info.sType, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    EXPECT_EQ(info.viewType, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    EXPECT_EQ(info.subresourceRange.baseMipLevel, 2u);
    EXPECT_EQ(info.subresourceRange.levelCount, 3u);
    EXPECT_EQ(info.subresourceRange.layerCount, 4u);
    EXPECT_EQ(info.subresourceRange.aspectMask, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageViewCreateInfo depth =
        MakeVkImageViewCreateInfo(RHITextureType::e2D, DataFormat::eD32SFloat, VK_NULL_HANDLE,
                                  RHITextureSubResourceRange::Depth(1));
    EXPECT_EQ(depth.viewType, VK_IMAGE_VIEW_TYPE_2D);
    EXPECT_EQ(depth.subresourceRange.aspectMask, VK_IMAGE_ASPECT_DEPTH_BIT);
    EXPECT_EQ(depth.subresourceRange.baseMipLevel, 1u);
}

TEST(VulkanDescriptorLayoutTest, CanonicalLayoutIdentityIncludesBindingFlagsAndSamplers)
{
    using namespace zen;
    test::ScopedVulkanCall<PFN_vkGetPhysicalDeviceProperties> properties(
        vkGetPhysicalDeviceProperties,
        [](VkPhysicalDevice, VkPhysicalDeviceProperties* properties) { *properties = {}; });
    VulkanDevice device(VK_NULL_HANDLE);
    VulkanDescriptorPoolManager2 manager(&device);
    VkDescriptorSetLayoutBinding bindings[] = {
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {7, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorBindingFlags flags[] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0};
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 2;
    info.pBindings    = bindings;
    auto id           = [&] {
        return manager.GetOrCreateLayoutId(info, MakeVecView(flags));
    };
    const uint32_t original = id();
    std::swap(bindings[0], bindings[1]);
    std::swap(flags[0], flags[1]);
    EXPECT_EQ(id(), original);
    flags[0] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    EXPECT_NE(id(), original);
    flags[0]                    = 0;
    bindings[1].descriptorCount = 2;
    EXPECT_NE(id(), original);
    bindings[1].descriptorCount = 3;
    bindings[1].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT;
    EXPECT_NE(id(), original);
    bindings[1].stageFlags     = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    EXPECT_NE(id(), original);
    bindings[1].descriptorType     = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkSampler sampler              = reinterpret_cast<VkSampler>(uintptr_t(100));
    bindings[0].pImmutableSamplers = &sampler;
    const uint32_t immutable       = id();
    EXPECT_NE(immutable, original);
    sampler = reinterpret_cast<VkSampler>(uintptr_t(101));
    EXPECT_NE(id(), immutable);
    manager.Destroy();
}
