#include "Graphics/RHI/RHIGPUFrameTiming.h"
#include <gtest/gtest.h>
#include <atomic>
#include <limits>
#include <thread>

namespace
{
using namespace zen;

RHIGPUTimingPtr AddPublishedInterval(RHIGPUFrameTiming& frame,
                                     uint64_t           begin,
                                     uint64_t           end,
                                     uint32_t           validBits = 64,
                                     double periodNanoseconds     = 1000)
{
    RHIGPUTimingPtr interval = frame.AddInterval();

    EXPECT_TRUE(interval);

    if (interval)
    {
        EXPECT_TRUE(interval->PublishTimestamps({begin, end, validBits, periodNanoseconds}));
    }

    return interval;
}

void PublishAfterRelease(RHIGPUTimingPtr interval, const std::atomic<bool>* released, std::atomic<bool>* completed)
{
    while (!released->load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }

    interval->PublishTimestamps({100, 250, 64, 1000});

    completed->store(true, std::memory_order_release);
}

TEST(RHIGPUFrameTiming, OverlappingQueuesUseElapsedEnvelopeInsteadOfDurationSum)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 100, 300);

    AddPublishedInterval(frame, 200, 400);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 300);

    EXPECT_EQ(frame.GetIntervalCount(), 2u);
}

TEST(RHIGPUFrameTiming, RecordingOrderDoesNotDetermineEarliestTimestamp)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 200, 260);

    AddPublishedInterval(frame, 100, 230);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 160);
}

TEST(RHIGPUFrameTiming, UnsealedFramesRemainPendingEvenWhenAllIntervalsAreReady)
{
    RHIGPUFrameTiming frame;

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::ePending);

    AddPublishedInterval(frame, 10, 40);

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::ePending);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 30);
}

TEST(RHIGPUFrameTiming, SealedFramesWaitForEverySubmittedInterval)
{
    RHIGPUFrameTiming frame;

    RHIGPUTimingPtr delayed = frame.AddInterval();

    AddPublishedInterval(frame, 200, 400);

    frame.Seal();

    EXPECT_TRUE(delayed);

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::ePending);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);

    if (delayed)
    {
        delayed->PublishTimestamps({100, 300, 64, 1000});
    }

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 300);
}

TEST(RHIGPUFrameTiming, CounterWrapAndUnusedHighBitsDoNotInflateElapsedTime)
{
    RHIGPUFrameTiming narrow;

    AddPublishedInterval(narrow, 0x1fa, 0x205, 8);

    AddPublishedInterval(narrow, 0x201, 0x210, 8);

    narrow.Seal();

    EXPECT_EQ(narrow.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(narrow.GetMicroseconds(), 22);

    RHIGPUFrameTiming full;

    AddPublishedInterval(full, UINT64_MAX - 10, 3);

    AddPublishedInterval(full, UINT64_MAX - 4, 8);

    full.Seal();

    EXPECT_EQ(full.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(full.GetMicroseconds(), 19);
}

TEST(RHIGPUFrameTiming, DifferentQueueCounterWidthsUseCommonValidBits)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 0xabf0, 0xac08, 16);

    AddPublishedInterval(frame, 0x05, 0x12, 8);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 34);
}

TEST(RHIGPUFrameTiming, TimestampPeriodIsAppliedOnceToTheEnvelope)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 100, 300, 64, 2.5);

    AddPublishedInterval(frame, 200, 500, 64, 2.5);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 1);
}

TEST(RHIGPUFrameTiming, NarrowerQueueCannotHideAWholeWrapFromAWiderInterval)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 0, 300, 16);

    AddPublishedInterval(frame, 20, 30, 8);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
}

TEST(RHIGPUFrameTiming, NarrowerQueueCannotHideAWholeWrapBetweenWiderIntervals)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 0, 10, 16);

    AddPublishedInterval(frame, 270, 280, 16);

    AddPublishedInterval(frame, 20, 30, 8);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
}

