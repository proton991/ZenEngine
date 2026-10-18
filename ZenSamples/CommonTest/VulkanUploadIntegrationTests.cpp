#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
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

class VulkanUploadIntegrationTest : public testing::TestWithParam<std::tuple<RHIExecutionMode, bool>>
{
protected:
    static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void*)
    {
        ADD_FAILURE() << data->pMessage;

        return VK_TRUE;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue, uint32_t count, const VkSubmitInfo* infos, VkFence fence)
    {
        submitBatchCounts.push_back(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            batchWaitCounts.push_back(infos[i].waitSemaphoreCount);

            gpuWaitCount    += infos[i].waitSemaphoreCount;

            emptyBatchCount += infos[i].commandBufferCount == 0;

            if (infos[i].waitSemaphoreCount != 0)
            {
                const VkTimelineSemaphoreSubmitInfo* timeline =
                    static_cast<const VkTimelineSemaphoreSubmitInfo*>(infos[i].pNext);

                if (timeline != nullptr && timeline->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)
                {
                    for (uint32_t j = 0; j < timeline->waitSemaphoreValueCount; ++j)
                    {
                        waitValues.push_back(timeline->pWaitSemaphoreValues[j]);

                        EXPECT_EQ(infos[i].pWaitDstStageMask[j], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    }
                }
            }
        }

        return submitBatchCounts.size() == failSubmitAt ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                        : originalSubmit(queue, count, infos, fence);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer commands)
    {
        ++endCalls;

        return endCalls == failEndAt ? VK_ERROR_OUT_OF_HOST_MEMORY : originalEnd(commands);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL KeepTimelinePending(VkDevice, VkSemaphore, uint64_t* value)
    {
        // Keep cached completion behind the producer so the dependency-placement
        // regression observes real queue waits even on a fast, otherwise idle GPU.
        *value = 0;

        return VK_SUCCESS;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice device, const VkSemaphoreWaitInfo* info, uint64_t timeout)
    {
        ++hostWaitCount;

        return originalWait(device, info, timeout);
    }

    void SetUp() override
    {
        countersWereEnabled = RHIOptions::GetInstance().ExecutionCountersEnabled();

        RHIOptions::GetInstance().SetExecutionCountersEnabled(true);

        backend = static_cast<VulkanRHI*>(DynamicRHI::Create(RHIAPIType::eVulkan));

        if (std::get<1>(GetParam()))
        {
            ASSERT_TRUE(backend->SupportsAsyncSubmissionDependencies());
        }
        else
        {
            backend->GetDevice()->GetExtensionFlags().hasTimelineSemaphore = 0;
        }

        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;

        info.pfnUserCallback = Validation;

        ASSERT_EQ(vkCreateDebugUtilsMessengerEXT(backend->GetInstance(), &info, nullptr, &messenger), VK_SUCCESS);

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

        originalSubmit  = vkQueueSubmit;

        originalWait    = vkWaitSemaphores;

        originalEnd     = vkEndCommandBuffer;

        failSubmitAt    = 0;

        failEndAt       = 0;

        endCalls        = 0;

        gpuWaitCount    = 0;

        hostWaitCount   = 0;

        emptyBatchCount = 0;

        waitValues.clear();

        submitBatchCounts.clear();

        batchWaitCounts.clear();
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
        waitValues        = {};

        submitBatchCounts = {};

        batchWaitCounts   = {};

        RHIOptions::GetInstance().SetExecutionCountersEnabled(countersWereEnabled);
    }

    RHIBuffer* Buffer(RHIBufferAllocateType allocation, bool storage = false)
    {
        RHIBufferCreateInfo info{};

        info.size         = bytes.size();

        info.allocateType = allocation;

        info.usageFlags.SetFlags(RHIBufferUsageFlagBits::eTransferSrcBuffer, RHIBufferUsageFlagBits::eTransferDstBuffer);

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

    uint64_t CompletedSerial(RHICommandContextType queue) const
    {
        return backend->GetDevice()->GetQueue(queue)->GetLastCompletedSerial();
    }

    struct ProducerWait
    {
        RHICommandContextType queue;
        uint64_t              serial;
    };

    // Each cross-queue GPU wait carries its producer's exact accepted serial, in submission order.
    // A wait is skipped when cached completion already covers the serial, which is also valid.
    void ExpectExactWaitsOrCompletedProducers(VectorView<const ProducerWait> producers)
    {
        EXPECT_LE(gpuWaitCount, producers.size());

        EXPECT_EQ(hostWaitCount, 0u);

        uint32_t emitted = 0;

        for (const ProducerWait& producer : producers)
        {
            if (emitted < waitValues.size() && waitValues[emitted] == producer.serial)
            {
                ++emitted;
            }
            else
            {
                EXPECT_GE(GetRHIThread().Invoke(&VulkanUploadIntegrationTest::CompletedSerial, this, producer.queue),
                          producer.serial);
            }
        }

        // Every emitted wait matched an expected producer.
        EXPECT_EQ(emitted, waitValues.size());
    }

    VulkanRHI*                           backend{nullptr};
    RHICommandListExecutor*              executor{nullptr};
    VkDebugUtilsMessengerEXT             messenger{VK_NULL_HANDLE};
    RHIBuffer*                           source{nullptr};
    RHIBuffer*                           destination{nullptr};
    RHIBuffer*                           readback{nullptr};
    std::array<uint8_t, 64>              bytes{};
    bool                                 countersWereEnabled{false};
    static inline PFN_vkQueueSubmit      originalSubmit;
    static inline PFN_vkWaitSemaphores   originalWait;
    static inline PFN_vkEndCommandBuffer originalEnd;
    static inline uint32_t               failSubmitAt{0};
    static inline uint32_t               failEndAt{0};
    static inline uint32_t               endCalls{0};
    static inline uint32_t               gpuWaitCount{0};
    static inline uint32_t               hostWaitCount{0};
    static inline uint32_t               emptyBatchCount{0};
    static inline HeapVector<uint64_t>   waitValues;
    static inline HeapVector<uint32_t>   submitBatchCounts;
    static inline HeapVector<uint32_t>   batchWaitCounts;
};

TEST_P(VulkanUploadIntegrationTest, PackedRunRejectsAtomicallyAndRetriesAfterNativeOrRecordingFailure)
{
    if (!std::get<1>(GetParam()))
    {
        GTEST_SKIP() << "Packing requires timeline submissions";
    }

    const RHICommandContextType queues[] = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics};

    RHICommandListPtr first(RHICommandList::Create(executor->GetCommandContext(queues[0])));

    RHICommandListPtr second(RHICommandList::Create(first->GetContext()));

    for (bool recordingFailure : {false, true})
    {
        SCOPED_TRACE(recordingFailure);

        RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 2));

        RHISubmissionGroup groups[]           = {{first.get()}, {second.get()}};

        groups[1].predecessors.push_back({queues[0], 0, state, 0});

        first->CopyBuffer(source, destination, {0, 0, bytes.size()});

        second->CopyBuffer(source, readback, {0, 0, bytes.size()});

        const uint64_t before = executor->GetLastSubmittedSerial(queues[0]);

        submitBatchCounts.clear();

        endCalls     = 0;

        failSubmitAt = recordingFailure ? 0 : 1;

        failEndAt    = recordingFailure ? 2 : 0;

        RHIBatchResult rejected;

        {
            test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

            test::ScopedVulkanCall endHook(vkEndCommandBuffer, &End);

            rejected = executor->SubmitGroups(MakeVecView(groups, 2), state);
        }

        EXPECT_EQ(rejected.submission, RHISubmissionResult::eRejected);

        ASSERT_EQ(rejected.groups.size(), 2u);

        for (const RHISubmissionGroupResult& group : rejected.groups)
        {
            EXPECT_EQ(group.submission, RHISubmissionResult::eRejected);

            EXPECT_EQ(group.accepted.serial, 0u);
        }

        EXPECT_EQ(executor->GetLastSubmittedSerial(queues[0]), before);

        EXPECT_FALSE(executor->AreSubmissionsBlocked());

        ASSERT_EQ(submitBatchCounts.size(), recordingFailure ? 0u : 1u);

        if (!recordingFailure)
        {
            EXPECT_EQ(submitBatchCounts[0], 2u);
        }

        RHISubmissionDependency point;

        EXPECT_EQ(state->Resolve(0, point), RHISubmissionPointStatus::eFailed);

        state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 2));

        groups[1].predecessors.clear();

        groups[1].predecessors.push_back({queues[0], 0, state, 0});

        first->CopyBuffer(source, destination, {0, 0, bytes.size()});

        second->CopyBuffer(source, readback, {0, 0, bytes.size()});

        submitBatchCounts.clear();

        failSubmitAt = 0;

        RHIBatchResult retry;

        {
            test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

            retry = executor->SubmitGroups(MakeVecView(groups, 2), state);
        }

        ASSERT_EQ(retry.submission, RHISubmissionResult::eSuccess);

        ASSERT_EQ(retry.groups.size(), 2u);

        ASSERT_EQ(submitBatchCounts.size(), 1u);

        EXPECT_EQ(submitBatchCounts[0], 2u);

        EXPECT_EQ(retry.groups[0].accepted.serial, before + 1);

        EXPECT_EQ(retry.groups[1].accepted.serial, before + 2);

        ASSERT_TRUE(executor->WaitForCompletion(queues[0], retry.groups[1].accepted.serial));

        const uint8_t* mapped = readback->Map();

        ASSERT_NE(mapped, nullptr);

        EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);

        readback->Unmap();
    }
}

