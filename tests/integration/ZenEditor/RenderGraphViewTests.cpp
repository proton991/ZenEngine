#include "Editor/ImGui/RenderGraphView.h"
#include "Editor/ImGui/EditorTheme.h"
#include "RenderGraphDrawing.h"
#include "imgui_internal.h"
#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>

namespace zen::editor
{
namespace
{
rc::RDGDebugSnapshot MakeLayoutGraph()
{
    rc::RDGDebugSnapshot graph;
    for (int32_t id = 0; id < 4; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "Repeated name";
        pass.live  = id < 3;
        pass.order = pass.live ? uint32_t(id) : UINT32_MAX;
        graph.passes.push_back(std::move(pass));
    }
    graph.dependencies.push_back({0, 1, 0, 1, rc::RDGDependencyReason::eVersionProducer});
    graph.dependencies.push_back({0, 1, 1, 2, rc::RDGDependencyReason::eWriteAfterWrite});
    graph.dependencies.push_back({1, 2, 1, 2, rc::RDGDependencyReason::eVersionProducer});
    return graph;
}

rc::RDGDebugSnapshot MakeDenseGraph(int32_t count)
{
    rc::RDGDebugSnapshot graph;
    const int32_t        strides[] = {1, 3, 7, 15, 31, 63};
    for (int32_t id = 0; id < count; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "Stage " + std::to_string(id);
        pass.live  = true;
        pass.order = uint32_t(id);
        graph.passes.push_back(std::move(pass));
        for (int32_t stride : strides)
        {
            if (id + stride < count)
            {
                graph.dependencies.push_back({id, id + stride});
            }
        }
    }
    return graph;
}

class GraphViewTest : public testing::Test
{
protected:
    void SetUp() override
    {
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime   = 1.0f / 60;
        ApplyEditorTheme(1);
        view.InitFonts(1);
        unsigned char* pixels = nullptr;
        int            width  = 0;
        int            height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
        state.Synchronize({false, 1, 1, "Scene.gltf"}, {}, false);
        Publish(MakeLayoutGraph());
    }

    void TearDown() override
    {
        ImGui::DestroyContext();
    }

    void Publish(const rc::RDGDebugSnapshot& graph)
    {
        state.Refresh();
        rc::RDGDebugSnapshot* pending = state.RequestCapture();
        ASSERT_NE(pending, nullptr);
        *pending          = graph;
        pending->accepted = true;
        state.CompleteCapture(true, {}, "GI -> GI", {});
    }

    void Draw(ImVec2 display = ImVec2(900, 600), uint32_t frames = 1)
    {
        ImGui::GetIO().DisplaySize = display;
        for (uint32_t frame = 0; frame < frames; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(display);
            ImGui::Begin("Graph interaction test", nullptr, ImGuiWindowFlags_NoDecoration);
            const ImGuiID canvasId = ImGui::GetID("GraphCanvas");
            view.Draw(state);
            for (const ImGuiWindow* child : ImGui::GetCurrentWindow()->DC.ChildWindows)
            {
                if (child->ChildId == canvasId)
                {
                    canvasOrigin = child->DC.CursorStartPos;
                }
            }
            ImGui::End();
            ImGui::Render();
        }
    }

