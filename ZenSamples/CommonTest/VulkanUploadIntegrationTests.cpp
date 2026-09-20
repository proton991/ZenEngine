#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "ScopedVulkanCall.h"
#include "VulkanIntegrationFixture.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <tuple>
#include <filesystem>

namespace
{
using namespace zen;

class VulkanUploadIntegrationTest :
    public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{
protected:
    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_TRUE;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 uint32_t count,
                                                 const VkSubmitInfo* infos,
                                                 VkFence fence)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            gpuWaitCount += infos[i].waitSemaphoreCount;
            if (infos[i].waitSemaphoreCount != 0)
            {
                const VkTimelineSemaphoreSubmitInfo* timeline =
                    static_cast<const VkTimelineSemaphoreSubmitInfo*>(infos[i].pNext);
                if (timeline != nullptr &&
                    timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                {
                    for (uint32_t j = 0; j < timeline->waitSemaphoreValueCount; ++j)
                    {
                        waitValues.push_back(timeline->pWaitSemaphoreValues[j]);
                        EXPECT_EQ(infos[i].pWaitDstStageMask[j],
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    }
                }
            }
        }
        return originalSubmit(queue, count, infos, fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice device,
                                               const VkSemaphoreWaitInfo* info,
                                               uint64_t timeout)
    {
        ++hostWaitCount;
        return originalWait(device, info, timeout);
    }

    void SetUp() override
    {
        backend = static_cast<VulkanRHI*>(DynamicRHI::Create(RHIAPIType::eVulkan));
        if (std::get<1>(GetParam()))
        {
            ASSERT_TRUE(backend->SupportsAsyncSubmissionDependencies());
        }
        else
        {
            backend->GetDevice()->GetExtensionFlags().hasTimelineSemaphore = 0;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(backend->GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
        executor    = ZEN_NEW() RHICommandListExecutor(backend, std::get<0>(GetParam()));
        GDynamicRHI = executor;
        GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, 3);
        source      = Buffer(RHIBufferAllocateType::eCPUWrite);
        destination = Buffer(RHIBufferAllocateType::eGPU);
        readback    = Buffer(RHIBufferAllocateType::eCPURead);
        for (uint32_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = uint8_t(i * 3 + 7);
        }
        uint8_t* mapped = source->Map();
        ASSERT_NE(mapped, nullptr);
        std::memcpy(mapped, bytes.data(), bytes.size());
        source->Unmap();
        originalSubmit = vkQueueSubmit;
        originalWait   = vkWaitSemaphores;
        gpuWaitCount   = 0;
        hostWaitCount  = 0;
        waitValues.clear();
    }

    void TearDown() override
    {
        if (executor != nullptr)
        {
            executor->WaitDeviceIdle();
            for (RHIBuffer* buffer : {source, destination, readback})
            {
                if (buffer != nullptr)
                {
                    buffer->ReleaseReference();
                }
            }
            vkDestroyDebugUtilsMessengerEXT(backend->GetInstance(), messenger, nullptr);
            executor->Destroy();
            ZEN_DELETE(executor);
        }
        else if (backend != nullptr)
        {
            backend->Destroy();
            ZEN_DELETE(backend);
        }
        GDynamicRHI = nullptr;
        GVulkanRHI  = nullptr;
        // Release the static capture buffer before the allocator's shutdown report.
        waitValues = {};
    }

    RHIBuffer* Buffer(RHIBufferAllocateType allocation, bool storage = false)
    {
        RHIBufferCreateInfo info{};
        info.size         = bytes.size();
        info.allocateType = allocation;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                 RHIBufferUsageFlagBits::eTransferDstBuffer);
        if (storage)
        {
            info.usageFlags.SetFlag(RHIBufferUsageFlagBits::eStorageBuffer);
        }
        return executor->CreateBuffer(info);
    }

    RHISubmissionResult SubmitCommands(RHICommandList& commands)
    {
        RHICommandList* list = &commands;
        return executor->SubmitBatch(MakeVecView(&list, 1));
    }

    VulkanRHI* backend{nullptr};
    RHICommandListExecutor* executor{nullptr};
    VkDebugUtilsMessengerEXT messenger{VK_NULL_HANDLE};
    RHIBuffer* source{nullptr};
    RHIBuffer* destination{nullptr};
    RHIBuffer* readback{nullptr};
    std::array<uint8_t, 64> bytes{};
    static inline PFN_vkQueueSubmit originalSubmit;
    static inline PFN_vkWaitSemaphores originalWait;
    static inline uint32_t gpuWaitCount{0};
    static inline uint32_t hostWaitCount{0};
    static inline HeapVector<uint64_t> waitValues;
};

TEST_P(VulkanUploadIntegrationTest, TransferFeedsGraphicsThroughSubmissionDependency)
{
    RHICommandListPtr upload(RHICommandList::Create(executor->GetTransferCommandContext()));
    upload->CopyBuffer(source, destination, {0, 0, bytes.size()});
    ASSERT_EQ(SubmitCommands(*upload), RHISubmissionResult::eSuccess);
    const uint64_t transfer = executor->GetLastSubmittedSerial(RHICommandContextType::eTransfer);

    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);
    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);
    RHICommandListPtr graphics(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    // Multiple resources in a batch can depend on the same queue value; emit one native wait.
    graphics->AddSubmissionDependency({RHICommandContextType::eTransfer, transfer});
    graphics->AddSubmissionDependency(
        {RHICommandContextType::eTransfer, RHISubmissionDependency::kLatestSubmitted});
    RHIBufferTransition transition{RHIAccessMode::eReadWrite, RHIAccessMode::eRead, destination,
                                   RHIBufferUsage::eTransferDst, RHIBufferUsage::eTransferSrc};
    const BitField<RHIPipelineStageFlagBits> transferStage(RHIPipelineStageFlagBits::eTransfer);
    graphics->AddTransitions(transferStage, transferStage, {}, MakeVecView(&transition, 1), {});
    graphics->CopyBuffer(destination, readback, {0, 0, bytes.size()});
    const RHIBatchResult result = executor->SubmitFrame(*graphics, nullptr).Wait();
    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
    const bool async = std::get<1>(GetParam()) && !executor->IsTransferQueueSharedWithGraphics();
    EXPECT_EQ(gpuWaitCount, async ? 1u : 0u);
    if (async)
    {
        EXPECT_EQ(hostWaitCount, 0u);
        ASSERT_EQ(waitValues.size(), 1u);
        EXPECT_EQ(waitValues[0], transfer);
    }
    ASSERT_TRUE(executor->WaitForSubmission(
        RHICommandContextType::eGraphics, result.completion.Get(RHICommandContextType::eGraphics)));
    uint8_t* mapped = readback->Map();
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);
    readback->Unmap();
}

