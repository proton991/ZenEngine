#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include <gtest/gtest.h>
#include <cmath>
#include "Utils/UniquePtr.h"

namespace
{
using namespace zen;

VKAPI_ATTR VkResult VKAPI_CALL RejectTimingPool(VkDevice,
                                                const VkQueryPoolCreateInfo*,
                                                const VkAllocationCallbacks*,
                                                VkQueryPool*)
{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

struct TimingReadObserver
{
    static inline PFN_vkGetQueryPoolResults original{};

    static inline uint32_t calls{0};

    static inline bool fail{false};

    static VKAPI_ATTR VkResult VKAPI_CALL Read(VkDevice device,
                                               VkQueryPool pool,
                                               uint32_t first,
                                               uint32_t count,
                                               size_t size,
                                               void* data,
                                               VkDeviceSize stride,
                                               VkQueryResultFlags flags)
    {
        ++calls;

        EXPECT_EQ(flags & VK_QUERY_RESULT_WAIT_BIT, 0u);

        EXPECT_NE(flags & VK_QUERY_RESULT_64_BIT, 0u);

        EXPECT_NE(flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT, 0u);

        return fail ? VK_ERROR_OUT_OF_HOST_MEMORY :
                      original(device, pool, first, count, size, data, stride, flags);
    }
};

class VulkanGPUTimingIntegrationTest : public testing::Test
{
protected:
    UniquePtr<test::VulkanSession> session;

    VkDebugUtilsMessengerEXT messenger{};

    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;

        return VK_FALSE;
    }

    void SetUp() override
    {
        session = MakeUnique<test::VulkanSession>();

        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;

        info.pfnUserCallback = Validation;

        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);

        TimingReadObserver::original = vkGetQueryPoolResults;

        TimingReadObserver::calls = 0;

        TimingReadObserver::fail = false;
    }

    void TearDown() override
    {
        session->rhi.WaitDeviceIdle();

        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);

        session.Reset();
    }

    bool SupportsTiming(FVulkanCommandListContext& context)
    {
        const VkQueueFamilyProperties& family = session->rhi.GetDevice()->GetQueueFamilyProperties(
            context.GetQueue()->GetFamilyIndex());

        return family.timestampValidBits != 0 &&
            (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
    }

    void SubmitAndComplete(FVulkanCommandListContext& context)
    {
        ASSERT_EQ(context.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

        ASSERT_TRUE(
            context.GetQueue()->WaitForCompletion(context.GetLastSubmittedSerial(), UINT64_MAX));
    }
};

TEST_F(VulkanGPUTimingIntegrationTest, SupportedQueuesResolveFiniteDurationsWithoutReadbackWaits)
{
    test::ScopedVulkanCall hook(vkGetQueryPoolResults, TimingReadObserver::Read);

    uint32_t supportedQueues = 0;

    for (RHICommandContextType queue :
         {RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute,
          RHICommandContextType::eTransfer})
    {
        SCOPED_TRACE(RHIQueueName(queue));

        FVulkanCommandListContext context(queue, session->rhi.GetDevice());

        RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

        context.RHIBeginGPUTiming(result);

        context.RHIEndGPUTiming(result);

        const bool supported = SupportsTiming(context);

        supportedQueues += supported ? 1u : 0u;

        EXPECT_EQ(result->GetStatus(),
                  supported ? RHIGPUTimingStatus::ePending : RHIGPUTimingStatus::eUnsupported);

        SubmitAndComplete(context);

        if (supported)
        {
            EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eAvailable);

            EXPECT_TRUE(std::isfinite(result->GetMicroseconds()));

            EXPECT_GE(result->GetMicroseconds(), 0.0);
        }
        else
        {
            // A transfer-only queue cannot execute vkCmdResetQueryPool.
            EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eUnsupported);
        }
    }

    EXPECT_EQ(TimingReadObserver::calls, supportedQueues);
}

TEST_F(VulkanGPUTimingIntegrationTest, CompletedBufferReuseDoesNotOverwritePublishedResults)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (!SupportsTiming(context))
    {
        GTEST_SKIP() << "Graphics timestamps unavailable";
    }

    RHIGPUTimingPtr first = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(first);

    FVulkanCommandBuffer* buffer = context.GetCommandBuffer();

    context.RHIEndGPUTiming(first);

    SubmitAndComplete(context);

    ASSERT_EQ(first->GetStatus(), RHIGPUTimingStatus::eAvailable);

    const double previous = first->GetMicroseconds();

    RHIGPUTimingPtr second = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(second);

    EXPECT_EQ(context.GetCommandBuffer(), buffer);

    context.RHIEndGPUTiming(second);

    EXPECT_EQ(second->GetStatus(), RHIGPUTimingStatus::ePending);

    SubmitAndComplete(context);

    EXPECT_EQ(second->GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_EQ(first->GetMicroseconds(), previous);
}

