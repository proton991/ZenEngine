#include "Editor/Model/RenderGraphCaptureState.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
void Publish(RenderGraphCaptureState&              state,
             uint64_t                              frame,
             std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
{
    rc::RDGDebugSnapshot* pending = state.RequestCapture(now);
    ASSERT_NE(pending, nullptr);
    pending->accepted = true;
    pending->frame    = frame;
    state.CompleteCapture(true, {}, "PBR", {});
}
} // namespace

TEST(RenderGraphCapture, LiveRevisionsCoalesceWhileManualRefreshIsImmediate)
{
    RenderGraphCaptureState  state;
    RenderGraphCaptureSource source{false, 1, 1, "scene"};
    state.Synchronize(source, {}, false);
    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    Publish(state, 10, start);
    EXPECT_EQ(state.RequestCapture(start), nullptr);
    state.Refresh();
    Publish(state, 11, start);
    uint64_t staleSerial = 0;
    for (int elapsed = 1; elapsed < 250; ++elapsed)
    {
        ++source.revision;
        state.Synchronize(source, {}, false);
        EXPECT_TRUE(state.IsStale());
        EXPECT_EQ(state.RequestCapture(start + std::chrono::milliseconds(elapsed)), nullptr);
        if (elapsed == 1)
        {
            staleSerial = state.GetChangeSerial();
        }
        EXPECT_EQ(state.GetChangeSerial(), staleSerial);
    }
    Publish(state, 12, start + std::chrono::milliseconds(250));
    EXPECT_EQ(state.GetCapturedSource().revision, source.revision);
    EXPECT_FALSE(state.IsStale());
}

TEST(RenderGraphCapture, FrozenSnapshotSurvivesPlayAndSceneChangesUntilRefreshed)
{
    RenderGraphCaptureState        state;
    const RenderGraphCaptureSource original{false, 1, 1, "first"};
    state.Synchronize(original, {}, false);
    Publish(state, 10);
    state.SetFrozen(true);
    for (const RenderGraphCaptureSource& source :
         {RenderGraphCaptureSource{true, 1, 1, "first"}, RenderGraphCaptureSource{false, 2, 2, "second"}})
    {
        state.Synchronize(source, {}, false);
        EXPECT_TRUE(state.GetSnapshot().accepted);
        EXPECT_EQ(state.GetSnapshot().frame, 10u);
        EXPECT_EQ(state.GetCapturedSource(), original);
        EXPECT_TRUE(state.IsStale());
        EXPECT_EQ(state.RequestCapture(), nullptr);
    }
    state.Refresh();
    Publish(state, 11);
    EXPECT_TRUE(state.IsFrozen());
    EXPECT_EQ(state.GetCapturedSource().path, "second");
    EXPECT_FALSE(state.IsStale());
}

TEST(RenderGraphCapture, FreezeKeepsRevisionUntilExplicitRefresh)
{
    RenderGraphCaptureState  state;
    RenderGraphCaptureSource source{false, 1, 1, "scene"};
    state.Synchronize(source, {}, false);
    Publish(state, 10);
    state.SetFrozen(true);
    ++source.revision;
    state.Synchronize(source, {}, true);
    EXPECT_EQ(state.RequestCapture(), nullptr);
    EXPECT_EQ(state.GetCapturedSource().revision, 1u);
    EXPECT_TRUE(state.IsAwaitingApply());
    state.Refresh();
    Publish(state, 11);
    EXPECT_TRUE(state.IsFrozen());
    EXPECT_EQ(state.GetCapturedSource().revision, 2u);
}

TEST(RenderGraphCapture, PausedSceneDoesNotConsumeRefreshAndResumeCaptures)
{
    RenderGraphCaptureState        state;
    const RenderGraphCaptureSource source{true, 1, 1, "scene"};
    state.Synchronize(source, {}, false);
    Publish(state, 1);
    state.Synchronize(source, "Paused", false);
    state.Refresh();
    EXPECT_EQ(state.RequestCapture(), nullptr);
    EXPECT_TRUE(state.IsStale());
    state.Synchronize(source, {}, false);
    Publish(state, 2);
    EXPECT_FALSE(state.IsStale());
}

TEST(RenderGraphCapture, FailureRetainsSnapshotAndSuccessfulRetryClearsError)
{
    RenderGraphCaptureState state;
    state.Synchronize({false, 1, 1, "scene"}, {}, false);
    Publish(state, 10);
    state.Refresh();
    ASSERT_NE(state.RequestCapture(), nullptr);
    state.CompleteCapture(false, "Rejected", {}, {});
    EXPECT_EQ(state.GetSnapshot().frame, 10u);
    EXPECT_EQ(state.GetError(), "Rejected");
    EXPECT_TRUE(state.IsStale());
    state.Refresh();
    Publish(state, 12);
    EXPECT_TRUE(state.GetError().empty());
}

TEST(RenderGraphCapture, ChangedSourceRejectsInFlightSnapshotAndClearsPreviousScene)
{
    RenderGraphCaptureState state;
    state.Synchronize({false, 1, 1, "first"}, {}, false);
    Publish(state, 1);
    state.Refresh();
    rc::RDGDebugSnapshot* pending = state.RequestCapture();
    ASSERT_NE(pending, nullptr);
    pending->accepted = true;
    state.Synchronize({true, 2, 3, "second"}, {}, false);
    state.CompleteCapture(true, {}, "GI", {});
    EXPECT_FALSE(state.GetSnapshot().accepted);
    Publish(state, 2);
    EXPECT_TRUE(state.GetCapturedSource().play);
    EXPECT_EQ(state.GetCapturedSource().scene, 2u);
}

TEST(RenderGraphCapture, FreezeWithoutSnapshotCapturesOnceWhenPausedSourceResumes)
{
    RenderGraphCaptureState state;
    state.Synchronize({false, 1, 1, "scene"}, {}, false);
    Publish(state, 1);
    const RenderGraphCaptureSource play{true, 1, 1, "scene"};
    state.Synchronize(play, "Paused", false);
    ASSERT_FALSE(state.GetSnapshot().accepted);
    state.SetFrozen(true);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    EXPECT_EQ(state.RequestCapture(now), nullptr);
    state.Synchronize(play, {}, false);
    Publish(state, 2, now);
    EXPECT_TRUE(state.IsFrozen());
    EXPECT_EQ(state.GetCapturedSource(), play);
    EXPECT_EQ(state.GetSnapshot().frame, 2u);
    EXPECT_EQ(state.RequestCapture(now + std::chrono::seconds(1)), nullptr);
}

TEST(RenderGraphCapture, FrozenInitialCaptureRetriesFailuresAtTheNormalRate)
{
    RenderGraphCaptureState state;
    state.Synchronize({false, 1, 1, "scene"}, {}, false);
    state.SetFrozen(true);
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    ASSERT_NE(state.RequestCapture(now), nullptr);
    state.CompleteCapture(false, "Declined", {}, {});
    EXPECT_FALSE(state.GetSnapshot().accepted);
    EXPECT_EQ(state.RequestCapture(now + std::chrono::milliseconds(249)), nullptr);
    Publish(state, 1, now + std::chrono::milliseconds(250));
    EXPECT_TRUE(state.GetError().empty());
    EXPECT_TRUE(state.IsFrozen());
    EXPECT_EQ(state.RequestCapture(now + std::chrono::seconds(1)), nullptr);
}
} // namespace zen::editor