TEST_P(VulkanUploadIntegrationTest, ExactComputeProducerSurvivesInterveningComputeSubmission)
{
    const RHIQueueCapabilities queues = executor->GetQueueCapabilities();
    if (!queues.computeSupported ||
        queues.AreQueuesShared(RHICommandContextType::eGraphics,
                               RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "A native compute queue distinct from graphics is required";
    }
    const RHICommandContextType queue     = RHICommandContextType::eAsyncCompute;
    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queue));
    RHICommandListPtr compute(RHICommandList::Create(executor->GetCommandContext(queue)));
    RHICommandListPtr graphics(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    RHIResourcePtr<RHIBuffer> scratch(Buffer(RHIBufferAllocateType::eGPU), false);
    compute->CopyBuffer(source, destination, {0, 0, bytes.size()});
    const RHISubmissionTicket producer = executor->SubmitFrame(*compute, nullptr, state);
    compute->ClearBuffer(scratch.Get(), 0, bytes.size());
    const RHISubmissionTicket unrelated = executor->SubmitFrame(*compute, nullptr);
    ASSERT_EQ(producer.Wait().submission, RHISubmissionResult::eSuccess);
    ASSERT_EQ(unrelated.Wait().submission, RHISubmissionResult::eSuccess);
    const RHISubmissionPoint point{queue, 0, state, 0};
    RHISubmissionDependency accepted;
    ASSERT_EQ(point.Resolve(accepted), RHISubmissionPointStatus::eAccepted);
    EXPECT_LT(accepted.serial, executor->GetLastSubmittedSerial(queue));
    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);
    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);
    graphics->CopyBuffer(destination, readback, {0, 0, bytes.size()});
    const RHIBatchResult result =
        executor->SubmitFrame(*graphics, nullptr, {}, MakeVecView(point)).Wait();
    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
    const bool timeline = std::get<1>(GetParam());
    EXPECT_EQ(gpuWaitCount, timeline ? 1u : 0u);
    if (timeline)
    {
        EXPECT_EQ(hostWaitCount, 0u);
        ASSERT_EQ(waitValues.size(), 1u);
        EXPECT_EQ(waitValues[0], accepted.serial);
    }
    ASSERT_TRUE(executor->WaitForSubmission(
        RHICommandContextType::eGraphics, result.completion.Get(RHICommandContextType::eGraphics)));
    uint8_t* mapped = readback->Map();
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);
    readback->Unmap();
    // Unrelated work is not covered by the graphics dependency.
    executor->WaitDeviceIdle();
}