TEST_P(VulkanUploadIntegrationTest, RejectedPackedRunKeepsAcceptedPrefixAndNeverSubmitsItsConsumer)
{
    const RHIQueueCapabilities capabilities = executor->GetQueueCapabilities();

    if (!std::get<1>(GetParam()) || !capabilities.computeSupported
        || capabilities.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "Requires timeline submissions and distinct graphics/compute queues";
    }

    const RHICommandContextType queues[]  = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics,
                                             RHICommandContextType::eAsyncCompute, RHICommandContextType::eAsyncCompute,
                                             RHICommandContextType::eGraphics};

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 5));

    HeapVector<RHICommandListPtr> lists;

    HeapVector<RHISubmissionGroup> groups;

    HeapVector<RHIResourcePtr<RHIBuffer>> outputs;

    for (uint32_t i = 0; i < 5; ++i)
    {
        lists.emplace_back(RHICommandList::Create(executor->GetCommandContext(queues[i])));

        outputs.emplace_back(Buffer(RHIBufferAllocateType::eGPU), false);

        lists.back()->CopyBuffer(source, outputs.back().Get(), {0, 0, bytes.size()});

        groups.push_back({lists.back().get()});

        if (i != 0)
        {
            groups.back().predecessors.push_back({queues[i - 1], 0, state, i - 1});
        }
    }

    const uint64_t graphicsBefore = executor->GetLastSubmittedSerial(queues[0]);

    const uint64_t computeBefore  = executor->GetLastSubmittedSerial(queues[2]);

    failSubmitAt                  = 2;

    RHIBatchResult result;

    {
        test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

        result = executor->SubmitGroups(groups, state);
    }

    ASSERT_EQ(result.submission, RHISubmissionResult::eFatal);

    ASSERT_EQ(result.groups.size(), 5u);

    ASSERT_EQ(submitBatchCounts.size(), 2u);

    for (uint32_t i = 0; i < 2; ++i)
    {
        EXPECT_EQ(submitBatchCounts[i], 2u);

        EXPECT_EQ(result.groups[i].submission, RHISubmissionResult::eSuccess);

        EXPECT_EQ(result.groups[i].accepted.serial, graphicsBefore + i + 1);

        EXPECT_EQ(result.groups[i + 2].submission, RHISubmissionResult::eFatal);

        EXPECT_EQ(result.groups[i + 2].accepted.serial, 0u);
    }

    EXPECT_EQ(result.groups[4].submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(result.groups[4].accepted.serial, 0u);

    EXPECT_EQ(executor->GetLastSubmittedSerial(queues[0]), graphicsBefore + 2);

    EXPECT_EQ(executor->GetLastSubmittedSerial(queues[2]), computeBefore);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    executor->WaitDeviceIdle();
}