TEST_F(VulkanGPUTimingIntegrationTest, AbandonedScopesAndCapacityOverflowHaveExplicitStatus)
{
    HeapVector<RHIGPUTimingPtr> results;

    RHIGPUTimingPtr overflow = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    {
        FVulkanCommandListContext context(RHICommandContextType::eGraphics,
                                          session->rhi.GetDevice());

        if (!SupportsTiming(context))
        {
            GTEST_SKIP() << "Graphics timestamps unavailable";
        }

        for (uint32_t index = 0; index < FVulkanCommandBuffer::kMaxGPUTimingScopes; ++index)
        {
            results.push_back(MakeShared<RHIGPUTimingResult, MultiThreadCounter>());

            context.RHIBeginGPUTiming(results.back());

            context.RHIEndGPUTiming(results.back());

            EXPECT_EQ(results.back()->GetStatus(), RHIGPUTimingStatus::ePending);
        }

        context.RHIBeginGPUTiming(overflow);

        context.RHIEndGPUTiming(overflow);

        EXPECT_EQ(overflow->GetStatus(), RHIGPUTimingStatus::eDropped);
    }

    for (const RHIGPUTimingPtr& result : results)
    {
        EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eDiscarded);
    }

    EXPECT_EQ(overflow->GetStatus(), RHIGPUTimingStatus::eDropped);
}

TEST_F(VulkanGPUTimingIntegrationTest, SplitScopeCannotFabricateATiming)
{
    VulkanSemaphore signal(session->rhi.GetDevice());

    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (!SupportsTiming(context))
    {
        GTEST_SKIP() << "Graphics timestamps unavailable";
    }

    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(result);

    context.AddSignalSemaphore(&signal);

    // Returning from signal to execute starts a new native command buffer.
    context.RHIEndGPUTiming(result);

    EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eError);
}

TEST_F(VulkanGPUTimingIntegrationTest, QueryReadFailureDoesNotFailRendering)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (!SupportsTiming(context))
    {
        GTEST_SKIP() << "Graphics timestamps unavailable";
    }

    TimingReadObserver::fail = true;

    test::ScopedVulkanCall hook(vkGetQueryPoolResults, TimingReadObserver::Read);

    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(result);

    context.RHIEndGPUTiming(result);

    SubmitAndComplete(context);

    EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_FALSE(session->rhi.AreSubmissionsBlocked());
}

TEST_F(VulkanGPUTimingIntegrationTest, PoolAllocationFailureReleasesCapacityForRetry)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (!SupportsTiming(context))
    {
        GTEST_SKIP() << "Graphics timestamps unavailable";
    }

    {
        test::ScopedVulkanCall hook(vkCreateQueryPool, RejectTimingPool);

        for (uint32_t attempt = 0; attempt <= FVulkanCommandBuffer::kMaxGPUTimingPools; ++attempt)
        {
            RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

            context.RHIBeginGPUTiming(result);

            EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eError);
        }
    }

    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(result);

    context.RHIEndGPUTiming(result);

    SubmitAndComplete(context);

    EXPECT_EQ(result->GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_FALSE(session->rhi.AreSubmissionsBlocked());
}

TEST_F(VulkanGPUTimingIntegrationTest, DevicePoolBudgetRecyclesRetiredHandles)
{
    VulkanDevice& device = *session->rhi.GetDevice();

    HeapVector<VkQueryPool> acquired;

    for (uint32_t index = 0; index < FVulkanCommandBuffer::kMaxGPUTimingPools; ++index)
    {
        VkQueryPool pool = VK_NULL_HANDLE;

        ASSERT_EQ(device.AcquireGPUTimingPool(pool), VK_SUCCESS);

        acquired.push_back(pool);
    }

    VkQueryPool overflow = VK_NULL_HANDLE;

    EXPECT_EQ(device.AcquireGPUTimingPool(overflow), VK_ERROR_TOO_MANY_OBJECTS);

    EXPECT_EQ(overflow, VK_NULL_HANDLE);

    for (VkQueryPool pool : acquired)
    {
        device.ReleaseGPUTimingPool(pool);
    }

    test::ScopedVulkanCall rejectNewAllocation(vkCreateQueryPool, RejectTimingPool);

    for (uint32_t index = 0; index < 2 * FVulkanCommandBuffer::kMaxGPUTimingPools; ++index)
    {
        VkQueryPool pool = VK_NULL_HANDLE;

        ASSERT_EQ(device.AcquireGPUTimingPool(pool), VK_SUCCESS);

        EXPECT_NE(pool, VK_NULL_HANDLE);

        device.ReleaseGPUTimingPool(pool);
    }
}