TEST_P(VulkanUploadIntegrationTest, OwnedScheduleDispatchesBetweenProducerAndGraphicsReadback)
{
    const RHIQueueCapabilities queues = executor->GetQueueCapabilities();
    if (!queues.computeSupported ||
        queues.AreQueuesShared(RHICommandContextType::eGraphics,
                               RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "Requires a native compute queue distinct from graphics";
    }
    RHIShaderCreateInfo shaderInfo{};
    shaderInfo.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);
    shaderInfo.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] =
        std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) /
                                      "async_resources.comp.spv",
                                  SPV_SHADER_PATH)
            .generic_string();
    RHIResourcePtr<RHIShader> shader(executor->CreateShader(shaderInfo), false);
    RHIResourcePtr<RHIPipeline> pipeline(
        executor->CreatePipeline(RHIComputePipelineCreateInfo{shader.Get()}), false);
    for (RHICommandContextType producerQueue :
         {RHICommandContextType::eGraphics, RHICommandContextType::eTransfer})
    {
        RHIResourcePtr<RHIBuffer> data(Buffer(RHIBufferAllocateType::eGPU, true), false);
        RHIResourcePtr<RHIBuffer> output(Buffer(RHIBufferAllocateType::eGPU, true), false);
        RHIResourcePtr<RHIBuffer> scratch(Buffer(RHIBufferAllocateType::eGPU), false);
        RHITextureCreateInfo textureInfo{};
        textureInfo.type  = RHITextureType::e2D;
        textureInfo.width = textureInfo.height = 1;
        textureInfo.format                     = DataFormat::eR32UInt;
        textureInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage,
                                        RHITextureUsageFlagBits::eTransferDst);
        RHIResourcePtr<RHITexture> texture(executor->CreateTexture(textureInfo), false);
        const SmallVector<RHICommandContextType, 4> contexts{
            RHICommandContextType::eGraphics, producerQueue, RHICommandContextType::eAsyncCompute,
            RHICommandContextType::eGraphics};
        RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(contexts);
        HeapVector<RHICommandListPtr> lists;
        HeapVector<RHISubmissionGroup> groups;
        for (RHICommandContextType queue : contexts)
        {
            lists.emplace_back(RHICommandList::Create(executor->GetCommandContext(queue)));
            groups.push_back({lists.back().get()});
        }
        lists[0]->ClearBuffer(scratch.Get(), 0, 64); // Independent prefix has no compute wait.
        lists[1]->CopyBuffer(source, data.Get(), {0, 0, bytes.size()});
        const RHITextureSubResourceRange range = texture->GetSubResourceRange();
        RHITextureTransition clear{};
        clear.pTexture         = texture.Get();
        clear.newUsage         = RHITextureUsage::eTransferDst;
        clear.newAccessMode    = RHIAccessMode::eReadWrite;
        clear.subResourceRange = range;
        const BitField<RHIPipelineStageFlagBits> transfer(RHIPipelineStageFlagBits::eTransfer);
        lists[1]->AddTransitions(
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTopOfPipe), transfer, {},
            {}, MakeVecView(clear));
        RHIBufferTextureCopyRegion imageCopy{};
        imageCopy.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        imageCopy.textureSubresources.layerCount = 1;
        imageCopy.textureSize                    = {1, 1, 1};
        lists[1]->CopyBufferToTexture(source, texture.Get(), MakeVecView(imageCopy));
        groups[2].predecessors.push_back({producerQueue, 0, state, 1});
        const bool alias =
            queues.AreQueuesShared(producerQueue, RHICommandContextType::eAsyncCompute);
        RHITextureTransition storage    = clear;
        storage.oldUsage                = RHITextureUsage::eTransferDst;
        storage.oldAccessMode           = RHIAccessMode::eReadWrite;
        storage.newUsage                = RHITextureUsage::eStorage;
        storage.hasSourceAccessOverride = true;
        if (alias)
        {
            storage.sourceAccess.SetFlag(RHIAccessFlagBits::eTransferWrite);
        }
        HeapVector<RHIBufferTransition> buffers;
        if (alias)
        {
            buffers.push_back({RHIAccessMode::eReadWrite, RHIAccessMode::eReadWrite, data.Get(),
                               RHIBufferUsage::eTransferDst, RHIBufferUsage::eStorageBuffer});
        }
        lists[2]->AddTransitions(
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eAllCommands),
            BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader), {},
            buffers, MakeVecView(storage));
        lists[2]->BindPipeline(RHIPipelineType::eCompute, pipeline.Get());
        RHIBatchedShaderParameters parameters;
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0),
                                    data.Get(), nullptr, 0);
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1),
                                    texture->GetDefaultView(), nullptr, 0);
        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 2),
                                    output.Get(), nullptr, 0);
        lists[2]->SetShaderParameters(parameters);
        const uint32_t initialize = 0;
        lists[2]->SetPushConstants(pipeline.Get(), reinterpret_cast<const uint8_t*>(&initialize),
                                   sizeof(initialize), 0);
        lists[2]->Dispatch(1, 1, 1);
        groups[3].predecessors.push_back({RHICommandContextType::eAsyncCompute, 0, state, 2});
        lists[3]->CopyBuffer(output.Get(), readback, {0, 0, 8});
        gpuWaitCount = hostWaitCount = 0;
        waitValues.clear();
        test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);
        test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);
        const RHIBatchResult result = executor->SubmitFrame(groups, nullptr, state).Wait();
        ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);
        ASSERT_EQ(result.groups.size(), 4u);
        if (std::get<1>(GetParam()))
        {
            EXPECT_EQ(hostWaitCount, 0u);
            ASSERT_EQ(waitValues.size(), alias ? 1u : 2u);
            if (!alias)
            {
                EXPECT_EQ(waitValues[0], result.groups[1].accepted.serial);
            }
            EXPECT_EQ(waitValues.back(), result.groups[2].accepted.serial);
        }
        ASSERT_TRUE(executor->WaitForSubmission(RHICommandContextType::eGraphics,
                                                result.groups[3].accepted.serial));
        uint32_t expected = 0;
        std::memcpy(&expected, bytes.data(), sizeof(expected));
        const uint32_t* mapped = reinterpret_cast<const uint32_t*>(readback->Map());
        ASSERT_NE(mapped, nullptr);
        EXPECT_EQ(mapped[0], expected + 5);
        EXPECT_EQ(mapped[1], expected + 7);
        readback->Unmap();
        executor->WaitDeviceIdle();
    }
}

