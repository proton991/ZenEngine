#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanBuffer.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDescriptorState.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
using namespace zen;

// Observe the Vulkan boundary while forwarding each operation to the actual driver.
struct DescriptorObserver
{
    static inline PFN_vkAllocateDescriptorSets allocate;
    static inline PFN_vkResetDescriptorPool reset;
    static inline PFN_vkCmdBindDescriptorSets bind;
    static inline PFN_vkUpdateDescriptorSets update;
    static inline std::unordered_map<VkDescriptorSet, VkDescriptorPool> owners;
    static inline std::unordered_set<VkDescriptorSet> freedSets;
    static inline VkDescriptorSet lastBound;
    static inline VkDescriptorBufferInfo lastBufferWrite;
    static inline uint32_t validationErrors;

    static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device,
                                                   const VkDescriptorSetAllocateInfo* info,
                                                   VkDescriptorSet* sets)
    {
        const VkResult result = allocate(device, info, sets);
        if (result == VK_SUCCESS)
        {
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i)
            {
                owners[sets[i]] = info->descriptorPool;
                freedSets.erase(sets[i]);
            }
        }
        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice device,
                                                VkDescriptorPool pool,
                                                VkDescriptorPoolResetFlags flags)
    {
        const VkResult result = reset(device, pool, flags);
        if (result == VK_SUCCESS)
        {
            for (const auto& [set, owner] : owners)
            {
                if (owner == pool)
                {
                    freedSets.insert(set);
                }
            }
        }
        return result;
    }

    static VKAPI_ATTR void VKAPI_CALL Bind(VkCommandBuffer commandBuffer,
                                           VkPipelineBindPoint point,
                                           VkPipelineLayout layout,
                                           uint32_t firstSet,
                                           uint32_t count,
                                           const VkDescriptorSet* sets,
                                           uint32_t offsetCount,
                                           const uint32_t* offsets)
    {
        if (firstSet <= test::kLocalResourceSet && test::kLocalResourceSet - firstSet < count)
        {
            lastBound = sets[test::kLocalResourceSet - firstSet];
        }
        bind(commandBuffer, point, layout, firstSet, count, sets, offsetCount, offsets);
    }

    static VKAPI_ATTR void VKAPI_CALL Update(VkDevice device,
                                             uint32_t count,
                                             const VkWriteDescriptorSet* writes,
                                             uint32_t copyCount,
                                             const VkCopyDescriptorSet* copies)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (writes[i].pBufferInfo != nullptr)
            {
                lastBufferWrite = writes[i].pBufferInfo[0];
            }
        }
        update(device, count, writes, copyCount, copies);
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ++validationErrors;
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }
};

class VulkanDescriptorIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    FVulkanCommandListContext* context{};
    std::vector<RHIBuffer*> buffers;
    std::vector<VulkanPipeline*> pipelines;
    std::vector<RHIShader*> shaders;
    VkDebugUtilsMessengerEXT messenger{};

    void SetUp() override
    {
        session = std::make_unique<test::VulkanSession>();
        DescriptorObserver::owners.clear();
        DescriptorObserver::freedSets.clear();
        DescriptorObserver::lastBound        = VK_NULL_HANDLE;
        DescriptorObserver::lastBufferWrite  = {};
        DescriptorObserver::validationErrors = 0;
        DescriptorObserver::allocate         = vkAllocateDescriptorSets;
        DescriptorObserver::reset            = vkResetDescriptorPool;
        DescriptorObserver::bind             = vkCmdBindDescriptorSets;
        DescriptorObserver::update           = vkUpdateDescriptorSets;
        vkAllocateDescriptorSets             = DescriptorObserver::Allocate;
        vkResetDescriptorPool                = DescriptorObserver::Reset;
        vkCmdBindDescriptorSets              = DescriptorObserver::Bind;
        vkUpdateDescriptorSets               = DescriptorObserver::Update;
        context                              = static_cast<FVulkanCommandListContext*>(
            session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = DescriptorObserver::Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
    }

    void TearDown() override
    {
        session->rhi.WaitDeviceIdle();
        ZEN_DELETE(context);
        for (VulkanPipeline* pipeline : pipelines)
        {
            session->rhi.DestroyPipeline(pipeline);
        }
        for (RHIShader* shader : shaders)
        {
            session->rhi.DestroyShader(shader);
        }
        for (RHIBuffer* buffer : buffers)
        {
            session->rhi.DestroyBuffer(buffer);
        }
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        session.reset();
        vkAllocateDescriptorSets = DescriptorObserver::allocate;
        vkResetDescriptorPool    = DescriptorObserver::reset;
        vkCmdBindDescriptorSets  = DescriptorObserver::bind;
        vkUpdateDescriptorSets   = DescriptorObserver::update;
        EXPECT_EQ(DescriptorObserver::validationErrors, 0u);
    }

    VulkanPipeline* Pipeline(const char* shaderFile)
    {
        RHIShaderCreateInfo info{};
        info.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
        info.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] =
            std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / shaderFile,
                                      SPV_SHADER_PATH)
                .generic_string();
        RHIShader* shader = session->rhi.CreateShader(info);
        shaders.push_back(shader);
        auto* pipeline = static_cast<VulkanPipeline*>(
            session->rhi.CreatePipeline(RHIComputePipelineCreateInfo{shader}));
        pipelines.push_back(pipeline);
        return pipeline;
    }

    RHIBuffer* Buffer(uint32_t size = 4, bool uniform = false)
    {
        RHIBufferCreateInfo info{};
        info.size = size;
        info.usageFlags.SetFlag(uniform ? RHIBufferUsageFlagBits::eUniformBuffer :
                                          RHIBufferUsageFlagBits::eStorageBuffer);
        info.allocateType = RHIBufferAllocateType::eCPURead;
        RHIBuffer* buffer = session->rhi.CreateBuffer(info);
        buffers.push_back(buffer);
        return buffer;
    }

    static RHIBatchedShaderParameters Parameters(VulkanPipeline* pipeline,
                                                 RHIBuffer* buffer,
                                                 uint32_t binding = 0,
                                                 uint32_t element = 0)
    {
        RHIBatchedShaderParameters params;
        params.AddResourceParam(
            *pipeline->GetShader()->GetSRDByLocation(test::kLocalResourceSet, binding), buffer,
            nullptr, element);
        return params;
    }

    VkDescriptorSet Resolve(VulkanDescriptorSetState& state, uint32_t* dynamicOffset = nullptr)
    {
        HeapVector<VkDescriptorSet> sets;
        HeapVector<uint32_t> offsets;
        uint32_t firstSet = 0;
        state.FlushPendingDescriptorWrites(context, sets, firstSet, offsets);
        EXPECT_EQ(firstSet, 0u);
        EXPECT_EQ(sets.size(), test::kLocalResourceSet + 1);
        if (dynamicOffset != nullptr)
        {
            EXPECT_EQ(offsets.size(), 1u);
            *dynamicOffset = offsets.empty() ? UINT32_MAX : offsets[0];
        }
        return sets.size() <= test::kLocalResourceSet ? VK_NULL_HANDLE :
                                                        sets[test::kLocalResourceSet];
    }

    void FillCache(VulkanPipeline* pipeline, uint32_t count)
    {
        VulkanDescriptorSetState other;
        other.SetPipeline(pipeline);
        for (uint32_t i = 0; i < count; ++i)
        {
            other.SetShaderParameters(Parameters(pipeline, Buffer()));
            EXPECT_NE(Resolve(other), VK_NULL_HANDLE);
        }
    }

    uint64_t Submit()
    {
        HeapVector<VulkanWorkload*> workloads;
        context->CollectWorkloads(workloads);
        VulkanQueue* queue = session->rhi.GetDevice()->GetGfxQueue();
        for (VulkanWorkload* workload : workloads)
        {
            queue->EnqueueWorkload(workload);
        }
        uint64_t serial = 0;
        EXPECT_EQ(queue->SubmitPendingWorkloads(serial), RHISubmissionResult::eSuccess);
        return serial;
    }

    void Complete(uint64_t serial)
    {
        EXPECT_TRUE(session->rhi.GetDevice()->GetGfxQueue()->WaitForCompletion(serial, UINT64_MAX));
        session->rhi.GetDescriptorPoolManager2()->BeginFrame(1);
    }
};