TEST_P(VulkanUploadIntegrationTest, PackedRunsKeepDistinctSignalsAndLaterWaitsOnSharedContexts)
{
    const RHIQueueCapabilities capabilities = executor->GetQueueCapabilities();

    if (!capabilities.computeSupported
        || capabilities.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "A native compute queue distinct from graphics is required";
    }

    const RHICommandContextType queues[]  = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics,
                                             RHICommandContextType::eAsyncCompute, RHICommandContextType::eGraphics,
                                             RHICommandContextType::eGraphics};

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 5));

    HeapVector<RHICommandListPtr> lists;

    HeapVector<RHISubmissionGroup> groups;

    HeapVector<RHIResourcePtr<RHIBuffer>> outputs;

    for (uint32_t i = 0; i < 5; ++i)
    {
        IRHICommandContext* context = i == 0 || i == 2 ? executor->GetCommandContext(queues[i]) : lists[0]->GetContext();

        lists.emplace_back(RHICommandList::Create(context));

        if (i < 4)
        {
            outputs.emplace_back(Buffer(RHIBufferAllocateType::eGPU), false);
        }

        lists.back()->CopyBuffer(source, i < 4 ? outputs.back().Get() : readback, {0, 0, bytes.size()});

        groups.push_back({lists.back().get()});
    }

    groups[1].predecessors.push_back({queues[0], 0, state, 0});

    groups[2].predecessors.push_back({queues[0], 0, state, 0});

    groups[4].predecessors.push_back({queues[2], 0, state, 2});

    groups[4].predecessors.push_back({queues[3], 0, state, 3});

    const uint64_t beforeCalls    = backend->GetExecutionCounters().submissions;

    const uint64_t beforeGraphics = executor->GetLastSubmittedSerial(RHICommandContextType::eGraphics);

    const uint64_t beforeCompute  = executor->GetLastSubmittedSerial(RHICommandContextType::eAsyncCompute);

    RHIBatchResult result;

    {
        test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

        test::ScopedVulkanCall completionHook(vkGetSemaphoreCounterValue, &KeepTimelinePending);

        result = executor->SubmitFrame(groups, nullptr, state).Wait();
    }

    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

    ASSERT_EQ(result.groups.size(), 5u);

    const uint64_t serials[] = {beforeGraphics + 1, beforeGraphics + 2, beforeCompute + 1, beforeGraphics + 3,
                                beforeGraphics + 4};

    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(result.groups[i].accepted.queue, queues[i]);

        EXPECT_EQ(result.groups[i].accepted.serial, serials[i]);
    }

    const bool timeline = std::get<1>(GetParam());

    EXPECT_EQ(backend->GetExecutionCounters().submissions - beforeCalls, timeline ? 3u : 5u);

    ASSERT_EQ(submitBatchCounts.size(), timeline ? 3u : 5u);

    ASSERT_EQ(batchWaitCounts.size(), 5u);

    if (timeline)
    {
        EXPECT_EQ(submitBatchCounts[0], 2u);

        EXPECT_EQ(submitBatchCounts[1], 1u);

        EXPECT_EQ(submitBatchCounts[2], 2u);

        ASSERT_EQ(waitValues.size(), 2u);

        EXPECT_EQ(waitValues[0], result.groups[0].accepted.serial);

        EXPECT_EQ(waitValues[1], result.groups[2].accepted.serial);
    }

    for (uint32_t i = 0; i < 5; ++i)
    {
        EXPECT_EQ(batchWaitCounts[i], timeline && (i == 2 || i == 4) ? 1u : 0u);
    }

    ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics, serials[4]));

    uint8_t* mapped = readback->Map();

    ASSERT_NE(mapped, nullptr);

    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);

    readback->Unmap();
}