    RenderGraphView         view;
    RenderGraphCaptureState state;
    ImVec2                  canvasOrigin;
};
} // namespace

TEST_F(GraphViewTest, ResizeAfterPanAndZoomPreservesTheViewedGraphPoint)
{
    Draw(ImVec2(900, 600), 20);
    ImGuiIO&     io = ImGui::GetIO();
    const ImVec2 mouse(canvasOrigin.x + 160, canvasOrigin.y + 150);
    io.AddMousePosEvent(mouse.x, mouse.y);
    Draw();
    const float initialZoom = view.GetNavigation().zoom;
    io.AddMouseWheelEvent(0, 2);
    Draw();
    EXPECT_GT(view.GetNavigation().zoom, initialZoom);
    const ImVec2 beforePan = view.GetNavigation().pan;
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
    Draw();
    io.AddMousePosEvent(mouse.x + 53, mouse.y + 37);
    Draw();
    io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
    Draw();
    EXPECT_NEAR(view.GetNavigation().pan.x - beforePan.x, 53, 0.01f);
    EXPECT_NEAR(view.GetNavigation().pan.y - beforePan.y, 37, 0.01f);
    const RenderGraphNavigation before = view.GetNavigation();
    const ImVec2                centre((view.GetCanvasSize().x * 0.5f - before.pan.x) / before.zoom,
                                       (view.GetCanvasSize().y * 0.5f - before.pan.y) / before.zoom);
    const ImVec2                displays[] = {ImVec2(2000, 1100), ImVec2(1100, 700), ImVec2(900, 600)};
    for (ImVec2 display : displays)
    {
        Draw(display, 20);
        const RenderGraphNavigation& navigation = view.GetNavigation();
        EXPECT_FALSE(navigation.IsActive());
        EXPECT_FLOAT_EQ(navigation.zoom, before.zoom);
        EXPECT_NEAR((view.GetCanvasSize().x * 0.5f - navigation.pan.x) / navigation.zoom, centre.x, 0.01f);
        EXPECT_NEAR((view.GetCanvasSize().y * 0.5f - navigation.pan.y) / navigation.zoom, centre.y, 0.01f);
    }
}

TEST_F(GraphViewTest, AutoFitKeepsIncrementalPositionsAndDoesNotPulseOnTemporaryWork)
{
    // A conditional second producer occupies a retained column. Removing it must not
    // run another full layout that recentres the surviving producer/consumer chain.
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.passes[3].name       = "Temporary GI preparation";
    graph.passes[3].live       = true;
    graph.passes[3].order      = 0;
    graph.dependencies.push_back({3, 1});
    Publish(graph);
    Draw(ImVec2(1600, 900), 20);
    const HeapVector<ImVec2>    positions = view.GetLayout().positions;
    const RenderGraphNavigation before    = view.GetNavigation();
    for (uint32_t capture = 0; capture < 4; ++capture)
    {
        graph.passes[3].live = capture % 2 != 0;
        Publish(graph);
        Draw(ImVec2(1600, 900), 20);
        for (uint32_t id = 0; id < 3; ++id)
        {
            EXPECT_FLOAT_EQ(view.GetLayout().positions[id].x, positions[id].x);
            EXPECT_FLOAT_EQ(view.GetLayout().positions[id].y, positions[id].y);
        }
        EXPECT_FLOAT_EQ(view.GetNavigation().zoom, before.zoom);
        EXPECT_FLOAT_EQ(view.GetNavigation().pan.x, before.pan.x);
        EXPECT_FLOAT_EQ(view.GetNavigation().pan.y, before.pan.y);
        EXPECT_FALSE(view.IsAnimating());
    }
}

TEST_F(GraphViewTest, AutoFitExpandsOnceAndKeepsTheScaleWhenWorkDisappears)
{
    Draw(ImVec2(900, 600), 20);
    const float          initialZoom = view.GetNavigation().zoom;
    rc::RDGDebugSnapshot graph       = MakeLayoutGraph();
    graph.passes[3].live             = true;
    graph.passes[3].order            = 3;
    graph.dependencies.push_back({2, 3});
    for (int32_t id = 4; id < 12; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "Preparation " + std::to_string(id);
        pass.live  = true;
        pass.order = uint32_t(id);
        graph.passes.push_back(std::move(pass));
        graph.dependencies.push_back({id - 1, id});
    }
    Publish(graph);
    Draw(ImVec2(900, 600), 20);
    const RenderGraphNavigation expanded = view.GetNavigation();
    EXPECT_LT(expanded.zoom, initialZoom);
    EXPECT_LE(expanded.pan.y + view.GetLayout().extent.y * expanded.zoom, view.GetCanvasSize().y);
    Publish(MakeLayoutGraph());
    Draw(ImVec2(900, 600), 20);
    EXPECT_FLOAT_EQ(view.GetNavigation().zoom, expanded.zoom);
    EXPECT_FLOAT_EQ(view.GetNavigation().pan.x, expanded.pan.x);
    EXPECT_FLOAT_EQ(view.GetNavigation().pan.y, expanded.pan.y);
}

TEST_F(GraphViewTest, AutoFitZoomsBackInOnceASmallerGraphStaysStable)
{
    // The first scene frame can carry a large one-off environment bake. The Fit chosen
    // for it must not leave the steady graph that follows as a speck at the top.
    rc::RDGDebugSnapshot bake = MakeDenseGraph(64);
    bake.frame                = 1;
    Publish(bake);
    Draw(ImVec2(900, 600), 20);
    const float          bakeZoom = view.GetNavigation().zoom;
    rc::RDGDebugSnapshot steady   = MakeLayoutGraph();
    for (uint64_t frame = 2; frame <= 5; ++frame)
    {
        steady.frame = frame;
        Publish(steady);
        Draw(ImVec2(900, 600), 20);
        // The replacing capture and the first unchanged one keep the camera steady.
        if (frame <= 3)
        {
            EXPECT_FLOAT_EQ(view.GetNavigation().zoom, bakeZoom) << frame;
        }
    }
    const RenderGraphNavigation& navigation = view.GetNavigation();
    const float                  width      = view.GetLayout().extent.x * navigation.zoom * EditorScale();
    EXPECT_GT(navigation.zoom, bakeZoom * 1.5f);
    EXPECT_FALSE(navigation.IsActive());
    EXPECT_NEAR(navigation.pan.x + width * 0.5f, view.GetCanvasSize().x * 0.5f, 1.0f);
}

TEST_F(GraphViewTest, OverviewGridDoesNotFloodTheDrawList)
{
    const ImVec2 display(2400, 1400);
    Draw(display, 20);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(canvasOrigin.x + 160, canvasOrigin.y + 150);
    Draw(display);
    io.AddMouseWheelEvent(0, std::log(0.41f / view.GetNavigation().zoom) / std::log(1.15f));
    Draw(display);
    EXPECT_NEAR(view.GetNavigation().zoom, 0.41f, 0.001f);
    // The old near-transparent grid alone emitted over 150,000 vertices here.
    EXPECT_LT(ImGui::GetDrawData()->TotalVtxCount, 20000);
}

// Opt-in timing probe: same deterministic workload before/after routing changes.
// No wall-time assertion in the regular test suite.
TEST(RenderGraphLayout, DISABLED_DenseRoutingBenchmark)
{
    const rc::RDGDebugSnapshot graph = MakeDenseGraph(128);
    for (uint32_t run = 0; run < 3; ++run)
    {
        RenderGraphLayout                           layout;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ASSERT_TRUE(layout.Update(graph, false));
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::printf("Dense routing: %zu passes, %zu edges, %.3f ms\n", graph.passes.size(), graph.dependencies.size(),
                    milliseconds);
    }
}

TEST(RenderGraphLayout, DenseChannelsClearCardsAndDoNotShareOccupiedSpans)
{
    rc::RDGDebugSnapshot graph = MakeDenseGraph(32);
    RenderGraphLayout    layout;
    // Exercise both the full ordering sweeps and retained positions after a change.
    for (uint32_t capture = 0; capture < 2; ++capture)
    {
        graph.passes[0].name = "Producer " + std::to_string(capture);
        ASSERT_TRUE(layout.Update(graph, false));
        uint32_t channels = 0;
        for (size_t index = 0; index < layout.edges.size(); ++index)
        {
            const RenderGraphEdge& edge = layout.edges[index];
            if (edge.route.size() > 2)
            {
                ++channels;
                const float x      = edge.route[1].x;
                const float top    = edge.route[1].y;
                const float bottom = edge.route[edge.route.size() - 2].y;
                for (size_t point = 1; point + 1 < edge.route.size(); ++point)
                {
                    EXPECT_FLOAT_EQ(edge.route[point].x, x);
                }
                for (const ImVec2& card : layout.positions)
                {
                    if (card.y < bottom && card.y + RenderGraphLayout::NodeHeight > top)
                    {
                        EXPECT_TRUE(x <= card.x - 8 || x >= card.x + RenderGraphLayout::NodeWidth + 8);
                    }
                }
                for (size_t other = index + 1; other < layout.edges.size(); ++other)
                {
                    const RenderGraphEdge& next = layout.edges[other];
                    if (next.route.size() > 2 && next.route[1].y < bottom && next.route[next.route.size() - 2].y > top)
                    {
                        EXPECT_GE(std::abs(x - next.route[1].x), 15.99f);
                    }
                }
            }
        }
        EXPECT_GT(channels, 20u);
    }
}

TEST(RenderGraphLayout, LayersFlowTopToBottomAndEdgesKeepEveryReason)
{
    const rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    RenderGraphLayout          layout;
    ASSERT_TRUE(layout.Update(graph, false));
    EXPECT_LT(layout.positions[0].y + RenderGraphLayout::NodeHeight, layout.positions[1].y);
    EXPECT_LT(layout.positions[1].y + RenderGraphLayout::NodeHeight, layout.positions[2].y);
    EXPECT_EQ(layout.positions[0].x, layout.positions[1].x);
    EXPECT_EQ(layout.positions[1].x, layout.positions[2].x);
    ASSERT_EQ(layout.edges.size(), 2u);
    EXPECT_EQ(layout.edges[0].reasons.size(), 2u);
    EXPECT_FALSE(layout.lanes[3]);
    EXPECT_TRUE(layout.Update(graph, true));
    EXPECT_TRUE(layout.lanes[3]);
    EXPECT_GT(layout.positions[3].x, layout.positions[0].x + RenderGraphLayout::NodeWidth);
    EXPECT_EQ(layout.edges.size(), 2u);
}

TEST(RenderGraphLayout, FrameChangesReuseLayoutButQueueChangesRebuildIt)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    RenderGraphLayout    layout;
    ASSERT_TRUE(layout.Update(graph, false));
    const ImVec2 previous = layout.positions[1];
    ++graph.frame;
    ++graph.generation;
    EXPECT_FALSE(layout.Update(graph, false));
    EXPECT_EQ(layout.positions[1].x, previous.x);
    graph.mode            = rc::RDGDebugExecutionMode::Grouped;
    graph.passes[1].queue = RHICommandContextType::eAsyncCompute;
    EXPECT_TRUE(layout.Update(graph, false));
    EXPECT_TRUE(layout.lanes[0]);
    EXPECT_TRUE(layout.lanes[1]);
    EXPECT_NE(layout.positions[1].x, previous.x);
    EXPECT_EQ(layout.positions[1].y, previous.y);
}

