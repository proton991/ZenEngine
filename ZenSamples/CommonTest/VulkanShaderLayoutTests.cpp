#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
using namespace zen;

class VulkanShaderLayoutLimitTest : public testing::Test
{
protected:
    RHIShaderGroupInfo                                 info;
    VkPhysicalDeviceLimits                             limits{};
    VkPhysicalDeviceDescriptorIndexingProperties       indexing{};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration{};
    RHIBindlessHeapCapacities                          capacities;

    void SetUp() override
    {
        info.SRDTable.resize(3);
        capacities.slots.fill(0);
        limits.maxPushConstantsSize                                = 128;
        limits.maxUniformBufferRange                               = 65536;
        limits.maxDescriptorSetSamplers                            = 1024;
        limits.maxDescriptorSetUniformBuffers                      = 1024;
        limits.maxDescriptorSetUniformBuffersDynamic               = 1024;
        limits.maxDescriptorSetStorageBuffers                      = 1024;
        limits.maxDescriptorSetSampledImages                       = 1024;
        limits.maxDescriptorSetStorageImages                       = 1024;
        limits.maxDescriptorSetInputAttachments                    = 1024;
        limits.maxPerStageDescriptorSamplers                       = 1024;
        limits.maxPerStageDescriptorUniformBuffers                 = 1024;
        limits.maxPerStageDescriptorStorageBuffers                 = 1024;
        limits.maxPerStageDescriptorSampledImages                  = 1024;
        limits.maxPerStageDescriptorStorageImages                  = 1024;
        limits.maxPerStageDescriptorInputAttachments               = 1024;
        limits.maxPerStageResources                                = 4096;
        indexing.maxPerStageUpdateAfterBindResources               = 4096;
        indexing.maxDescriptorSetUpdateAfterBindSamplers           = 4096;
        indexing.maxDescriptorSetUpdateAfterBindSampledImages      = 4096;
        indexing.maxPerStageDescriptorUpdateAfterBindSamplers      = 4096;
        indexing.maxPerStageDescriptorUpdateAfterBindSampledImages = 4096;
        acceleration.maxDescriptorSetAccelerationStructures        = 1024;
        acceleration.maxPerStageDescriptorAccelerationStructures   = 1024;
    }

    RHIShaderResourceDescriptor& Add(RHIShaderResourceType  type,
                                     uint32_t               count,
                                     RHIShaderStageFlagBits stage = RHIShaderStageFlagBits::eFragment,
                                     uint32_t               set   = 1)
    {
        RHIShaderResourceDescriptor descriptor;
        descriptor.type      = type;
        descriptor.arraySize = count;
        descriptor.set       = set;
        descriptor.binding   = static_cast<uint32_t>(info.SRDTable[set].size());
        descriptor.stageFlags.SetFlag(stage);
        info.SRDTable[set].push_back(descriptor);
        return info.SRDTable[set].back();
    }

    bool Fits() const
    {
        return ShaderLayoutFitsLimits(info, limits, indexing, acceleration, capacities);
    }
};

struct DescriptorLimitCase
{
    RHIShaderResourceType type;
    uint32_t VkPhysicalDeviceLimits::* layoutLimit;
    uint32_t VkPhysicalDeviceLimits::* stageLimit;
};
} // namespace