TEST_P(VulkanUploadIntegrationTest, EmptyGroupRelaysItsSameQueueProducerToAnotherQueue)
{
    const RHIQueueCapabilities capabilities = executor->GetQueueCapabilities();

    if (!capabilities.computeSupported
        || capabilities.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "Requires distinct graphics and compute queues";
    }

    const RHICommandContextType queues[]  = {RHICommandContextType::eGraphics, RHICommandContextType::eGraphics,
                                             RHICommandContextType::eAsyncCompute};

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 3));

    RHICommandListPtr producer(RHICommandList::Create(executor->GetCommandContext(queues[0])));

    RHICommandListPtr empty(RHICommandList::Create(producer->GetContext()));

    RHICommandListPtr consumer(RHICommandList::Create(executor->GetCommandContext(queues[2])));

    producer->CopyBuffer(source, destination, {0, 0, bytes.size()});

    consumer->CopyBuffer(destination, readback, {0, 0, bytes.size()});

    RHISubmissionGroup groups[] = {{producer.get()}, {empty.get()}, {consumer.get()}};

    groups[1].predecessors.push_back({queues[0], 0, state, 0});

    groups[2].predecessors.push_back({queues[1], 0, state, 1});

    const uint64_t before = executor->GetLastSubmittedSerial(queues[0]);

    RHIBatchResult result;

    {
        test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

        test::ScopedVulkanCall completionHook(vkGetSemaphoreCounterValue, &KeepTimelinePending);

        result = executor->SubmitFrame(MakeVecView(groups, 3), nullptr, state).Wait();
    }

    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

    ASSERT_EQ(result.groups.size(), 3u);

    EXPECT_EQ(result.groups[0].accepted.serial, before + 1);

    EXPECT_EQ(result.groups[1].accepted.serial, before + 2);

    const bool timeline = std::get<1>(GetParam());

    EXPECT_EQ(submitBatchCounts.size(), timeline ? 2u : 3u);

    EXPECT_EQ(emptyBatchCount, 1u);

    if (timeline)
    {
        ASSERT_EQ(waitValues.size(), 1u);

        EXPECT_EQ(waitValues[0], result.groups[1].accepted.serial);
    }

    ASSERT_TRUE(executor->WaitForCompletion(queues[2], result.groups[2].accepted.serial));

    const uint8_t* mapped = readback->Map();

    ASSERT_NE(mapped, nullptr);

    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);

    readback->Unmap();
}