TEST(RHIGPUFrameTiming, WidestCounterAnchorIsChosenIndependentlyOfRecordingOrder)
{
    RHIGPUFrameTiming shortFrame;

    AddPublishedInterval(shortFrame, 0x05, 0x12, 8);

    AddPublishedInterval(shortFrame, 0xabf0, 0xac08, 16);

    shortFrame.Seal();

    EXPECT_EQ(shortFrame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(shortFrame.GetMicroseconds(), 34);

    RHIGPUFrameTiming longFrame;

    AddPublishedInterval(longFrame, 20, 30, 8);

    AddPublishedInterval(longFrame, 0, 10, 16);

    AddPublishedInterval(longFrame, 270, 280, 16);

    longFrame.Seal();

    EXPECT_EQ(longFrame.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(longFrame.GetMicroseconds(), 0);
}

TEST(RHIGPUFrameTiming, MismatchedClockPeriodsCannotProduceAFrameDuration)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 100, 300, 64, 1);

    AddPublishedInterval(frame, 200, 400, 64, 2);

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
}

TEST(RHIGPUFrameTiming, AmbiguousHalfCounterRangeIsRejectedForIntervalsAndEnvelope)
{
    RHIGPUFrameTiming exactHalf;

    AddPublishedInterval(exactHalf, 0, 128, 8);

    exactHalf.Seal();

    EXPECT_EQ(exactHalf.GetStatus(), RHIGPUTimingStatus::eError);

    RHIGPUFrameTiming longEnvelope;

    AddPublishedInterval(longEnvelope, 80, 100, 8);

    AddPublishedInterval(longEnvelope, 180, 210, 8);

    longEnvelope.Seal();

    EXPECT_EQ(longEnvelope.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(longEnvelope.GetMicroseconds(), 0);

    RHIGPUFrameTiming fullWidth;

    AddPublishedInterval(fullWidth, 0, uint64_t(1) << 63);

    fullWidth.Seal();

    EXPECT_EQ(fullWidth.GetStatus(), RHIGPUTimingStatus::eError);
}

TEST(RHIGPUFrameTiming, InvalidTimestampDescriptionsStayUnavailable)
{
    const RHIGPUTimestampInterval invalid[] = {
        {0, 1, 0, 1},
        {0, 1, 65, 1},
        {0, 1, 64, 0},
        {0, 1, 64, -1},
        {0, 1, 64, std::numeric_limits<double>::infinity()},
        {0, 1, 64, std::numeric_limits<double>::quiet_NaN()},
    };

    for (const RHIGPUTimestampInterval& description : invalid)
    {
        RHIGPUFrameTiming frame;

        RHIGPUTimingPtr interval = frame.AddInterval();

        EXPECT_TRUE(interval);

        if (interval)
        {
            interval->PublishTimestamps(description);
        }

        frame.Seal();

        EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eError);

        EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
    }
}

TEST(RHIGPUFrameTiming, DurationOnlyTokensCannotBeMistakenForSharedClockTimestamps)
{
    RHIGPUFrameTiming frame;

    RHIGPUTimingPtr interval = frame.AddInterval();

    EXPECT_TRUE(interval);

    if (interval)
    {
        interval->Publish(RHIGPUTimingStatus::eAvailable, 12.5);
    }

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eError);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
}

TEST(RHIGPUFrameTiming, FailedIntervalsInvalidateTheWholeFrame)
{
    const RHIGPUTimingStatus failures[] = {
        RHIGPUTimingStatus::eUnsupported,
        RHIGPUTimingStatus::eDropped,
        RHIGPUTimingStatus::eDiscarded,
        RHIGPUTimingStatus::eError,
    };

    for (const RHIGPUTimingStatus status : failures)
    {
        RHIGPUFrameTiming frame;

        AddPublishedInterval(frame, 100, 150);

        RHIGPUTimingPtr failed = frame.AddInterval();

        EXPECT_TRUE(failed);

        if (failed)
        {
            failed->Publish(status);
        }

        frame.Seal();

        EXPECT_EQ(frame.GetStatus(), status);

        EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
    }
}

TEST(RHIGPUFrameTiming, ExplicitSealFailureOverridesValidIntervals)
{
    const RHIGPUTimingStatus failures[] = {
        RHIGPUTimingStatus::eUnsupported,
        RHIGPUTimingStatus::eError,
    };

    for (const RHIGPUTimingStatus status : failures)
    {
        RHIGPUFrameTiming frame;

        AddPublishedInterval(frame, 100, 150);

        frame.Seal(status);

        EXPECT_EQ(frame.GetStatus(), status);

        EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);
    }
}