TEST_F(VulkanDescriptorIntegrationTest, CacheEvictionPreservesRecordedAndPendingDispatches)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_storage.comp.spv");
    context->RHIBindPipeline(pipeline);
    VkDescriptorSet first = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 2049; ++i)
    {
        context->RHISetShaderParameters(Parameters(pipeline, Buffer()));
        context->RHIDispatch(1, 1, 1);
        if (i == 0)
        {
            first = DescriptorObserver::lastBound;
        }
    }
    EXPECT_NE(first, VK_NULL_HANDLE);
    EXPECT_FALSE(DescriptorObserver::freedSets.contains(first));
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(context->GetCommandBuffer()->GetVkHandle(),
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &barrier, 0, nullptr, 0, nullptr);
    const uint64_t serial = Submit();
    {
        test::ScopedVulkanCall<PFN_vkGetSemaphoreCounterValue> holdTimeline(
            vkGetSemaphoreCounterValue, [](VkDevice, VkSemaphore, uint64_t* value) -> VkResult {
                *value = 0;
                return VK_SUCCESS;
            });
        test::ScopedVulkanCall<PFN_vkGetFenceStatus> holdFence(
            vkGetFenceStatus, [](VkDevice, VkFence) -> VkResult { return VK_NOT_READY; });
        session->rhi.GetDevice()->GetGfxQueue()->ProcessPendingWorkloads(0);
        session->rhi.GetDescriptorPoolManager2()->BeginFrame(1);
        EXPECT_FALSE(DescriptorObserver::freedSets.contains(first));
    }
    Complete(serial);
    EXPECT_TRUE(DescriptorObserver::freedSets.contains(first));
    for (RHIBuffer* buffer : buffers)
    {
        EXPECT_EQ(*reinterpret_cast<uint32_t*>(buffer->Map()), 0x1234u);
        buffer->Unmap();
    }
}

TEST_F(VulkanDescriptorIntegrationTest, UnchangedBindingsRetainPoolInNewWorkload)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_storage.comp.spv");
    VulkanDescriptorSetState state;
    state.SetPipeline(pipeline);
    state.SetShaderParameters(Parameters(pipeline, Buffer()));
    VkDescriptorSet first = Resolve(state);
    Complete(Submit());
    EXPECT_EQ(Resolve(state), first);
    FillCache(pipeline, 1536);
    EXPECT_FALSE(DescriptorObserver::freedSets.contains(first));
    Complete(Submit());
    EXPECT_TRUE(DescriptorObserver::freedSets.contains(first));
}

TEST_F(VulkanDescriptorIntegrationTest, EvictedResolvedStateRebuildsWithoutChangingParameters)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_storage.comp.spv");
    VulkanDescriptorSetState state;
    state.SetPipeline(pipeline);
    state.SetShaderParameters(Parameters(pipeline, Buffer()));
    VkDescriptorSet first = Resolve(state);
    FillCache(pipeline, 1536);
    VkDescriptorSet rebuilt = Resolve(state);
    EXPECT_NE(rebuilt, VK_NULL_HANDLE);
    EXPECT_NE(rebuilt, first);
    EXPECT_FALSE(DescriptorObserver::freedSets.contains(rebuilt));
}

TEST_F(VulkanDescriptorIntegrationTest, SparseArrayPositionsAndLengthsCannotAlias)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_array.comp.spv");
    RHIBuffer* a             = Buffer();
    RHIBuffer* b             = Buffer();
    auto resolveArray        = [&](uint32_t aIndex, uint32_t bIndex) {
        VulkanDescriptorSetState state;
        state.SetPipeline(pipeline);
        state.SetShaderParameters(Parameters(pipeline, a, 0, aIndex));
        state.SetShaderParameters(Parameters(pipeline, b, 0, bIndex));
        return Resolve(state);
    };
    VkDescriptorSet sparse = resolveArray(0, 2);
    EXPECT_NE(sparse, resolveArray(0, 1));
    EXPECT_NE(sparse, resolveArray(1, 2));
    EXPECT_EQ(sparse, resolveArray(0, 2));
}

