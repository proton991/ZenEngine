#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "ScopedVulkanCall.h"
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include <tuple>

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
    }

    RHIBuffer* Buffer(RHIBufferAllocateType allocation)
    {
        RHIBufferCreateInfo info{};
        info.size         = bytes.size();
        info.allocateType = allocation;
        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eTransferSrcBuffer,
                                 RHIBufferUsageFlagBits::eTransferDstBuffer);
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
    ASSERT_TRUE(executor->WaitForSubmission(RHICommandContextType::eGraphics, result.serials[0]));
    uint8_t* mapped = readback->Map();
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);
    readback->Unmap();
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
