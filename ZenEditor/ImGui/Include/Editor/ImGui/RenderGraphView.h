#pragma once
#include "Editor/Model/RenderGraphCaptureState.h"
#include "imgui.h"

namespace zen::editor
{
struct RenderGraphStyle
{
    static constexpr float NodeWidth    = 170;
    static constexpr float NodeHeight   = 56;
    static constexpr float ColumnPitch  = 200;
    static constexpr float LayerPitch   = 95;
    static constexpr float LaneGap      = 44;
    static constexpr float HeaderHeight = 42;
    static constexpr float DimAlpha     = 0.32f;
    // Unrelated edges recede further than cards so the focused subgraph reads first.
    static constexpr float EdgeDimAlpha      = 0.12f;
    static constexpr float EdgeIdleAlpha     = 0.5f;
    static constexpr float NavigationSeconds = 0.15f;
    static constexpr ImU32 Queues[4] = {IM_COL32(143, 140, 242, 255), IM_COL32(78, 201, 168, 255), IM_COL32(217, 164, 106, 255),
                                        IM_COL32(107, 119, 138, 255)};
    static constexpr ImU32 Canvas    = IM_COL32(34, 39, 50, 255);
    static constexpr ImU32 Body      = IM_COL32(43, 50, 65, 255);
    static constexpr ImU32 SelectedBody = IM_COL32(47, 56, 80, 255);
    static constexpr ImU32 Brand        = IM_COL32(139, 184, 255, 255);
    static constexpr ImU32 Secondary    = IM_COL32(155, 168, 187, 255);
    static constexpr ImU32 Text         = IM_COL32(232, 237, 245, 255);
    static constexpr ImU32 Pill         = IM_COL32(27, 31, 40, 255);
    static constexpr ImU32 ChipBody     = IM_COL32(44, 53, 69, 255);
    static constexpr ImU32 Row          = IM_COL32(43, 52, 68, 255);
    static constexpr ImU32 RowHovered   = IM_COL32(60, 83, 111, 255);
};

bool IsRenderGraphHostPass(const std::string& name);

struct RenderGraphEdge
{
    int32_t              source{-1};
    int32_t              destination{-1};
    bool                 ordering{false};
    HeapVector<uint32_t> reasons;
    // Bottom/top ports and two points around each skipped layer's node band.
    HeapVector<ImVec2> route;
};

struct RenderGraphLayout
{
    static constexpr float NodeWidth  = RenderGraphStyle::NodeWidth;
    static constexpr float NodeHeight = RenderGraphStyle::NodeHeight;

    uint64_t           fingerprint{0};
    HeapVector<ImVec2> positions;
    // Name/type/occurrence keys remap build-local IDs across conditional passes.
    HeapVector<std::string>          keys;
    HeapVector<uint32_t>             passLanes;
    HeapVector<RenderGraphEdge>      edges;
    HeapVector<HeapVector<uint32_t>> incoming;
    HeapVector<HeapVector<uint32_t>> outgoing;
    ImVec2                           extent;
    float                            laneX[4]{};
    float                            laneWidth[4]{};
    uint32_t                         laneCounts[4]{};
    bool                             lanes[4]{};

    // Rebuild discards retained positions, for an explicit compact Fit.
    bool Update(const rc::RDGDebugSnapshot& snapshot, bool showCulled, bool rebuild = false, bool hideHost = false);
};

struct RenderGraphNavigation
{
    ImVec2 pan{30, 40};
    float  zoom{1};
    ImVec2 startPan;
    ImVec2 targetPan;
    float  startZoom{1};
    float  targetZoom{1};
    float  elapsed{RenderGraphStyle::NavigationSeconds};
    void   Start(ImVec2 destination, float scale);
    void   Advance(float seconds);
    void   Stop();
    // Moves the view and any running animation together, keeping its destination.
    void Offset(ImVec2 delta);
    // Places the view immediately, for example while a window is being resized.
    void Jump(ImVec2 destination, float scale);
    bool IsActive() const;
};

class RenderGraphView
{
public:
    // Called before the window uploads its static atlas.
    void InitFonts(float scale);
    void Draw(RenderGraphCaptureState& state);
    bool IsAnimating() const;
    // Applied when a capture first contains a pass with this name.
    void RequestSelection(const std::string& name);

    const RenderGraphNavigation& GetNavigation() const
    {
        return m_navigation;
    }
    const RenderGraphLayout& GetLayout() const
    {
        return m_layout;
    }
    ImVec2 GetCanvasSize() const
    {
        return m_canvasSize;
    }

private:
    void              SelectPass(int32_t id);
    void              DrawCanvas(const rc::RDGDebugSnapshot& snapshot);
    void              DrawInspector(const rc::RDGDebugSnapshot& snapshot);
    void              DrawEdgeDetails(const rc::RDGDebugSnapshot& snapshot, const RenderGraphEdge& edge);
    void              DrawToolbar(RenderGraphCaptureState& state);
    void              UpdateFocus(int32_t id);
    void              DrawMinimap(ImVec2 origin, ImVec2 size, bool hovered);
    void              DrawNode(const rc::RDGDebugPass& pass, ImVec2 minimum, float scale, float alpha);
    void              DrawLegend(ImVec2 origin, ImVec2 maximum, float right);
    bool              DependencyRow(const std::string& name, const char* reason, ImU32 reasonColor, bool input, bool enabled);
    RenderGraphLayout m_layout;
    ImGuiTextFilter   m_search;
    RenderGraphNavigation m_navigation;
    ImFont*               m_titleFont{nullptr};
    ImFont*               m_detailFont{nullptr};
    HeapVector<bool>      m_focusNodes;
    HeapVector<bool>      m_focusEdges;
    int32_t               m_focus{-2};
    uint64_t              m_focusFingerprint{0};
    bool                  m_focusTrace{false};
    int32_t               m_selected{-1};
    std::string           m_selectedKey;
    std::string           m_requestedSelection;
    uint64_t              m_scene{UINT64_MAX};
    bool                  m_play{false};
    bool                  m_showCulled{false};
    bool                  m_hideHost{false};
    bool                  m_trace{false};
    bool                  m_fit{true};
    // Until the user pans or zooms, expand the view when new work no longer fits.
    bool m_autoFit{true};
    bool m_layoutChanged{false};
    // Captures since the last topology change, counted by snapshot frame. A graph that
    // stays much smaller than the view this long is fitted again (see DrawCanvas).
    uint32_t m_stableCaptures{0};
    uint64_t m_stableFrame{UINT64_MAX};
    bool     m_focusSelection{false};
    // A Fit caused by a canvas resize is applied at once, so it follows the window edge.
    bool   m_snapFit{false};
    ImVec2 m_canvasSize;
};
} // namespace zen::editor