TEST_F(VulkanShaderLayoutLimitTest, EveryOrdinaryDescriptorClassChecksLayoutAndStageArrayCounts)
{
    const DescriptorLimitCase cases[] = {
        {RHIShaderResourceType::eSampler, &VkPhysicalDeviceLimits::maxDescriptorSetSamplers,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSamplers},
        {RHIShaderResourceType::eTexture, &VkPhysicalDeviceLimits::maxDescriptorSetSampledImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
        {RHIShaderResourceType::eSamplerWithTexture, &VkPhysicalDeviceLimits::maxDescriptorSetSamplers,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSamplers},
        {RHIShaderResourceType::eSamplerWithTexture, &VkPhysicalDeviceLimits::maxDescriptorSetSampledImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
        {RHIShaderResourceType::eImage, &VkPhysicalDeviceLimits::maxDescriptorSetStorageImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageImages},
        {RHIShaderResourceType::eTextureBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetSampledImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
        {RHIShaderResourceType::eSamplerWithTextureBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetSampledImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorSampledImages},
        {RHIShaderResourceType::eImageBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetStorageImages,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageImages},
        {RHIShaderResourceType::eUniformBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetUniformBuffers,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers},
        {RHIShaderResourceType::eUniformBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetUniformBuffersDynamic,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorUniformBuffers},
        {RHIShaderResourceType::eStorageBuffer, &VkPhysicalDeviceLimits::maxDescriptorSetStorageBuffers,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers},
        {RHIShaderResourceType::eInputAttachment, &VkPhysicalDeviceLimits::maxDescriptorSetInputAttachments,
         &VkPhysicalDeviceLimits::maxPerStageDescriptorInputAttachments}};
    for (const DescriptorLimitCase& testCase : cases)
    {
        SCOPED_TRACE(ToUnderlying(testCase.type));
        info.SRDTable[1].clear();
        Add(testCase.type, 8);
        limits.*testCase.layoutLimit = 8;
        limits.*testCase.stageLimit  = 8;
        EXPECT_TRUE(Fits());
        limits.*testCase.layoutLimit = 7;
        EXPECT_FALSE(Fits());
        limits.*testCase.layoutLimit = 8;
        limits.*testCase.stageLimit  = 7;
        EXPECT_FALSE(Fits());
        limits.*testCase.layoutLimit = 1024;
        limits.*testCase.stageLimit  = 1024;
    }
}

TEST_F(VulkanShaderLayoutLimitTest, DynamicUniformElementsAggregateAcrossSetsAndVisibleStages)
{
    Add(RHIShaderResourceType::eUniformBuffer, 3).stageFlags.SetFlag(RHIShaderStageFlagBits::eVertex);
    Add(RHIShaderResourceType::eUniformBuffer, 1, RHIShaderStageFlagBits::eFragment, 2);
    limits.maxDescriptorSetUniformBuffersDynamic = 4;
    limits.maxDescriptorSetUniformBuffers        = 4;
    limits.maxPerStageDescriptorUniformBuffers   = 4;
    EXPECT_TRUE(Fits());
    limits.maxDescriptorSetUniformBuffersDynamic = 3;
    EXPECT_FALSE(Fits());
    limits.maxDescriptorSetUniformBuffersDynamic = 4;
    limits.maxDescriptorSetUniformBuffers        = 3;
    EXPECT_FALSE(Fits());
    limits.maxDescriptorSetUniformBuffers      = 4;
    limits.maxPerStageDescriptorUniformBuffers = 3;
    EXPECT_FALSE(Fits());
    info.SRDTable[2][0].stageFlags.Clear();
    info.SRDTable[2][0].stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    EXPECT_TRUE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, GlobalAndOrdinarySamplerAndImageBudgetsAreCombined)
{
    Add(RHIShaderResourceType::eSamplerWithTexture, 4);
    Add(RHIShaderResourceType::eSamplerWithTexture, 4, RHIShaderStageFlagBits::eVertex, 2);
    capacities.slots                = {4, 4, 8};
    limits.maxDescriptorSetSamplers = limits.maxDescriptorSetSampledImages = 8;
    limits.maxPerStageDescriptorSamplers = limits.maxPerStageDescriptorSampledImages = 8;
    indexing.maxDescriptorSetUpdateAfterBindSamplers = indexing.maxDescriptorSetUpdateAfterBindSampledImages = 16;
    indexing.maxPerStageDescriptorUpdateAfterBindSamplers = indexing.maxPerStageDescriptorUpdateAfterBindSampledImages = 12;
    EXPECT_TRUE(Fits()); // Set zero is absent from reflection, but present in the native layout.
    --indexing.maxDescriptorSetUpdateAfterBindSamplers;
    EXPECT_FALSE(Fits());
    ++indexing.maxDescriptorSetUpdateAfterBindSamplers;
    --indexing.maxDescriptorSetUpdateAfterBindSampledImages;
    EXPECT_FALSE(Fits());
    ++indexing.maxDescriptorSetUpdateAfterBindSampledImages;
    --indexing.maxPerStageDescriptorUpdateAfterBindSamplers;
    EXPECT_FALSE(Fits());
    ++indexing.maxPerStageDescriptorUpdateAfterBindSamplers;
    --indexing.maxPerStageDescriptorUpdateAfterBindSampledImages;
    EXPECT_FALSE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, CombinedBudgetsUseTheUpdateAfterBindLimitsAlone)
{
    Add(RHIShaderResourceType::eSamplerWithTexture, 4);
    capacities.slots = {4, 0, 4};
    EXPECT_TRUE(Fits());
    // Each value stays below its ordinary limit, which must not widen the native limit.
    indexing.maxDescriptorSetUpdateAfterBindSamplers = 7;
    EXPECT_FALSE(Fits());
    indexing.maxDescriptorSetUpdateAfterBindSamplers      = 8;
    indexing.maxPerStageDescriptorUpdateAfterBindSamplers = 7;
    EXPECT_FALSE(Fits());
    indexing.maxPerStageDescriptorUpdateAfterBindSamplers = 8;
    indexing.maxDescriptorSetUpdateAfterBindSampledImages = 7;
    EXPECT_FALSE(Fits());
    indexing.maxDescriptorSetUpdateAfterBindSampledImages      = 8;
    indexing.maxPerStageDescriptorUpdateAfterBindSampledImages = 7;
    EXPECT_FALSE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, StageResourceBudgetCombinesClassesWithoutDoubleCountingSamplers)
{
    Add(RHIShaderResourceType::eSamplerWithTexture, 3);
    Add(RHIShaderResourceType::eUniformBuffer, 2);
    Add(RHIShaderResourceType::eStorageBuffer, 1);
    Add(RHIShaderResourceType::eImageBuffer, 2);
    Add(RHIShaderResourceType::eInputAttachment, 1);
    Add(RHIShaderResourceType::eSampler, 32);
    Add(RHIShaderResourceType::eAccelerationStructure, 16);
    capacities.slots                             = {3, 1, 8};
    limits.maxPerStageResources                  = 9;
    indexing.maxPerStageUpdateAfterBindResources = 13;
    EXPECT_TRUE(Fits());
    limits.maxPerStageResources = 8;
    EXPECT_FALSE(Fits());
    limits.maxPerStageResources                  = 9;
    indexing.maxPerStageUpdateAfterBindResources = 12;
    EXPECT_FALSE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, PushConstantsAndUniformBlockRangeUseDeviceByteLimits)
{
    info.pushConstants.size                                 = 128;
    Add(RHIShaderResourceType::eUniformBuffer, 1).blockSize = limits.maxUniformBufferRange;
    EXPECT_TRUE(Fits());
    info.pushConstants.size = 132;
    EXPECT_FALSE(Fits());
    info.pushConstants.size = 128;
    ++info.SRDTable[1][0].blockSize;
    EXPECT_FALSE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, AccelerationStructureLimitsCountBindingsAcrossSetsAndStages)
{
    Add(RHIShaderResourceType::eAccelerationStructure, 16);
    Add(RHIShaderResourceType::eAccelerationStructure, 16, RHIShaderStageFlagBits::eFragment, 2);
    acceleration.maxDescriptorSetAccelerationStructures      = 2;
    acceleration.maxPerStageDescriptorAccelerationStructures = 2;
    EXPECT_TRUE(Fits());
    acceleration.maxDescriptorSetAccelerationStructures = 1;
    EXPECT_FALSE(Fits());
    acceleration.maxDescriptorSetAccelerationStructures      = 2;
    acceleration.maxPerStageDescriptorAccelerationStructures = 1;
    EXPECT_FALSE(Fits());
    info.SRDTable[2][0].stageFlags.Clear();
    info.SRDTable[2][0].stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    EXPECT_TRUE(Fits());
}

TEST_F(VulkanShaderLayoutLimitTest, DescriptorAndHeapSumsCannotWrapAtUint32Max)
{
    const uint32_t maximum                = std::numeric_limits<uint32_t>::max();
    limits.maxDescriptorSetStorageBuffers = limits.maxPerStageDescriptorStorageBuffers = limits.maxPerStageResources = maximum;
    indexing.maxPerStageUpdateAfterBindResources                                                                     = maximum;
    Add(RHIShaderResourceType::eStorageBuffer, maximum);
    EXPECT_TRUE(Fits());
    Add(RHIShaderResourceType::eStorageBuffer, 1, RHIShaderStageFlagBits::eFragment, 2);
    EXPECT_FALSE(Fits());
    info.SRDTable[1].clear();
    info.SRDTable[2].clear();
    limits.maxDescriptorSetSampledImages = limits.maxPerStageDescriptorSampledImages = maximum;
    indexing.maxDescriptorSetUpdateAfterBindSampledImages = indexing.maxPerStageDescriptorUpdateAfterBindSampledImages =
        maximum;
    indexing.maxPerStageUpdateAfterBindResources = maximum;
    capacities.slots                             = {maximum, 1, 0};
    EXPECT_FALSE(Fits());
}