TEST_P(VulkanUploadIntegrationTest, TransferContextsCanBeDestroyedAndRecreated)
{
    for (uint32_t iteration = 0; iteration < 2; ++iteration)
    {
        RHICommandListPtr upload(RHICommandList::Create(executor->GetTransferCommandContext()));

        upload->CopyBuffer(source, destination, {0, 0, bytes.size()});

        ASSERT_EQ(SubmitCommands(*upload), RHISubmissionResult::eSuccess);

        ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eTransfer,
                                                executor->GetLastSubmittedSerial(RHICommandContextType::eTransfer)));
    }
}

TEST_P(VulkanUploadIntegrationTest, TransferFeedsGraphicsThroughSubmissionDependency)
{
    RHICommandListPtr upload(RHICommandList::Create(executor->GetTransferCommandContext()));

    upload->CopyBuffer(source, destination, {0, 0, bytes.size()});

    ASSERT_EQ(SubmitCommands(*upload), RHISubmissionResult::eSuccess);

    const uint64_t transfer = executor->GetLastSubmittedSerial(RHICommandContextType::eTransfer);

    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);

    RHICommandListPtr graphics(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    // Multiple resources in a batch can depend on the same queue value; emit one native wait.
    graphics->AddSubmissionDependency({RHICommandContextType::eTransfer, transfer});

    graphics->AddSubmissionDependency({RHICommandContextType::eTransfer, RHISubmissionDependency::kLatestSubmitted});

    RHIBufferTransition transition{RHIAccessMode::eReadWrite, RHIAccessMode::eRead, destination, RHIBufferUsage::eTransferDst,
                                   RHIBufferUsage::eTransferSrc};

    const BitField<RHIPipelineStageFlagBits> transferStage(RHIPipelineStageFlagBits::eTransfer);

    graphics->AddTransitions(transferStage, transferStage, {}, MakeVecView(&transition, 1), {});

    graphics->CopyBuffer(destination, readback, {0, 0, bytes.size()});

    const RHIBatchResult result = executor->SubmitFrame(*graphics, nullptr).Wait();

    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

    const bool async = std::get<1>(GetParam()) && !executor->IsTransferQueueSharedWithGraphics();

    if (async)
    {
        const ProducerWait producer{RHICommandContextType::eTransfer, transfer};

        ExpectExactWaitsOrCompletedProducers(MakeVecView(producer));
    }
    else
    {
        EXPECT_EQ(gpuWaitCount, 0u);
    }

    ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                            result.requiredSerials.Get(RHICommandContextType::eGraphics)));

    uint8_t* mapped = readback->Map();

    ASSERT_NE(mapped, nullptr);

    EXPECT_EQ(std::memcmp(mapped, bytes.data(), bytes.size()), 0);

    readback->Unmap();
}