TEST(RenderGraphLayout, GroupOrderingSeparatesIndependentPasses)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.dependencies.clear();
    graph.groups.resize(2);
    graph.groups[0].id = 0;
    graph.groups[0].passes.push_back({0});
    graph.groups[1].id = 1;
    graph.groups[1].passes.push_back({1});
    graph.groups[1].predecessors.push_back(0);
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    EXPECT_LT(layout.positions[0].y + RenderGraphLayout::NodeHeight, layout.positions[1].y);
    ASSERT_EQ(layout.edges.size(), 1u);
    EXPECT_TRUE(layout.edges[0].ordering);
}

TEST(RenderGraphLayout, GroupOrderingMergesWithResourceReasonsForTheSamePassPair)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.groups.resize(2);
    graph.groups[0].passes.push_back({0});
    graph.groups[1].passes.push_back({1});
    graph.groups[1].predecessors.push_back(0);
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    ASSERT_EQ(layout.edges.size(), 2u);
    EXPECT_TRUE(layout.edges[0].ordering);
    EXPECT_EQ(layout.edges[0].reasons.size(), 2u);
}

TEST(RenderGraphLayout, InsertingAndRemovingConditionalPassesPreservesIdentityAndPositions)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    RenderGraphLayout    layout;
    ASSERT_TRUE(layout.Update(graph, false));
    const HeapVector<ImVec2>      previous = layout.positions;
    const HeapVector<std::string> keys     = layout.keys;
    const rc::RDGDebugSnapshot    original = graph;
    rc::RDGDebugPass              pick;
    pick.name  = "Pick";
    pick.live  = true;
    pick.id    = 0;
    pick.order = 0;
    graph.passes.insert(graph.passes.begin(), &pick, &pick + 1);
    for (size_t id = 1; id < graph.passes.size(); ++id)
    {
        graph.passes[id].id = int32_t(id);
        if (graph.passes[id].live)
        {
            ++graph.passes[id].order;
        }
    }
    for (rc::RDGDependency& edge : graph.dependencies)
    {
        edge.source      = int32_t(edge.source) + 1;
        edge.destination = int32_t(edge.destination) + 1;
    }
    // An independent conditional pass must not disturb valid retained cells.
    ASSERT_TRUE(layout.Update(graph, false));
    for (size_t id = 0; id < 3; ++id)
    {
        EXPECT_EQ(layout.keys[id + 1], keys[id]);
        EXPECT_EQ(layout.positions[id + 1].x, previous[id].x);
        EXPECT_EQ(layout.positions[id + 1].y, previous[id].y);
    }
    EXPECT_GT(layout.positions[0].x, previous[0].x + RenderGraphLayout::NodeWidth);
    EXPECT_EQ(layout.positions[0].y, previous[0].y);
    ASSERT_TRUE(layout.Update(original, false));
    for (size_t id = 0; id < 3; ++id)
    {
        EXPECT_EQ(layout.keys[id], keys[id]);
        EXPECT_EQ(layout.positions[id].x, previous[id].x);
        EXPECT_EQ(layout.positions[id].y, previous[id].y);
    }
}

