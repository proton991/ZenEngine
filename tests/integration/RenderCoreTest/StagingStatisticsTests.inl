TEST_F(RenderCoreTest, StagingStatisticsTrackPaddingRetirementAndPoolReuse)
{
    rhi->bufferAllocationPadding = 8;
    StagingBufferManager manager(32, 64);
    StagingAllocation    first, second, oversized, blocked;
    ASSERT_EQ(manager.Allocate(3, 1, &first), StagingFlushAction::eNone);
    ASSERT_EQ(manager.Allocate(5, 16, &second), StagingFlushAction::eNone);
    EXPECT_EQ(second.offset, 16u);
    StagingPoolStatistics statistics = manager.GetStatistics();
    EXPECT_EQ(statistics.capacityBytes, 32u);
    EXPECT_EQ(statistics.occupiedBytes, 21u);
    EXPECT_EQ(statistics.allocationBytes, 40u);
    EXPECT_TRUE(statistics.allocationBytesAvailable);
    EXPECT_EQ(statistics.blockCount, 1u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);

    manager.Release(first, {{{2, 0, 0}}});
    manager.Release(second, {{{3, 0, 0}}});
    manager.Reclaim();
    EXPECT_EQ(manager.GetStatistics().occupiedBytes, 21u);
    rhi->completed = {3, 0, 0};
    manager.Reclaim();
    EXPECT_EQ(manager.GetStatistics().occupiedBytes, 0u);
    EXPECT_EQ(manager.GetStatistics().peakOccupiedBytes, 21u);
    EXPECT_EQ(manager.GetStatistics().capacityBytes, 32u);

    ASSERT_EQ(manager.Allocate(48, 1, &oversized), StagingFlushAction::eNone);
    statistics = manager.GetStatistics();
    EXPECT_EQ(statistics.capacityBytes, 48u);
    EXPECT_EQ(statistics.allocationBytes, 56u);
    EXPECT_EQ(statistics.peakCapacityBytes, 48u);
    EXPECT_EQ(statistics.peakAllocationBytes, 56u);
    EXPECT_EQ(statistics.peakBlockCount, 1u);
    EXPECT_EQ(manager.Allocate(1, 1, &blocked), StagingFlushAction::eFlush);
    EXPECT_EQ(manager.GetStatistics().allocationFlushRequests, 1u);
    manager.ResetStatistics();
    statistics = manager.GetStatistics();
    EXPECT_EQ(statistics.allocationFlushRequests, 0u);
    EXPECT_EQ(statistics.peakOccupiedBytes, 48u);
    EXPECT_EQ(statistics.peakAllocationBytes, 56u);
    EXPECT_TRUE(rhi->submissionWaits.empty());
    manager.Release(oversized, {});
    manager.Destroy();
    statistics = manager.GetStatistics();
    EXPECT_EQ(statistics.capacityBytes, 0u);
    EXPECT_EQ(statistics.allocationBytes, 0u);
    EXPECT_EQ(statistics.occupiedBytes, 0u);
    EXPECT_EQ(statistics.peakOccupiedBytes, 48u);
}

TEST_F(RenderCoreTest, StagingStatisticsSeparateAllocationFailureAndUnavailableNativeSizes)
{
    StagingBufferManager manager(16, 16);
    StagingAllocation    allocation;
    rhi->failBufferCreationAt = rhi->bufferCreations + 1;
    EXPECT_EQ(manager.Allocate(4, 1, &allocation), StagingFlushAction::eFailed);
    EXPECT_EQ(manager.GetStatistics().allocationFailures, 1u);
    EXPECT_EQ(manager.GetStatistics().peakCapacityBytes, 0u);
    EXPECT_TRUE(manager.GetStatistics().allocationBytesAvailable);
    ASSERT_EQ(manager.Allocate(4, 1, &allocation), StagingFlushAction::eNone);
    EXPECT_FALSE(manager.GetStatistics().allocationBytesAvailable);
    EXPECT_EQ(manager.GetStatistics().capacityBytes, 16u);
    manager.Release(allocation, {});
    manager.Destroy();
    EXPECT_FALSE(manager.GetStatistics().allocationBytesAvailable);
    manager.ResetStatistics();
    EXPECT_TRUE(manager.GetStatistics().allocationBytesAvailable);
    EXPECT_EQ(manager.GetStatistics().allocationFailures, 0u);
    EXPECT_EQ(manager.GetStatistics().peakCapacityBytes, 0u);
}

TEST_F(RenderCoreTest, StagingStatisticsKeepFailedWaitOccupancyUntilConfirmedCompletion)
{
    rhi->bufferAllocationPadding = 0;
    StagingBufferManager manager(16, 16);
    StagingAllocation    allocation;
    ASSERT_EQ(manager.Allocate(16, 1, &allocation), StagingFlushAction::eNone);
    manager.Release(allocation, {{{1, 0, 1}}});
    rhi->submitted          = {3, 0, 3};
    rhi->failSubmissionWait = true;
    EXPECT_FALSE(manager.WaitForSubmittedAllocations());
    EXPECT_EQ(manager.GetStatistics().waitCalls, 1u);
    EXPECT_EQ(manager.GetStatistics().waitFailures, 1u);
    EXPECT_EQ(manager.GetStatistics().occupiedBytes, 16u);
    const uint64_t failedWaitNS = manager.GetStatistics().waitCPUTimeNS;
    rhi->failSubmissionWait     = false;
    ASSERT_TRUE(manager.WaitForSubmittedAllocations());
    EXPECT_EQ(manager.GetStatistics().waitCalls, 2u);
    EXPECT_EQ(manager.GetStatistics().waitFailures, 1u);
    EXPECT_EQ(manager.GetStatistics().occupiedBytes, 0u);
    EXPECT_GE(manager.GetStatistics().waitCPUTimeNS, failedWaitNS);
    EXPECT_EQ(rhi->deviceIdleWaits, 0u);
    EXPECT_EQ(rhi->completed, (std::array<uint64_t, 3>{1, 0, 1}));
    manager.Destroy();
}