TEST_P(VulkanUploadIntegrationTest, ExactComputeProducerSurvivesInterveningComputeSubmission)
{
    const RHIQueueCapabilities queues = executor->GetQueueCapabilities();

    if (!queues.computeSupported
        || queues.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "A native compute queue distinct from graphics is required";
    }

    const RHICommandContextType queue     = RHICommandContextType::eAsyncCompute;

    RefCountPtr<RHISubmissionState> state = MakeRefCountPtr<RHISubmissionState>(MakeVecView(queue));

    RHICommandListPtr compute(RHICommandList::Create(executor->GetCommandContext(queue)));

    RHICommandListPtr graphics(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

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

    const RHIBatchResult result = executor->SubmitFrame(*graphics, nullptr, {}, MakeVecView(point)).Wait();

    ASSERT_EQ(result.submission, RHISubmissionResult::eSuccess);

    const bool timeline = std::get<1>(GetParam());

    if (timeline)
    {
        const ProducerWait producer{queue, accepted.serial};

        ExpectExactWaitsOrCompletedProducers(MakeVecView(producer));
    }
    else
    {
        EXPECT_EQ(gpuWaitCount, 0u);
    }

    ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics,
                                            result.requiredSerials.Get(RHICommandContextType::eGraphics)));

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

    if (!queues.computeSupported
        || queues.AreQueuesShared(RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute))
    {
        GTEST_SKIP() << "Requires a native compute queue distinct from graphics";
    }

    RHIShaderCreateInfo shaderInfo{};

    shaderInfo.stageFlags.SetFlag(RHIShaderStageFlagBits::eCompute);

    shaderInfo.spirvFileName[ToUnderlying(RHIShaderStage::eCompute)] =
        std::filesystem::relative(std::filesystem::path(RDG_REFLECTION_TEST_PATH) / "async_resources.comp.spv", SPV_SHADER_PATH)
            .generic_string();

    RHIResourcePtr<RHIShader> shader(executor->CreateShader(shaderInfo), false);

    RHIResourcePtr<RHIPipeline> pipeline(executor->CreatePipeline(RHIComputePipelineCreateInfo{shader.Get()}), false);

    for (RHICommandContextType producerQueue : {RHICommandContextType::eGraphics, RHICommandContextType::eTransfer})
    {
        RHIResourcePtr<RHIBuffer> data(Buffer(RHIBufferAllocateType::eGPU, true), false);

        RHIResourcePtr<RHIBuffer> output(Buffer(RHIBufferAllocateType::eGPU, true), false);

        RHIResourcePtr<RHIBuffer> scratch(Buffer(RHIBufferAllocateType::eGPU), false);

        RHITextureCreateInfo textureInfo{};

        textureInfo.type  = RHITextureType::e2D;

        textureInfo.width = textureInfo.height = 1;

        textureInfo.format                     = DataFormat::eR32UInt;

        textureInfo.usageFlags.SetFlags(RHITextureUsageFlagBits::eStorage, RHITextureUsageFlagBits::eTransferDst);

        RHIResourcePtr<RHITexture> texture(executor->CreateTexture(textureInfo), false);

        const SmallVector<RHICommandContextType, 4> contexts{RHICommandContextType::eGraphics, producerQueue,
                                                             RHICommandContextType::eAsyncCompute,
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

        lists[1]->AddTransitions(BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eTopOfPipe), transfer, {}, {},
                                 MakeVecView(clear));

        RHIBufferTextureCopyRegion imageCopy{};

        imageCopy.textureSubresources.aspect.SetFlag(RHITextureAspectFlagBits::eColor);

        imageCopy.textureSubresources.layerCount = 1;

        imageCopy.textureSize                    = {1, 1, 1};

        lists[1]->CopyBufferToTexture(source, texture.Get(), MakeVecView(imageCopy));

        groups[2].predecessors.push_back({producerQueue, 0, state, 1});

        const bool alias                = queues.AreQueuesShared(producerQueue, RHICommandContextType::eAsyncCompute);

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
            buffers.push_back({RHIAccessMode::eReadWrite, RHIAccessMode::eReadWrite, data.Get(), RHIBufferUsage::eTransferDst,
                               RHIBufferUsage::eStorageBuffer});
        }

        lists[2]->AddTransitions(BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eAllCommands),
                                 BitField<RHIPipelineStageFlagBits>(RHIPipelineStageFlagBits::eComputeShader), {}, buffers,
                                 MakeVecView(storage));

        lists[2]->BindPipeline(RHIPipelineType::eCompute, pipeline.Get());

        RHIBatchedShaderParameters parameters;

        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 0), data.Get(), nullptr, 0);

        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 1), texture->GetDefaultView(), nullptr,
                                    0);

        parameters.AddResourceParam(*shader->GetSRDByLocation(test::kLocalResourceSet, 2), output.Get(), nullptr, 0);

        lists[2]->SetShaderParameters(parameters);

        const uint32_t initialize = 0;

        lists[2]->SetPushConstants(pipeline.Get(), reinterpret_cast<const uint8_t*>(&initialize), sizeof(initialize), 0);

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
            HeapVector<ProducerWait> producers;

            if (!alias)
            {
                producers.push_back({producerQueue, result.groups[1].accepted.serial});
            }

            producers.push_back({RHICommandContextType::eAsyncCompute, result.groups[2].accepted.serial});

            ExpectExactWaitsOrCompletedProducers(MakeVecView(producers));
        }

        ASSERT_TRUE(executor->WaitForCompletion(RHICommandContextType::eGraphics, result.groups[3].accepted.serial));

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
    RHICommandListPtr commands(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    commands->AddSubmissionDependency({RHICommandContextType::eTransfer, 100});

    commands->CopyBuffer(source, readback, {0, 0, bytes.size()});

    EXPECT_EQ(SubmitCommands(*commands), RHISubmissionResult::eFatal);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    EXPECT_EQ(executor->GetLastSubmittedSerial(RHICommandContextType::eGraphics), 0u);
}