TEST_F(VulkanGPUTimingIntegrationTest, FrameEnvelopeCollectsNativeQueuesWithoutReadbackWaits)
{
    if (session->rhi.GetDevice()->GetExtensionFlags().hasCalibratedTimestamps)
    {
        test::ScopedVulkanCall hook(vkGetQueryPoolResults, TimingReadObserver::Read);

        RHIGPUFrameTimingPtr frame = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(frame);

        HeapVector<UniquePtr<FVulkanCommandListContext>> contexts;

        uint32_t included = 0;

        uint32_t excluded = 0;

        bool supported = true;

        for (RHICommandContextType queue :
             {RHICommandContextType::eGraphics, RHICommandContextType::eAsyncCompute,
              RHICommandContextType::eTransfer})
        {
            contexts.push_back(
                MakeUnique<FVulkanCommandListContext>(queue, session->rhi.GetDevice()));

            FVulkanCommandListContext& context = *contexts.back();

            const VkQueueFamilyProperties& family =
                session->rhi.GetDevice()->GetQueueFamilyProperties(
                    context.GetQueue()->GetFamilyIndex());

            const bool timingQueue =
                (family.queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;

            included += timingQueue ? 1u : 0u;

            excluded += timingQueue ? 0u : 1u;

            supported &= !timingQueue || SupportsTiming(context);

            context.GetCommandBuffer();

            EXPECT_EQ(context.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);
        }

        session->rhi.EndGPUFrameTiming(frame, true);

        session->rhi.WaitDeviceIdle();

        EXPECT_EQ(frame->GetIntervalCount(), included);

        EXPECT_EQ(frame->GetExcludedIntervalCount(), excluded);

        EXPECT_EQ(frame->GetStatus(),
                  supported ? RHIGPUTimingStatus::eAvailable : RHIGPUTimingStatus::eUnsupported);

        EXPECT_TRUE(std::isfinite(frame->GetMicroseconds()));

        EXPECT_GE(frame->GetMicroseconds(), 0.0);

        EXPECT_GT(TimingReadObserver::calls, 0u);
    }
    else
    {
        GTEST_SKIP() << "Device timestamps are not comparable across submissions";
    }
}

TEST_F(VulkanGPUTimingIntegrationTest, FrameEnvelopeFollowsNativeBufferSplitsAndReuse)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (SupportsTiming(context) &&
        session->rhi.GetDevice()->GetExtensionFlags().hasCalibratedTimestamps)
    {
        VulkanSemaphore signal(session->rhi.GetDevice());

        RHIGPUFrameTimingPtr first = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(first);

        FVulkanCommandBuffer* buffer = context.GetCommandBuffer();

        context.AddSignalSemaphore(&signal);

        EXPECT_NE(context.GetCommandBuffer(), buffer);

        EXPECT_EQ(context.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);

        session->rhi.EndGPUFrameTiming(first, true);

        EXPECT_TRUE(
            context.GetQueue()->WaitForCompletion(context.GetLastSubmittedSerial(), UINT64_MAX));

        EXPECT_EQ(first->GetStatus(), RHIGPUTimingStatus::eAvailable);

        EXPECT_EQ(first->GetIntervalCount(), 2u);

        const double previous = first->GetMicroseconds();

        RHIGPUFrameTimingPtr second = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(second);

        context.GetCommandBuffer();

        SubmitAndComplete(context);

        session->rhi.EndGPUFrameTiming(second, true);

        EXPECT_EQ(second->GetStatus(), RHIGPUTimingStatus::eAvailable);

        EXPECT_EQ(second->GetIntervalCount(), 1u);

        EXPECT_EQ(first->GetMicroseconds(), previous);
    }
    else
    {
        GTEST_SKIP() << "Comparable graphics timestamps unavailable";
    }
}