TEST(RenderGraphLayout, ExpansionMovesConsumersAfterNewPredecessorsAndFitCompactsContraction)
{
    const rc::RDGDebugSnapshot original = MakeLayoutGraph();
    RenderGraphLayout          layout;
    ASSERT_TRUE(layout.Update(original, false));
    const HeapVector<ImVec2>      originalPositions = layout.positions;
    const HeapVector<std::string> originalKeys      = layout.keys;
    const float                   originalHeight    = layout.extent.y;
    rc::RDGDebugSnapshot          expanded          = original;
    // Model GI preparation/composition inserted before surviving UI/pick passes.
    for (int32_t id = 4; id < 7; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "GI stage " + std::to_string(id);
        pass.type  = rc::RDGNodeType::eComputePass;
        pass.live  = true;
        pass.order = uint32_t(id - 3);
        expanded.passes.push_back(std::move(pass));
    }
    expanded.passes[1].order = 4;
    expanded.passes[2].order = 5;
    expanded.dependencies    = {{0, 4}, {4, 5}, {5, 6}, {6, 1}, {1, 2}};
    ASSERT_TRUE(layout.Update(expanded, false));
    for (const RenderGraphEdge& edge : layout.edges)
    {
        EXPECT_LT(layout.positions[edge.source].y + RenderGraphLayout::NodeHeight, layout.positions[edge.destination].y);
    }
    for (size_t id = 0; id < originalKeys.size(); ++id)
    {
        EXPECT_EQ(layout.keys[id], originalKeys[id]);
    }
    EXPECT_EQ(layout.positions[0].x, originalPositions[0].x);
    EXPECT_EQ(layout.positions[0].y, originalPositions[0].y);
    EXPECT_GT(layout.positions[1].y, originalPositions[1].y);
    EXPECT_GT(layout.positions[2].y, originalPositions[2].y);

    // Reusing still-valid cells is stable, but explicit Fit rebuilds even if the
    // topology fingerprint has not changed since the preceding capture.
    ASSERT_TRUE(layout.Update(original, false));
    EXPECT_GT(layout.extent.y, originalHeight);
    EXPECT_FALSE(layout.Update(original, false));
    ASSERT_TRUE(layout.Update(original, false, true));
    EXPECT_EQ(layout.extent.y, originalHeight);
    for (size_t id = 0; id < originalPositions.size(); ++id)
    {
        EXPECT_EQ(layout.positions[id].x, originalPositions[id].x);
        EXPECT_EQ(layout.positions[id].y, originalPositions[id].y);
        EXPECT_EQ(layout.keys[id], originalKeys[id]);
    }
}