TEST(RHIGPUFrameTiming, EmptyFramesAreDiscardedAndExcludedTransfersAreCounted)
{
    RHIGPUFrameTiming empty;

    empty.ExcludeInterval();

    empty.Seal();

    EXPECT_EQ(empty.GetStatus(), RHIGPUTimingStatus::eDiscarded);

    EXPECT_EQ(empty.GetIntervalCount(), 0u);

    EXPECT_EQ(empty.GetExcludedIntervalCount(), 1u);

    RHIGPUFrameTiming graphics;

    AddPublishedInterval(graphics, 10, 40);

    graphics.ExcludeInterval();

    graphics.ExcludeInterval();

    graphics.Seal();

    EXPECT_EQ(graphics.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(graphics.GetMicroseconds(), 30);

    EXPECT_EQ(graphics.GetIntervalCount(), 1u);

    EXPECT_EQ(graphics.GetExcludedIntervalCount(), 2u);
}

TEST(RHIGPUFrameTiming, IntervalCapacityDropsTheFrameInsteadOfReportingPartialWork)
{
    RHIGPUFrameTiming frame;

    for (uint32_t index = 0; index < RHIGPUFrameTiming::kMaxIntervals; ++index)
    {
        AddPublishedInterval(frame, index, index + 1);
    }

    EXPECT_FALSE(frame.AddInterval());

    frame.Seal();

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eDropped);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 0);

    EXPECT_EQ(frame.GetIntervalCount(), RHIGPUFrameTiming::kMaxIntervals);
}

TEST(RHIGPUFrameTiming, SealingPreventsFurtherRecordingAndKeepsTheFirstOutcome)
{
    RHIGPUFrameTiming frame;

    AddPublishedInterval(frame, 100, 150);

    frame.ExcludeInterval();

    frame.Seal();

    EXPECT_FALSE(frame.AddInterval());

    frame.ExcludeInterval();

    frame.Seal(RHIGPUTimingStatus::eError);

    EXPECT_EQ(frame.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame.GetMicroseconds(), 50);

    EXPECT_EQ(frame.GetIntervalCount(), 1u);

    EXPECT_EQ(frame.GetExcludedIntervalCount(), 1u);
}

TEST(RHIGPUFrameTiming, SubsequentFramesCannotOverwriteAnEarlierCapture)
{
    RHIGPUFrameTiming first;

    RHIGPUTimingPtr interval = AddPublishedInterval(first, 100, 150);

    first.Seal();

    if (interval)
    {
        EXPECT_FALSE(interval->PublishTimestamps({200, 1200, 64, 1000}));
    }

    RHIGPUFrameTiming next;

    interval = AddPublishedInterval(next, 200, 1200);

    next.Seal();

    EXPECT_EQ(first.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(first.GetMicroseconds(), 50);

    EXPECT_EQ(next.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(next.GetMicroseconds(), 1000);
}

TEST(RHIGPUFrameTiming, CompletionPublicationReleasesRawTimestampsToConcurrentReaders)
{
    RHIGPUFrameTimingPtr frame = MakeShared<RHIGPUFrameTiming, MultiThreadCounter>();

    RHIGPUTimingPtr interval   = frame->AddInterval();

    EXPECT_TRUE(interval);

    frame->Seal();

    EXPECT_EQ(frame->GetStatus(), RHIGPUTimingStatus::ePending);

    if (interval)
    {
        std::atomic<bool> released{false};

        std::atomic<bool> completed{false};

        std::thread producer(PublishAfterRelease, interval, &released, &completed);

        released.store(true, std::memory_order_release);

        while (!completed.load(std::memory_order_acquire))
        {
            const RHIGPUTimingStatus status = frame->GetStatus();

            EXPECT_TRUE(status == RHIGPUTimingStatus::ePending || status == RHIGPUTimingStatus::eAvailable);

            if (status == RHIGPUTimingStatus::eAvailable)
            {
                EXPECT_DOUBLE_EQ(frame->GetMicroseconds(), 150);
            }

            std::this_thread::yield();
        }

        producer.join();
    }

    EXPECT_EQ(frame->GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(frame->GetMicroseconds(), 150);
}
} // namespace
