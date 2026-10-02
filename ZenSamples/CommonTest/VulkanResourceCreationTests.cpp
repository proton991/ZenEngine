#include "Graphics/RHI/RHIResource.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanResourceSharing.h"
#include "Graphics/VulkanRHI/VulkanCopyCapabilities.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "ScopedVulkanCall.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>

namespace
{
template <typename Info> void CheckAllocationStorage()
{
    struct SharingCase
    {
        uint32_t computeFamily;
        uint32_t transferFamily;
        bool transferUsage;
        uint32_t familyCount;
        std::array<uint32_t, 3> families;
    };
    const SharingCase cases[] = {
        {2, 2, false, 1, {2}},    {2, 2, true, 1, {2}},      {2, 7, false, 1, {2}},
        {2, 7, true, 2, {2, 7}},  {2, 11, false, 1, {2}},    {2, 11, true, 2, {2, 11}},
        {7, 2, false, 2, {2, 7}}, {7, 2, true, 2, {2, 7}},   {7, 7, false, 2, {2, 7}},
        {7, 7, true, 2, {2, 7}},  {7, 11, false, 2, {2, 7}}, {7, 11, true, 3, {2, 7, 11}}};
    for (const SharingCase& testCase : cases)
    {
        SCOPED_TRACE(testing::Message() << "compute=" << testCase.computeFamily
                                        << " transfer=" << testCase.transferFamily
                                        << " transferUsage=" << testCase.transferUsage);
        Info info{};
        bool called = false;
        std::array<uint32_t, 3> consumed{};
        zen::AllocateWithQueueSharing(
            info, 2, testCase.computeFamily, testCase.transferFamily, testCase.transferUsage, [&] {
                called = true;

                if (testCase.familyCount > 1)
                {
                    EXPECT_EQ(info.sharingMode, VK_SHARING_MODE_CONCURRENT);
                    ASSERT_EQ(info.queueFamilyIndexCount, testCase.familyCount);
                    ASSERT_NE(info.pQueueFamilyIndices, nullptr);
                    // Read at the allocation boundary, after sharing-mode setup has completed.
                    std::copy_n(info.pQueueFamilyIndices, info.queueFamilyIndexCount,
                                consumed.begin());
                }
                else
                {
                    EXPECT_EQ(info.sharingMode, VK_SHARING_MODE_EXCLUSIVE);
                    EXPECT_EQ(info.queueFamilyIndexCount, 0u);
                    EXPECT_EQ(info.pQueueFamilyIndices, nullptr);
                }
            });

        EXPECT_TRUE(called);

        if (testCase.familyCount > 1)
        {
            EXPECT_EQ(consumed, testCase.families);
        }

        EXPECT_EQ(info.queueFamilyIndexCount, 0u);
        EXPECT_EQ(info.pQueueFamilyIndices, nullptr);
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

namespace
{
zen::RHIShaderResourceDescriptor UniformDescriptor(
    zen::RHIShaderResourceType type,
    uint32_t arraySize,
    zen::BitField<zen::RHIShaderStageFlagBits> stages,
    bool bindless = false)
{
    zen::RHIShaderResourceDescriptor descriptor{};
    descriptor.type       = type;
    descriptor.arraySize  = arraySize;
    descriptor.stageFlags = stages;
    descriptor.bindless   = bindless;

    return descriptor;
}
} // namespace

TEST(VulkanDescriptorLayoutTest, UniformBufferLimitsCountEveryDynamicElementPerLayoutAndStage)
{
    using namespace zen;

    BitField<RHIShaderStageFlagBits> vertexFragment;
    vertexFragment.SetFlag(RHIShaderStageFlagBits::eVertex);
    vertexFragment.SetFlag(RHIShaderStageFlagBits::eFragment);

    BitField<RHIShaderStageFlagBits> fragment;
    fragment.SetFlag(RHIShaderStageFlagBits::eFragment);

    RHIShaderResourceDescriptorTable table(3);
    table[1].push_back(UniformDescriptor(RHIShaderResourceType::eUniformBuffer, 3, vertexFragment));
    table[1].push_back(UniformDescriptor(RHIShaderResourceType::eStorageBuffer, 5, fragment));
    table[2].push_back(UniformDescriptor(RHIShaderResourceType::eUniformBuffer, 1, fragment));
    // Bindless descriptors are bounded by the update-after-bind limits instead.
    table[2].push_back(
        UniformDescriptor(RHIShaderResourceType::eUniformBuffer, 64, fragment, true));

    const VulkanUniformBufferUsage usage = CountUniformBufferDescriptors(table);

    EXPECT_EQ(usage.dynamicCount, 4u);
    EXPECT_EQ(usage.maxPerStageCount, 4u);

    VkPhysicalDeviceLimits limits{};
    limits.maxDescriptorSetUniformBuffersDynamic = 4;
    limits.maxDescriptorSetUniformBuffers        = 4;
    limits.maxPerStageDescriptorUniformBuffers   = 4;

    EXPECT_TRUE(UniformBuffersFitLimits(usage, limits));

    limits.maxDescriptorSetUniformBuffersDynamic = 3;

    EXPECT_FALSE(UniformBuffersFitLimits(usage, limits));

    limits.maxDescriptorSetUniformBuffersDynamic = 4;
    limits.maxDescriptorSetUniformBuffers        = 3;

    EXPECT_FALSE(UniformBuffersFitLimits(usage, limits));

    limits.maxDescriptorSetUniformBuffers      = 4;
    limits.maxPerStageDescriptorUniformBuffers = 3;

    EXPECT_FALSE(UniformBuffersFitLimits(usage, limits));
}

TEST(RHIFormatTests, AllDeclaredTexelFormatsHaveSizes)
{
    const DataFormat formats[] = {DataFormat::eR8UNORM,
                                  DataFormat::eR8UInt,
                                  DataFormat::eR8G8B8SRGB,
                                  DataFormat::eR8G8B8UNORM,
                                  DataFormat::eR8G8B8A8UInt,
                                  DataFormat::eR8G8B8A8SRGB,
                                  DataFormat::eR8G8B8A8UNORM,
                                  DataFormat::eB8G8R8A8UNORM,
                                  DataFormat::eB8G8R8A8SRGB,
                                  DataFormat::eR16UInt,
                                  DataFormat::eR16SInt,
                                  DataFormat::eR16SFloat,
                                  DataFormat::eR16G16UInt,
                                  DataFormat::eR16G16SInt,
                                  DataFormat::eR16G16SFloat,
                                  DataFormat::eR16G16B16UInt,
                                  DataFormat::eR16G16B16SInt,
                                  DataFormat::eR16G16B16SFloat,
                                  DataFormat::eR16G16B16A16UInt,
                                  DataFormat::eR16G16B16A16SInt,
                                  DataFormat::eR16G16B16A16SFloat,
                                  DataFormat::eR32UInt,
                                  DataFormat::eR32SInt,
                                  DataFormat::eR32SFloat,
                                  DataFormat::eR32G32UInt,
                                  DataFormat::eR32G32SInt,
                                  DataFormat::eR32G32SFloat,
                                  DataFormat::eR32G32B32UInt,
                                  DataFormat::eR32G32B32SInt,
                                  DataFormat::eR32G32B32SFloat,
                                  DataFormat::eR32G32B32A32UInt,
                                  DataFormat::eR32G32B32A32SInt,
                                  DataFormat::eR32G32B32A32SFloat,
                                  DataFormat::eR64UInt,
                                  DataFormat::eR64SInt,
                                  DataFormat::eR64SFloat,
                                  DataFormat::eR64G64UInt,
                                  DataFormat::eR64G64SInt,
                                  DataFormat::eR64G64SFloat,
                                  DataFormat::eR64G64B64UInt,
                                  DataFormat::eR64G64B64SInt,
                                  DataFormat::eR64G64B64SFloat,
                                  DataFormat::eR64G64B64A64UInt,
                                  DataFormat::eR64G64B64A64SInt,
                                  DataFormat::eR64G64B64A64SFloat,
                                  DataFormat::eD16UNORM,
                                  DataFormat::eD32SFloat,
                                  DataFormat::eS8UInt,
                                  DataFormat::eD16UNORMS8UInt,
                                  DataFormat::eD24UNORMS8UInt,
                                  DataFormat::eD32SFloatS8UInt};

    for (DataFormat format : formats)
    {
        EXPECT_GT(GetTextureFormatPixelSize(format), 0u) << uint32_t(format);
    }

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eUndefined), 0u);

    EXPECT_EQ(GetTextureFormatPixelSize(static_cast<DataFormat>(UINT32_MAX)), 0u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eB8G8R8A8UNORM), 4u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eB8G8R8A8SRGB), 4u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eD16UNORM), 2u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eD24UNORMS8UInt), 4u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eD16UNORMS8UInt), 3u);

    EXPECT_EQ(GetTextureFormatPixelSize(DataFormat::eD32SFloatS8UInt), 5u);

    EXPECT_EQ(GetTextureFormatMemoryPixelSize(DataFormat::eD16UNORMS8UInt), 4u);

    EXPECT_EQ(GetTextureFormatMemoryPixelSize(DataFormat::eD32SFloatS8UInt), 8u);

    EXPECT_EQ(GetTextureFormatMemoryPixelSize(DataFormat::eD24UNORMS8UInt), 4u);

    EXPECT_EQ(GetTextureFormatMemoryPixelSize(DataFormat::eR8G8B8A8UNORM), 4u);

    EXPECT_EQ(uint32_t(DataFormat::eR8G8B8UNORM), uint32_t(VK_FORMAT_R8G8B8_UNORM));
}