TEST(RenderGraphLayout, HidingCulledPassesAndDisablingQueuesRemoveUnusedLaneSpace)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.mode                 = rc::RDGDebugExecutionMode::Grouped;
    graph.passes[1].queue      = RHICommandContextType::eAsyncCompute;
    graph.passes[2].queue      = RHICommandContextType::eTransfer;
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, true));
    const float withCulled = layout.extent.x;
    ASSERT_TRUE(layout.Update(graph, false));
    EXPECT_FALSE(layout.lanes[3]);
    EXPECT_LT(layout.extent.x, withCulled);
    RenderGraphLayout fresh;
    ASSERT_TRUE(fresh.Update(graph, false));
    EXPECT_EQ(layout.extent.x, fresh.extent.x);

    // Keep a later lane visible to catch an empty compute band between lanes.
    graph.passes[1].queue = RHICommandContextType::eGraphics;
    ASSERT_TRUE(layout.Update(graph, false));
    fresh = {};
    ASSERT_TRUE(fresh.Update(graph, false));
    EXPECT_FALSE(layout.lanes[1]);
    EXPECT_EQ(layout.laneX[2], fresh.laneX[2]);
    EXPECT_EQ(layout.extent.x, fresh.extent.x);

    graph.mode            = rc::RDGDebugExecutionMode::Graphics;
    graph.passes[2].queue = RHICommandContextType::eGraphics;
    ASSERT_TRUE(layout.Update(graph, false));
    fresh = {};
    ASSERT_TRUE(fresh.Update(graph, false));
    EXPECT_FALSE(layout.lanes[1]);
    EXPECT_FALSE(layout.lanes[2]);
    EXPECT_EQ(layout.extent.x, fresh.extent.x);
}

