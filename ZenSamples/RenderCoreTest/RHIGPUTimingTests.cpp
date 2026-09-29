#include "Graphics/RHI/RHIGPUTiming.h"
#include <gtest/gtest.h>
#include <limits>
#include <thread>

namespace
{
using namespace zen;

TEST(RHIGPUTiming, PublishesOnceAndKeepsUnavailableDistinctFromZero)
{
    RHIGPUTimingResult result;

    EXPECT_EQ(result.GetStatus(), RHIGPUTimingStatus::ePending);

    EXPECT_FALSE(result.Publish(RHIGPUTimingStatus::ePending));

    EXPECT_TRUE(result.Publish(RHIGPUTimingStatus::eAvailable, 0));

    EXPECT_FALSE(result.Publish(RHIGPUTimingStatus::eDiscarded));

    EXPECT_EQ(result.GetStatus(), RHIGPUTimingStatus::eAvailable);

    EXPECT_EQ(result.GetMicroseconds(), 0);

    RHIGPUTimingResult invalid;

    invalid.Publish(RHIGPUTimingStatus::eAvailable, std::numeric_limits<double>::infinity());

    EXPECT_EQ(invalid.GetStatus(), RHIGPUTimingStatus::eError);
}

TEST(RHIGPUTiming, ConcurrentPublicationReleasesTheMatchingValue)
{
    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    std::thread available([result] { result->Publish(RHIGPUTimingStatus::eAvailable, 123.5); });

    std::thread discarded([result] { result->Publish(RHIGPUTimingStatus::eDiscarded); });

    while (result->GetStatus() == RHIGPUTimingStatus::ePending)
    {
        std::this_thread::yield();
    }

    const RHIGPUTimingStatus status = result->GetStatus();

    EXPECT_EQ(result->GetMicroseconds(), status == RHIGPUTimingStatus::eAvailable ? 123.5 : 0);

    available.join();

    discarded.join();

    EXPECT_TRUE(result.Unique());
}

TEST(RHIGPUTiming, ConvertsTimestampPeriodAndWraparound)
{
    double time = 0;

    EXPECT_TRUE(ConvertGPUTimestampsToMicroseconds(250, 5, 8, 1000, time));

    EXPECT_DOUBLE_EQ(time, 11);

    EXPECT_TRUE(ConvertGPUTimestampsToMicroseconds(UINT64_MAX - 4, 3, 64, 0.5, time));

    EXPECT_DOUBLE_EQ(time, 0.004);

    EXPECT_TRUE(ConvertGPUTimestampsToMicroseconds(0x100000000ULL, 0x100000064ULL, 36, 2, time));

    EXPECT_DOUBLE_EQ(time, 0.2);

    EXPECT_FALSE(ConvertGPUTimestampsToMicroseconds(0, 1, 0, 1, time));

    EXPECT_FALSE(ConvertGPUTimestampsToMicroseconds(0, 1, 65, 1, time));

    EXPECT_FALSE(ConvertGPUTimestampsToMicroseconds(0, 1, 64, -1, time));
}
} // namespace
