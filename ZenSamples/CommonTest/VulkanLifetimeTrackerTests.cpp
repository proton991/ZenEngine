#include "Graphics/VulkanRHI/VulkanLifetimeTracker.h"
#include <gtest/gtest.h>

namespace
{
using namespace zen;

void CountDestruction(void* resource)
{
    ++*static_cast<uint32_t*>(resource);
}

TEST(VulkanLifetimeTrackerTest, RetirementWaitsForEveryRecording)
{
    VulkanLifetimeTracker tracker;
    const uint64_t id  = tracker.Create();
    uint32_t destroyed = 0;
    tracker.RetainRecording(id);
    tracker.RetainRecording(id);
    tracker.Retire(id, &destroyed, CountDestruction);
    EXPECT_EQ(destroyed, 0u);
    tracker.ReleaseRecordings(MakeVecView(&id, 1));
    tracker.Collect();
    EXPECT_TRUE(tracker.HasRecordings(id));
    EXPECT_FALSE(tracker.IsComplete(id));
    EXPECT_EQ(destroyed, 0u);
    tracker.ReleaseRecordings(MakeVecView(&id, 1));
    tracker.Collect();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(tracker.GetTrackedCount(), 0u);
    tracker.Destroy();
    EXPECT_EQ(destroyed, 1u);
}

TEST(VulkanLifetimeTrackerTest, CompletionKeepsActiveOwnerAvailableForAnotherRecording)
{
    VulkanLifetimeTracker tracker;
    const uint64_t id = tracker.Create();
    for (uint32_t replay = 0; replay < 3; ++replay)
    {
        EXPECT_TRUE(tracker.IsComplete(id));
        tracker.RetainRecording(id);
        EXPECT_FALSE(tracker.IsComplete(id));
        tracker.ReleaseRecordings(MakeVecView(&id, 1));
        tracker.Collect();
        EXPECT_EQ(tracker.GetTrackedCount(), 1u);
    }
    tracker.Retire(id);
    EXPECT_EQ(tracker.GetTrackedCount(), 0u);
}

TEST(VulkanLifetimeTrackerTest, IDsRemainDistinctAcrossTrackersAndReinitialization)
{
    VulkanLifetimeTracker first, second;
    const uint64_t oldId     = first.Create();
    const uint64_t foreignId = second.Create();
    EXPECT_GT(foreignId, oldId);
    EXPECT_FALSE(first.IsComplete(foreignId));
    EXPECT_FALSE(second.IsComplete(oldId));
    first.Retire(oldId);
    first.Destroy();
    const uint64_t nextId = first.Create();
    EXPECT_GT(nextId, foreignId);
    EXPECT_FALSE(first.IsComplete(oldId));
    first.Retire(nextId);
    second.Retire(foreignId);
}

TEST(VulkanLifetimeTrackerTest, CompletedEntriesDoNotAccumulateDuringStreaming)
{
    VulkanLifetimeTracker tracker;
    for (uint32_t cycle = 0; cycle < 32; ++cycle)
    {
        HeapVector<uint64_t> ids;
        for (uint32_t i = 0; i < 128; ++i)
        {
            const uint64_t id = tracker.Create();
            tracker.RetainRecording(id);
            ids.push_back(id);
            tracker.Retire(id);
        }
        EXPECT_EQ(tracker.GetTrackedCount(), 128u);
        tracker.ReleaseRecordings(ids);
        tracker.Collect();
        EXPECT_EQ(tracker.GetTrackedCount(), 0u);
    }
}

TEST(VulkanLifetimeTrackerTest, TerminalDestroyReleasesUncertainRetiredResourcesOnce)
{
    VulkanLifetimeTracker tracker;
    const uint64_t id  = tracker.Create();
    uint32_t destroyed = 0;
    tracker.RetainRecording(id);
    tracker.Retire(id, &destroyed, CountDestruction);
    tracker.Collect();
    EXPECT_EQ(destroyed, 0u);
    tracker.Destroy();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(tracker.GetTrackedCount(), 0u);
    // Workloads are destroyed after allocator teardown during device shutdown.
    tracker.ReleaseRecordings(MakeVecView(&id, 1));
    tracker.Destroy();
    EXPECT_EQ(destroyed, 1u);
}

TEST(VulkanLifetimeTrackerTest, ResourceCleanupCanRetireAnotherOwner)
{
    VulkanLifetimeTracker tracker;
    const uint64_t first  = tracker.Create();
    const uint64_t second = tracker.Create();
    struct Cleanup
    {
        VulkanLifetimeTracker& tracker;
        uint64_t next;
        uint32_t destroyed{0};
    } cleanup{tracker, second};
    tracker.Retire(first, &cleanup, [](void* resource) {
        auto& cleanup = *static_cast<Cleanup*>(resource);
        ++cleanup.destroyed;
        cleanup.tracker.Retire(cleanup.next, &cleanup.destroyed, CountDestruction);
    });
    EXPECT_EQ(cleanup.destroyed, 2u);
    EXPECT_EQ(tracker.GetTrackedCount(), 0u);
}
} // namespace