TEST(RenderGraphLayout, SweepsReduceCrossingsAndRetainTheResultBetweenCaptures)
{
    rc::RDGDebugSnapshot graph;
    for (int32_t id = 0; id < 6; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "Pass " + std::to_string(id);
        pass.live  = true;
        pass.order = uint32_t(id);
        graph.passes.push_back(std::move(pass));
    }
    graph.dependencies = {{0, 5}, {1, 4}, {2, 3}};
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    uint32_t crossings = 0;
    for (size_t a = 0; a < layout.edges.size(); ++a)
    {
        for (size_t b = a + 1; b < layout.edges.size(); ++b)
        {
            const RenderGraphEdge& first  = layout.edges[a];
            const RenderGraphEdge& second = layout.edges[b];
            crossings += (layout.positions[first.source].x - layout.positions[second.source].x)
                           * (layout.positions[first.destination].x - layout.positions[second.destination].x)
                       < 0;
        }
    }
    // First-free-slot placement has all three pairwise crossings on this graph.
    EXPECT_LT(crossings, 3u);
    EXPECT_EQ(crossings, 0u);
    const HeapVector<ImVec2> positions = layout.positions;
    ++graph.frame;
    EXPECT_FALSE(layout.Update(graph, false));
    graph.passes[0].name = "Renamed producer";
    ASSERT_TRUE(layout.Update(graph, false));
    for (size_t id = 1; id < positions.size(); ++id)
    {
        EXPECT_EQ(layout.positions[id].x, positions[id].x);
        EXPECT_EQ(layout.positions[id].y, positions[id].y);
    }
}

