#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanDescriptorPool.h"
#include "Graphics/VulkanRHI/VulkanPipeline.h"
#include <gtest/gtest.h>
#include <limits>

namespace
{
using namespace zen;

struct RejectedNativeCreation
{
    static inline uint32_t modules;
    static inline uint32_t setLayouts;
    static inline uint32_t pipelineLayouts;
    static inline uint32_t pipelines;

    static VKAPI_ATTR VkResult VKAPI_CALL Module(VkDevice,
                                                 const VkShaderModuleCreateInfo*,
                                                 const VkAllocationCallbacks*,
                                                 VkShaderModule*)
    {
        ++modules;
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL SetLayout(VkDevice,
                                                    const VkDescriptorSetLayoutCreateInfo*,
                                                    const VkAllocationCallbacks*,
                                                    VkDescriptorSetLayout*)
    {
        ++setLayouts;
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL PipelineLayout(VkDevice,
                                                         const VkPipelineLayoutCreateInfo*,
                                                         const VkAllocationCallbacks*,
                                                         VkPipelineLayout*)
    {
        ++pipelineLayouts;
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Graphics(VkDevice,
                                                   VkPipelineCache,
                                                   uint32_t,
                                                   const VkGraphicsPipelineCreateInfo*,
                                                   const VkAllocationCallbacks*,
                                                   VkPipeline*)
    {
        ++pipelines;
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
};

void ExpectShaderRejectedBeforeNativeCreation(test::VulkanSession& session, const RHIShaderCreateInfo& info)
{
    RejectedNativeCreation::modules         = 0;
    RejectedNativeCreation::setLayouts      = 0;
    RejectedNativeCreation::pipelineLayouts = 0;
    test::ScopedVulkanCall<PFN_vkCreateShaderModule>        module(vkCreateShaderModule, RejectedNativeCreation::Module);
    test::ScopedVulkanCall<PFN_vkCreateDescriptorSetLayout> setLayout(vkCreateDescriptorSetLayout,
                                                                      RejectedNativeCreation::SetLayout);
    test::ScopedVulkanCall<PFN_vkCreatePipelineLayout>      pipelineLayout(vkCreatePipelineLayout,
                                                                           RejectedNativeCreation::PipelineLayout);
    RHIShader*                                              shader = session.rhi.CreateShader(info);
    EXPECT_EQ(shader, nullptr);
    EXPECT_EQ(RejectedNativeCreation::modules, 0u);
    EXPECT_EQ(RejectedNativeCreation::setLayouts, 0u);
    EXPECT_EQ(RejectedNativeCreation::pipelineLayouts, 0u);
    if (shader != nullptr)
    {
        session.rhi.DestroyShader(shader);
    }
}

class ScopedFragmentDescriptors
{
public:
    ScopedFragmentDescriptors(RHIShader* shader, uint32_t resourceCount) :
        table(*const_cast<RHIShaderResourceDescriptorTable*>(shader->GetSRDTable())), previous(table)
    {
        table.clear();
        table.resize(2);
        RHIShaderResourceDescriptor descriptor;
        descriptor.type = RHIShaderResourceType::eTexture;
        descriptor.set  = 1;
        descriptor.stageFlags.SetFlag(RHIShaderStageFlagBits::eFragment);
        table[1].push_back(descriptor);
        SetCount(resourceCount);
    }

    ~ScopedFragmentDescriptors()
    {
        table = std::move(previous);
    }

    void SetCount(uint32_t resourceCount)
    {
        table[1][0].arraySize = resourceCount;
    }

private:
    RHIShaderResourceDescriptorTable& table;
    RHIShaderResourceDescriptorTable  previous;
};
} // namespace

TEST(VulkanShaderLayoutIntegrationTest, TessellationCapabilityAndStagesAreExplicitlyUnsupported)
{
    test::VulkanSession session;
    EXPECT_FALSE(session.rhi.QueryGPUInfo().supportTessellationShader);
    EXPECT_EQ(session.rhi.GetDevice()->GetPhysicalDeviceFeatures().tessellationShader, VK_FALSE);
    RHIShaderCreateInfo control;
    test::SetShaderStage(control, RHIShaderStage::eTesselationControl, "unsupported_tessellation.tesc.spv");
    ExpectShaderRejectedBeforeNativeCreation(session, control);
    RHIShaderCreateInfo evaluation;
    test::SetShaderStage(evaluation, RHIShaderStage::eTesselationEvaluation, "unsupported_tessellation.tese.spv");
    ExpectShaderRejectedBeforeNativeCreation(session, evaluation);
}

TEST(VulkanShaderLayoutIntegrationTest, PatchTopologyIsRejectedBeforeNativeGraphicsCreation)
{
    test::VulkanSession session;
    RHIShaderCreateInfo shaderInfo;
    test::SetShaderStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");
    test::SetShaderStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");
    RHIShader* shader = session.rhi.CreateShader(shaderInfo);
    ASSERT_NE(shader, nullptr);
    RHIRenderingLayout layout;
    layout.SetRenderArea(0, 0, 8, 8);
    layout.numColorRenderTargets        = 1;
    layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8UNORM;
    RHIGfxPipelineCreateInfo info;
    info.pShader                      = shader;
    info.pRenderingLayout             = &layout;
    info.states.primitiveType         = RHIDrawPrimitiveType::ePatchList;
    RejectedNativeCreation::pipelines = 0;
    {
        test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> pipeline(vkCreateGraphicsPipelines,
                                                                       RejectedNativeCreation::Graphics);
        RHIPipeline*                                          rejected = session.rhi.CreatePipeline(info);
        EXPECT_EQ(rejected, nullptr);
        EXPECT_EQ(RejectedNativeCreation::pipelines, 0u);
        if (rejected != nullptr)
        {
            session.rhi.DestroyPipeline(rejected);
        }
    }
    info.states.primitiveType = RHIDrawPrimitiveType::eTriangleList;
    RHIPipeline* valid        = session.rhi.CreatePipeline(info);
    EXPECT_NE(valid, nullptr);
    if (valid != nullptr)
    {
        session.rhi.DestroyPipeline(valid);
    }
    session.rhi.DestroyShader(shader);
}

TEST(VulkanShaderLayoutIntegrationTest, DeviceLayoutLimitsRejectSpecializationMetadataBeforeNativeCreation)
{
    test::VulkanSession session;
    RHIShaderCreateInfo source;
    test::SetShaderStage(source, RHIShaderStage::eCompute, "pipeline_specialization.comp.spv");
    RHIShader* original = session.rhi.CreateShader(source);
    ASSERT_NE(original, nullptr);
    const RHIShaderCreateInfo reusable = original->GetCreateInfo();
    ASSERT_TRUE(reusable.reflection.has_value());
    const VkPhysicalDeviceLimits limits = session.rhi.GetDevice()->GetPhysicalDeviceProperties().limits;

    RHIShaderCreateInfo pushConstants   = reusable;
    ASSERT_LE(limits.maxPushConstantsSize, std::numeric_limits<uint32_t>::max() - 4);
    pushConstants.reflection->pushConstants.size = limits.maxPushConstantsSize + 4;
    pushConstants.reflection->pushConstants.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    ExpectShaderRejectedBeforeNativeCreation(session, pushConstants);

    struct DescriptorCase
    {
        RHIShaderResourceType type;
        uint32_t              limit;
    };
    const DescriptorCase cases[] = {
        {RHIShaderResourceType::eTexture,
         std::min(limits.maxDescriptorSetSampledImages, limits.maxPerStageDescriptorSampledImages)},
        {RHIShaderResourceType::eImage,
         std::min(limits.maxDescriptorSetStorageImages, limits.maxPerStageDescriptorStorageImages)},
        {RHIShaderResourceType::eStorageBuffer,
         std::min(limits.maxDescriptorSetStorageBuffers, limits.maxPerStageDescriptorStorageBuffers)},
        {RHIShaderResourceType::eInputAttachment,
         std::min(limits.maxDescriptorSetInputAttachments, limits.maxPerStageDescriptorInputAttachments)}};
    for (const DescriptorCase& testCase : cases)
    {
        SCOPED_TRACE(ToUnderlying(testCase.type));
        ASSERT_LT(testCase.limit, std::numeric_limits<uint32_t>::max());
        RHIShaderCreateInfo          overLimit  = reusable;
        RHIShaderResourceDescriptor& descriptor = overLimit.reflection->SRDTable[1][0];
        descriptor.type                         = testCase.type;
        descriptor.arraySize                    = testCase.limit + 1;
        ExpectShaderRejectedBeforeNativeCreation(session, overLimit);
    }

    // Reusing unchanged reflection remains valid after all the rejected attempts.
    RHIShader* valid = session.rhi.CreateShader(reusable);
    EXPECT_NE(valid, nullptr);
    if (valid != nullptr)
    {
        session.rhi.DestroyShader(valid);
    }
    session.rhi.DestroyShader(original);
}

TEST(VulkanShaderLayoutIntegrationTest, FragmentColorAttachmentCountsBeforeNativeGraphicsCreation)
{
    test::VulkanSession session;
    RHIShaderCreateInfo shaderInfo;
    test::SetShaderStage(shaderInfo, RHIShaderStage::eVertex, "pipeline.vert.spv");
    test::SetShaderStage(shaderInfo, RHIShaderStage::eFragment, "pipeline.frag.spv");
    RHIShader* shader = session.rhi.CreateShader(shaderInfo);
    ASSERT_NE(shader, nullptr);
    const VulkanDevice*              device       = session.rhi.GetDevice();
    const RHIBindlessHeapCapacities& capacities   = session.rhi.GetBindlessDescriptorPoolManager()->GetCapacities();
    const uint64_t                   globalImages = capacities.GetImageSlots();
    const uint32_t aggregateLimit = device->GetDescriptorIndexingProperties().maxPerStageUpdateAfterBindResources;
    ASSERT_GT(aggregateLimit, globalImages);
    const uint32_t resourceBudget = std::min(device->GetPhysicalDeviceProperties().limits.maxPerStageResources,
                                             static_cast<uint32_t>(aggregateLimit - globalImages));
    ASSERT_GT(resourceBudget, 0u);
    RHIRenderingLayout layout;
    layout.SetRenderArea(0, 0, 8, 8);
    layout.numColorRenderTargets        = 1;
    layout.colorRenderTargets[0].format = DataFormat::eR8G8B8A8UNORM;
    RHIGfxPipelineCreateInfo info;
    info.pShader          = shader;
    info.pRenderingLayout = &layout;

    // Exercise device-dependent count boundaries without allocating a huge native shader layout.
    // The native shader stays valid; restore its reflected table before destruction.
    {
        ScopedFragmentDescriptors descriptors(shader, resourceBudget);
        RejectedNativeCreation::pipelines = 0;
        {
            test::ScopedVulkanCall<PFN_vkCreateGraphicsPipelines> pipeline(vkCreateGraphicsPipelines,
                                                                           RejectedNativeCreation::Graphics);
            RHIPipeline*                                          rejected = session.rhi.CreatePipeline(info);
            EXPECT_EQ(rejected, nullptr);
            EXPECT_EQ(RejectedNativeCreation::pipelines, 0u);
            if (rejected != nullptr)
            {
                session.rhi.DestroyPipeline(rejected);
            }
        }
        descriptors.SetCount(resourceBudget - 1);
        RHIPipeline* boundary = session.rhi.CreatePipeline(info);
        EXPECT_NE(boundary, nullptr);
        if (boundary != nullptr)
        {
            session.rhi.DestroyPipeline(boundary);
        }
    }
    session.rhi.DestroyShader(shader);
}