TEST_F(VulkanGPUTimingIntegrationTest, UnclosedNativeBuffersInvalidateFrameBoundaries)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    context.GetCommandBuffer();

    RHIGPUFrameTimingPtr beganLate = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    session->rhi.BeginGPUFrameTiming(beganLate);

    SubmitAndComplete(context);

    session->rhi.EndGPUFrameTiming(beganLate, true);

    EXPECT_EQ(beganLate->GetStatus(), RHIGPUTimingStatus::eError);

    if (session->rhi.GetDevice()->GetExtensionFlags().hasCalibratedTimestamps)
    {
        RHIGPUFrameTimingPtr endedEarly = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(endedEarly);

        context.GetCommandBuffer();

        session->rhi.EndGPUFrameTiming(endedEarly, true);

        SubmitAndComplete(context);

        EXPECT_EQ(endedEarly->GetStatus(), RHIGPUTimingStatus::eError);
    }
}

TEST_F(VulkanGPUTimingIntegrationTest, UnsubmittedNativeBuffersInvalidateFrameBoundaries)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    context.GetCommandBuffer()->End();

    RHIGPUFrameTimingPtr beganLate = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    session->rhi.BeginGPUFrameTiming(beganLate);

    SubmitAndComplete(context);

    session->rhi.EndGPUFrameTiming(beganLate, true);

    EXPECT_EQ(beganLate->GetStatus(), RHIGPUTimingStatus::eError);

    if (session->rhi.GetDevice()->GetExtensionFlags().hasCalibratedTimestamps)
    {
        RHIGPUFrameTimingPtr endedEarly = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(endedEarly);

        context.GetCommandBuffer()->End();

        session->rhi.EndGPUFrameTiming(endedEarly, true);

        SubmitAndComplete(context);

        EXPECT_EQ(endedEarly->GetStatus(), RHIGPUTimingStatus::eError);
    }
}

TEST_F(VulkanGPUTimingIntegrationTest, MissingCommonDomainKeepsPerPassTimingAvailable)
{
    RHIGPUFrameTimingPtr frame = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    DeviceExtensionFlags& flags = session->rhi.GetDevice()->GetExtensionFlags();

    const uint32_t saved = flags.hasCalibratedTimestamps;

    flags.hasCalibratedTimestamps = 0;

    session->rhi.BeginGPUFrameTiming(frame);

    flags.hasCalibratedTimestamps = saved;

    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    RHIGPUTimingPtr pass = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    context.RHIBeginGPUTiming(pass);

    context.RHIEndGPUTiming(pass);

    SubmitAndComplete(context);

    session->rhi.EndGPUFrameTiming(frame, true);

    EXPECT_EQ(frame->GetStatus(), RHIGPUTimingStatus::eUnsupported);

    EXPECT_EQ(frame->GetIntervalCount(), 0u);

    EXPECT_EQ(pass->GetStatus(),
              SupportsTiming(context) ? RHIGPUTimingStatus::eAvailable :
                                        RHIGPUTimingStatus::eUnsupported);
}

TEST_F(VulkanGPUTimingIntegrationTest, DiscardedAndFailedFramesCannotPublishPartialDurations)
{
    FVulkanCommandListContext context(RHICommandContextType::eGraphics, session->rhi.GetDevice());

    if (SupportsTiming(context) &&
        session->rhi.GetDevice()->GetExtensionFlags().hasCalibratedTimestamps)
    {
        RHIGPUFrameTimingPtr discarded = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(discarded);

        {
            FVulkanCommandListContext recording(RHICommandContextType::eGraphics,
                                                session->rhi.GetDevice());

            recording.GetCommandBuffer();
        }

        session->rhi.EndGPUFrameTiming(discarded, true);

        EXPECT_EQ(discarded->GetStatus(), RHIGPUTimingStatus::eDiscarded);

        EXPECT_EQ(discarded->GetMicroseconds(), 0.0);

        RHIGPUFrameTimingPtr failed = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

        session->rhi.BeginGPUFrameTiming(failed);

        context.GetCommandBuffer();

        SubmitAndComplete(context);

        session->rhi.EndGPUFrameTiming(failed, false);

        EXPECT_EQ(failed->GetStatus(), RHIGPUTimingStatus::eError);

        EXPECT_EQ(failed->GetMicroseconds(), 0.0);
    }
    else
    {
        GTEST_SKIP() << "Comparable graphics timestamps unavailable";
    }
}
} // namespace