TEST(RenderGraphLayout, LongEdgesRouteAroundInterveningCardsAndUseDistinctPorts)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.passes[3].live       = true;
    graph.passes[3].order      = 3;
    graph.dependencies         = {{0, 1}, {1, 2}, {2, 3}, {0, 3}};
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    const RenderGraphEdge& longEdge = layout.edges.back();
    ASSERT_EQ(longEdge.route.size(), 6u);
    EXPECT_NE(longEdge.route.front().x, layout.edges[0].route.front().x);
    EXPECT_NE(longEdge.route.back().x, layout.edges[2].route.back().x);
    for (size_t section = 1; section < longEdge.route.size(); ++section)
    {
        const ImVec2 a = longEdge.route[section - 1];
        const ImVec2 b = longEdge.route[section];
        EXPECT_LT(a.y, b.y);
        for (uint32_t sample = 1; sample < 20; ++sample)
        {
            const float t = float(sample) / 20;
            const float u = 1 - t;
            const float x = a.x * (u * u * u + 3 * u * u * t) + b.x * (3 * u * t * t + t * t * t);
            const float y = a.y * u * u * u + (a.y + b.y) * 0.5f * (3 * u * u * t + 3 * u * t * t) + b.y * t * t * t;
            for (int32_t id = 1; id < 3; ++id)
            {
                const ImVec2 p = layout.positions[id];
                EXPECT_FALSE(x > p.x && x < p.x + RenderGraphLayout::NodeWidth && y > p.y
                             && y < p.y + RenderGraphLayout::NodeHeight);
            }
        }
    }
}

TEST(RenderGraphLayout, HostFilterHidesOnlyNamedHostWorkAndKeepsCaptureMetadata)
{
    rc::RDGDebugSnapshot graph = MakeLayoutGraph();
    graph.passes[2].name       = "UI";
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    const std::string key = layout.keys[2];
    ASSERT_TRUE(layout.Update(graph, false, false, true));
    EXPECT_LT(layout.positions[2].x, 0);
    EXPECT_EQ(layout.keys[2], key);
    EXPECT_EQ(layout.edges.size(), 2u);
    EXPECT_TRUE(layout.edges[1].route.empty());
    EXPECT_EQ(layout.laneCounts[0], 2u);
    EXPECT_TRUE(IsRenderGraphHostPass("EditorReadPick"));
    EXPECT_TRUE(IsRenderGraphHostPass("EditorMeshPreviewClear"));
    EXPECT_FALSE(IsRenderGraphHostPass("HybridComposite"));
    EXPECT_FALSE(IsRenderGraphHostPass("BuildSceneTLAS"));
    ASSERT_TRUE(layout.Update(graph, false));
    EXPECT_GE(layout.positions[2].x, 0);
    EXPECT_FALSE(layout.edges[1].route.empty());
}

TEST(RenderGraphNavigation, AnimationFinishesAndManualNavigationCancelsFurtherMotion)
{
    RenderGraphNavigation navigation;
    navigation.Start(ImVec2(100, 200), 2);
    EXPECT_TRUE(navigation.IsActive());
    navigation.Advance(0.075f);
    EXPECT_GT(navigation.pan.x, 30);
    EXPECT_LT(navigation.pan.x, 100);
    EXPECT_GT(navigation.zoom, 1);
    navigation.Advance(0.1f);
    EXPECT_FALSE(navigation.IsActive());
    EXPECT_FLOAT_EQ(navigation.pan.x, 100);
    EXPECT_FLOAT_EQ(navigation.pan.y, 200);
    EXPECT_FLOAT_EQ(navigation.zoom, 2);
    navigation.Start(ImVec2(0, 0), 0.5f);
    navigation.Advance(0.03f);
    navigation.Stop();
    const ImVec2 stopped = navigation.pan;
    const float  zoom    = navigation.zoom;
    navigation.Advance(1);
    EXPECT_FALSE(navigation.IsActive());
    EXPECT_FLOAT_EQ(navigation.pan.x, stopped.x);
    EXPECT_FLOAT_EQ(navigation.pan.y, stopped.y);
    EXPECT_FLOAT_EQ(navigation.zoom, zoom);
}

