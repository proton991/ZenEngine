#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include "Graphics/VulkanRHI/VulkanTypes.h"
#include "Graphics/VulkanRHI/VulkanResourceSharing.h"
#include "Graphics/VulkanRHI/VulkanCopyCapabilities.h"
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

namespace zen
{
// Exercise the production descriptor write path without a Vulkan device or allocator.
struct VulkanDescriptorStateTestAccess
{
    struct Shader : RHIShader
    {
        Shader() : RHIShader(RHIShaderCreateInfo{})
        {
            m_SRDTable.resize(1);
            RHIShaderResourceDescriptor descriptor{};
            descriptor.type      = RHIShaderResourceType::eUniformBuffer;
            descriptor.arraySize = 1;
            descriptor.blockSize = 16;
            m_SRDTable[0].push_back(descriptor);
        }

        void Init() override {}

        void Destroy() override {}
    };
    struct Pipeline : VulkanPipeline
    {
        explicit Pipeline(const RHIComputePipelineCreateInfo& info) : VulkanPipeline(info) {}

        void Destroy() override {}
    };
    struct Buffer : RHIBuffer
    {
        Buffer() : RHIBuffer(RHIBufferCreateInfo{}) {}

        uint8_t* Map() override
        {
            return nullptr;
        }

        void Unmap() override {}

        void SetTexelFormat(DataFormat) override {}

        void Init() override {}

        void Destroy() override {}
    };

    static void VerifyUniformSwitch()
    {
        Shader shader;

        RHIComputePipelineCreateInfo info{};
        info.pShader = &shader;
        Pipeline pipeline(info);
        Buffer buffer;
        VulkanDescriptorSetState state;
        state.SetPipeline(&pipeline);
        zen::RHIShaderResourceDescriptor const& descriptor = (*shader.GetSRDTable())[0][0];
        const uint32_t values[4]                           = {1, 2, 3, 4};
        RHIBatchedShaderParameters packed;
        packed.AddValueParam(descriptor, values, sizeof(values));
        state.SetShaderParameters(packed);
        zen::VulkanDescriptorSetState::BindingState& binding =
            state.FindOrAddBinding(state.m_setStates[0], 0, RHIShaderResourceType::eUniformBuffer);
        binding.dynamicOffset = 256; // The prior draw used a packed allocation.
        binding.valueRange    = 64;
        RHIBatchedShaderParameters external;
        external.AddResourceParam(descriptor, &buffer, nullptr, 0);
        state.SetShaderParameters(external);

        EXPECT_EQ(binding.dynamicOffset, 0u);
        EXPECT_EQ(binding.valueRange, 16u);
        ASSERT_EQ(binding.srb.resources.size(), 1u);
        EXPECT_EQ(binding.srb.resources[0], &buffer);
        ASSERT_EQ(state.m_packedValueBuffers.size(), 1u);
        EXPECT_FALSE(state.m_packedValueBuffers[0].dirty);

        state.SetShaderParameters(packed); // Switching back still schedules a fresh packed upload.

        EXPECT_TRUE(state.m_packedValueBuffers[0].dirty);

        state.Reset();
        buffer.ReleaseReference();
        pipeline.ReleaseReference();
        shader.ReleaseReference();
    }
};
} // namespace zen

TEST(VulkanResourceCreationTests, PhysicalUniformBindingClearsPriorPackedOffsetsAndPendingWrites)
{
    zen::VulkanDescriptorStateTestAccess::VerifyUniformSwitch();
}