TEST(RHIHeaderTests, StageNamesCountsAndDefaults)
{
    using namespace zen;

    EXPECT_EQ(RHIShaderStageToString(RHIShaderStage::eFragment), "Fragment");

    EXPECT_EQ(RHIShaderStageToString(RHIShaderStage::eGeometry), "Geometry");

    EXPECT_TRUE(RHIShaderStageFlagToString({}).empty());

    EXPECT_EQ(RHIShaderStageFlagToString(
                  BitField<RHIShaderStageFlagBits>(RHIShaderStageFlagBits::eGeometry)),
              "Geometry");

    RHIShaderGroupSPIRV group;

    group.SetStageSPIRV(RHIShaderStage::eVertex, {});

    group.SetStageSPIRV(RHIShaderStage::eVertex, {});

    EXPECT_EQ(group.GetStageCount(), 1u);

    group.SetStageSPIRV(RHIShaderStage::eFragment, {});

    EXPECT_EQ(group.GetStageCount(), 2u);

    group.SetStageFlags(int64_t(RHIShaderStageFlagBits::eVertex) |
                        int64_t(RHIShaderStageFlagBits::eFragment));
    group.SetStageSPIRV(RHIShaderStage::eVertex, {});
    EXPECT_EQ(group.GetStageCount(), 2u);

    RHIGfxPipelineColorBlendState blend;

    blend.AddAttachments(MAX_NUM_COLOR_ATTACHMENTS);

    EXPECT_EQ(RHITextureTransition{}.oldUsage, RHITextureUsage::eNone);

    EXPECT_EQ(RHIBufferTransition{}.newAccessMode, RHIAccessMode::eNone);
}