TEST_P(VulkanUploadIntegrationTest, SameQueueDependencyAddsNoSemaphoreOrHostWait)
{
    RHICommandListPtr producer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    producer->CopyBuffer(source, destination, {0, 0, bytes.size()});

    ASSERT_EQ(SubmitCommands(*producer), RHISubmissionResult::eSuccess);

    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);

    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);

    RHICommandListPtr consumer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

    consumer->AddSubmissionDependency({RHICommandContextType::eGraphics, RHISubmissionDependency::kLatestSubmitted});

    consumer->CopyBuffer(source, readback, {0, 0, bytes.size()});

    ASSERT_EQ(SubmitCommands(*consumer), RHISubmissionResult::eSuccess);

    EXPECT_EQ(gpuWaitCount, 0u);

    EXPECT_EQ(hostWaitCount, 0u);
}

TEST_P(VulkanUploadIntegrationTest, CompletedCrossQueueDependencyWaitCount)
{
    RHICommandListPtr producer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eAsyncCompute)));
    producer->CopyBuffer(source, destination, {0, 0, bytes.size()});
    ASSERT_EQ(SubmitCommands(*producer), RHISubmissionResult::eSuccess);
    executor->WaitDeviceIdle();
    test::ScopedVulkanCall submitHook(vkQueueSubmit, &Submit);
    test::ScopedVulkanCall waitHook(vkWaitSemaphores, &Wait);
    RHICommandListPtr      consumer(RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));
    consumer->AddSubmissionDependency({RHICommandContextType::eAsyncCompute, RHISubmissionDependency::kLatestSubmitted});
    consumer->CopyBuffer(source, readback, {0, 0, bytes.size()});
    ASSERT_EQ(SubmitCommands(*consumer), RHISubmissionResult::eSuccess);
    printf("RVFIX_WAIT completed_frame_gpu_waits=%u host_waits=%u\n", gpuWaitCount, hostWaitCount);
    EXPECT_EQ(gpuWaitCount, 0u);
    EXPECT_EQ(hostWaitCount, 0u);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         VulkanUploadIntegrationTest,
                         testing::Combine(testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded),
                                          testing::Bool()));
} // namespace