TEST_P(VulkanUploadIntegrationTest, InvalidFutureDependencyNeverSubmitsConsumer)
{
    RHICommandListPtr commands(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    commands->AddSubmissionDependency({RHICommandContextType::eTransfer, 100});
    commands->CopyBuffer(source, readback, {0, 0, bytes.size()});
    EXPECT_EQ(SubmitCommands(*commands), RHISubmissionResult::eFatal);
    EXPECT_TRUE(executor->AreSubmissionsBlocked());
    EXPECT_EQ(executor->GetLastSubmittedSerial(RHICommandContextType::eGraphics), 0u);
}

TEST_P(VulkanUploadIntegrationTest, SameQueueDependencyAddsNoSemaphoreOrHostWait)
{
    RHICommandListPtr producer(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    producer->CopyBuffer(source, destination, {0, 0, bytes.size()});
    ASSERT_EQ(SubmitCommands(*producer), RHISubmissionResult::eSuccess);
    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);
    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);
    RHICommandListPtr consumer(
        RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    consumer->AddSubmissionDependency(
        {RHICommandContextType::eGraphics, RHISubmissionDependency::kLatestSubmitted});
    consumer->CopyBuffer(source, readback, {0, 0, bytes.size()});
    ASSERT_EQ(SubmitCommands(*consumer), RHISubmissionResult::eSuccess);
    EXPECT_EQ(gpuWaitCount, 0u);
    EXPECT_EQ(hostWaitCount, 0u);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         VulkanUploadIntegrationTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline,
                                                          RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