TEST(RenderGraphLayout, WideParallelFrontiersWrapWithoutBreakingDependencies)
{
    rc::RDGDebugSnapshot graph;
    for (int32_t id = 0; id < 41; ++id)
    {
        rc::RDGDebugPass pass;
        pass.id    = id;
        pass.name  = "Shadow " + std::to_string(id);
        pass.live  = true;
        pass.order = uint32_t(id);
        graph.passes.push_back(std::move(pass));
        if (id < 40)
        {
            graph.dependencies.push_back({id, 40});
        }
    }
    RenderGraphLayout layout;
    ASSERT_TRUE(layout.Update(graph, false));
    EXPECT_LT(layout.extent.x, 2000);
    EXPECT_GT(layout.extent.y, 900);
    for (int32_t id = 0; id < 40; ++id)
    {
        EXPECT_LT(layout.positions[id].y + RenderGraphLayout::NodeHeight, layout.positions[40].y);
    }
}

TEST(RenderGraphView, ToolbarFitsAtBothScalesAndNavigationSettles)
{
    const float scales[] = {1, 2};
    const float widths[] = {720, 1280, 2400};
    for (float scale : scales)
    {
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DeltaTime   = 1.0f / 60;
        ApplyEditorTheme(scale);
        RenderGraphView view;
        view.InitFonts(scale);
        unsigned char* pixels = nullptr;
        int            width  = 0;
        int            height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(1);
        RenderGraphCaptureState state;
        state.Synchronize({false, 1, 1, "Scene.gltf"}, {}, false);
        rc::RDGDebugSnapshot* pending = state.RequestCapture();
        ASSERT_NE(pending, nullptr);
        *pending          = MakeLayoutGraph();
        pending->accepted = true;
        state.CompleteCapture(true, {}, "GI -> GI", {});
        for (float size : widths)
        {
            io.DisplaySize = ImVec2(size, 800);
            for (int frame = 0; frame < 12; ++frame)
            {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("Graph test", nullptr, ImGuiWindowFlags_NoDecoration);
                view.Draw(state);
                const ImGuiWindow* window = ImGui::GetCurrentWindow();
                EXPECT_LE(window->DC.CursorMaxPos.x, size - ImGui::GetStyle().WindowPadding.x + 1);
                ImGui::End();
                ImGui::Render();
            }
            EXPECT_FALSE(view.IsAnimating());
        }
        const std::string shortened = graph_drawing::Shorten(nullptr, "A very long resource name", 80 * scale);
        EXPECT_LE(ImGui::CalcTextSize(shortened.c_str()).x, 80 * scale);
        EXPECT_TRUE(shortened.ends_with("\xe2\x80\xa6"));
        ImGui::DestroyContext();
    }
}

TEST(RenderGraphView, ResizingTheWindowKeepsTheGraphCentred)
{
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime   = 1.0f / 60;
    ApplyEditorTheme(1);
    RenderGraphView view;
    view.InitFonts(1);
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    io.Fonts->SetTexID(1);
    RenderGraphCaptureState state;
    state.Synchronize({false, 1, 1, "Scene.gltf"}, {}, false);
    rc::RDGDebugSnapshot* pending = state.RequestCapture();
    ASSERT_NE(pending, nullptr);
    *pending          = MakeLayoutGraph();
    pending->accepted = true;
    state.CompleteCapture(true, {}, "GI -> GI", {});
    // A small window, then maximized, then back: the unnavigated graph stays centred
    // horizontally in the canvas each time, as Fit places it.
    const ImVec2 displays[] = {ImVec2(900, 600), ImVec2(2000, 1100), ImVec2(1100, 700)};
    for (const ImVec2& display : displays)
    {
        io.DisplaySize = display;
        for (int frame = 0; frame < 20; ++frame)
        {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Graph resize test", nullptr, ImGuiWindowFlags_NoDecoration);
            view.Draw(state);
            ImGui::End();
            ImGui::Render();
        }
        const RenderGraphNavigation& navigation = view.GetNavigation();
        const float                  graphWidth = view.GetLayout().extent.x * navigation.zoom * EditorScale();
        EXPECT_FALSE(navigation.IsActive());
        EXPECT_NEAR(navigation.pan.x + graphWidth * 0.5f, view.GetCanvasSize().x * 0.5f, 1.0f) << display.x;
    }
    ImGui::DestroyContext();
}
} // namespace zen::editor