TEST_F(VulkanDescriptorIntegrationTest, ParameterInsertionOrderDoesNotChangeCachedSet)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_bindings.comp.spv");
    RHIBuffer* a             = Buffer();
    RHIBuffer* b             = Buffer();
    VulkanDescriptorSetState left, right;
    left.SetPipeline(pipeline);
    right.SetPipeline(pipeline);
    left.SetShaderParameters(Parameters(pipeline, a, 2));
    left.SetShaderParameters(Parameters(pipeline, b, 7));
    right.SetShaderParameters(Parameters(pipeline, b, 7));
    right.SetShaderParameters(Parameters(pipeline, a, 2));
    EXPECT_EQ(Resolve(left), Resolve(right));
}

TEST_F(VulkanDescriptorIntegrationTest, ResourceIdentityDistinguishesCachedSets)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_storage.comp.spv");
    RHIBuffer* buffer        = Buffer();
    RHIBuffer* replacement   = Buffer();
    EXPECT_NE(buffer->GetStableId(), replacement->GetStableId());
    VulkanDescriptorSetState state;
    state.SetPipeline(pipeline);
    state.SetShaderParameters(Parameters(pipeline, buffer));
    const VkDescriptorSet first = Resolve(state);
    state.SetShaderParameters(Parameters(pipeline, buffer));
    EXPECT_EQ(Resolve(state), first);
    state.SetShaderParameters(Parameters(pipeline, replacement));
    EXPECT_NE(Resolve(state), first);
    state.SetShaderParameters(Parameters(pipeline, buffer));
    EXPECT_EQ(Resolve(state), first);
}

TEST_F(VulkanDescriptorIntegrationTest, UniformRangeDistinguishesOtherwiseCompatibleSets)
{
    VulkanPipeline* smallPipeline = Pipeline("descriptor_uniform.comp.spv");
    VulkanPipeline* largePipeline = Pipeline("descriptor_uniform_large.comp.spv");
    RHIBuffer* buffer             = Buffer(32, true);
    VulkanDescriptorSetState state;
    state.SetPipeline(smallPipeline);
    state.SetShaderParameters(Parameters(smallPipeline, buffer));
    VkDescriptorSet first = Resolve(state);
    EXPECT_EQ(DescriptorObserver::lastBufferWrite.range, 16u);
    state.SetPipeline(largePipeline);
    state.SetShaderParameters(Parameters(largePipeline, buffer));
    EXPECT_NE(Resolve(state), first);
    EXPECT_EQ(DescriptorObserver::lastBufferWrite.range, 32u);
}

TEST_F(VulkanDescriptorIntegrationTest, PackedOffsetsAreBindTimeStateAndExternalBindingResetsThem)
{
    VulkanPipeline* pipeline = Pipeline("descriptor_uniform.comp.spv");
    VulkanDescriptorSetState state;
    state.SetPipeline(pipeline);
    const uint32_t values[] = {1, 2, 3, 4};
    RHIBatchedShaderParameters packed;
    packed.AddValueParam(*pipeline->GetShader()->GetSRDByLocation(test::kLocalResourceSet, 0),
                         values, sizeof(values));
    state.SetShaderParameters(packed);
    uint32_t firstOffset = 0, nextOffset = 0;
    VkDescriptorSet first = Resolve(state, &firstOffset);
    state.SetShaderParameters(packed);
    EXPECT_EQ(Resolve(state, &nextOffset), first);
    EXPECT_GT(nextOffset, firstOffset);
    RHIBuffer* external = Buffer(64, true);
    state.SetShaderParameters(packed);
    state.SetShaderParameters(Parameters(pipeline, external));
    uint32_t externalOffset = UINT32_MAX;
    EXPECT_NE(Resolve(state, &externalOffset), first);
    EXPECT_EQ(externalOffset, 0u);
    EXPECT_EQ(DescriptorObserver::lastBufferWrite.buffer,
              static_cast<VulkanBuffer*>(external)->GetVkBuffer());
    EXPECT_EQ(DescriptorObserver::lastBufferWrite.range, 16u);
    state.SetShaderParameters(packed);
    EXPECT_EQ(Resolve(state, &nextOffset), first);
    EXPECT_GT(nextOffset, firstOffset);
}
} // namespace
