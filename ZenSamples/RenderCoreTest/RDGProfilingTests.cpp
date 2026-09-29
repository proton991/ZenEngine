#include "Graphics/RenderCore/V2/RenderGraph/RDGMetrics.h"
#include <gtest/gtest.h>

namespace zen::rc
{
// Supply the recording outcome at the boundary exercised by the real graph tests.
struct RDGProfilingTestAccess
{
    static void Record(RDGMetrics& metrics, const RHIGPUTimingPtr& result, uint64_t frame)
    {
        metrics.m_capture = true;

        metrics.m_snapshot = {};

        metrics.m_snapshot.graph = "deferred_graph";

        metrics.m_snapshot.frameIndex = frame;

        metrics.m_snapshot.gpuTimingsEnabled = true;

        RDGNodeMetrics node;

        node.id = 7;

        node.name = "retained_pass";

        node.gpuStatus = RHIGPUTimingStatus::ePending;

        metrics.m_snapshot.nodes.push_back(node);

        metrics.m_gpuTimings = {result};

        metrics.m_executeStart = std::chrono::steady_clock::now();

        metrics.End();

        metrics.m_capture = false;
    }
};
} // namespace zen::rc

namespace
{
using namespace zen;

using namespace zen::rc;

struct RDGProfiling : testing::Test
{
    RDGMetrics metrics;

    HeapVector<RDGMetricsSnapshot> reports;

    void SetUp() override
    {
        metrics.SetSink({});

        metrics.SetGPUSink([this](const RDGMetricsSnapshot& sample) { reports.push_back(sample); });
    }
};

TEST_F(RDGProfiling, DeferredResultsRetainIdentityAndPublishOnce)
{
    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RDGProfilingTestAccess::Record(metrics, result, 42);

    metrics.CollectGPUResults();

    EXPECT_TRUE(reports.empty());

    EXPECT_EQ(metrics.GetPendingGPUCaptureCount(), 1u);

    result->Publish(RHIGPUTimingStatus::eAvailable, 12.5);

    metrics.CollectGPUResults();

    ASSERT_EQ(reports.size(), 1u);

    EXPECT_EQ(reports[0].frameIndex, 42u);

    EXPECT_EQ(reports[0].graph, "deferred_graph");

    EXPECT_EQ(reports[0].nodes[0].id, 7);

    EXPECT_EQ(reports[0].nodes[0].name, "retained_pass");

    EXPECT_EQ(reports[0].nodes[0].gpuStatus, RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(reports[0].nodes[0].gpuUs, 12.5);

    EXPECT_EQ(metrics.GetLastSnapshot().nodes[0].gpuStatus, RHIGPUTimingStatus::ePending);

    metrics.CollectGPUResults();

    EXPECT_EQ(reports.size(), 1u);
}

TEST_F(RDGProfiling, OverflowDropsOldCaptureWithoutCancellingNativeOwnership)
{
    RDGMetricsOptions options = metrics.GetOptions();

    options.maxPendingGPUCaptures = 1;

    metrics.Configure(options);

    RHIGPUTimingPtr old = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RHIGPUTimingPtr next = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RDGProfilingTestAccess::Record(metrics, old, 1);

    RDGProfilingTestAccess::Record(metrics, next, 2);

    ASSERT_EQ(reports.size(), 1u);

    EXPECT_EQ(reports[0].frameIndex, 1u);

    EXPECT_EQ(reports[0].nodes[0].gpuStatus, RHIGPUTimingStatus::eDropped);

    EXPECT_EQ(old->GetStatus(), RHIGPUTimingStatus::ePending);

    EXPECT_EQ(metrics.GetPendingGPUCaptureCount(), 1u);

    old->Publish(RHIGPUTimingStatus::eAvailable, 99);

    next->Publish(RHIGPUTimingStatus::eAvailable, 4);

    metrics.CollectGPUResults();

    ASSERT_EQ(reports.size(), 2u);

    EXPECT_EQ(reports[1].frameIndex, 2u);

    EXPECT_DOUBLE_EQ(reports[1].nodes[0].gpuUs, 4);

    EXPECT_EQ(metrics.GetPendingGPUCaptureCount(), 0u);
}

TEST_F(RDGProfiling, ReadyCapturesBypassPendingOnesAndFinalAbandonmentIsExplicit)
{
    RHIGPUTimingPtr pending = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RHIGPUTimingPtr unsupported = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RHIGPUTimingPtr discarded = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RDGProfilingTestAccess::Record(metrics, pending, 1);

    RDGProfilingTestAccess::Record(metrics, unsupported, 2);

    RDGProfilingTestAccess::Record(metrics, discarded, 3);

    unsupported->Publish(RHIGPUTimingStatus::eUnsupported);

    discarded->Publish(RHIGPUTimingStatus::eDiscarded);

    metrics.CollectGPUResults();

    ASSERT_EQ(reports.size(), 2u);

    EXPECT_EQ(reports[0].nodes[0].gpuStatus, RHIGPUTimingStatus::eUnsupported);

    EXPECT_EQ(reports[1].nodes[0].gpuStatus, RHIGPUTimingStatus::eDiscarded);

    metrics.CollectGPUResults(true);

    ASSERT_EQ(reports.size(), 3u);

    EXPECT_EQ(reports[2].frameIndex, 1u);

    EXPECT_EQ(reports[2].nodes[0].gpuStatus, RHIGPUTimingStatus::eDropped);

    EXPECT_EQ(pending->GetStatus(), RHIGPUTimingStatus::ePending);

    EXPECT_EQ(metrics.GetPendingGPUCaptureCount(), 0u);
}

TEST_F(RDGProfiling, RollbackCopiesCannotResurrectDeliveredCaptures)
{
    RHIGPUTimingPtr result = MakeShared<RHIGPUTimingResult, MultiThreadCounter>();

    RDGProfilingTestAccess::Record(metrics, result, 8);

    RDGMetrics checkpoint = metrics;

    result->Publish(RHIGPUTimingStatus::eAvailable, 0);

    metrics.CollectGPUResults();

    metrics = std::move(checkpoint);

    metrics.CollectGPUResults();

    ASSERT_EQ(reports.size(), 1u);

    EXPECT_EQ(reports[0].nodes[0].gpuStatus, RHIGPUTimingStatus::eAvailable);

    EXPECT_DOUBLE_EQ(reports[0].nodes[0].gpuUs, 0);

    EXPECT_EQ(metrics.GetPendingGPUCaptureCount(), 0u);
}
} // namespace
