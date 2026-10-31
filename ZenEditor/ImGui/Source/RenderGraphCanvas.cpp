#include "Editor/ImGui/RenderGraphView.h"
#include "Editor/ImGui/EditorTheme.h"
#include "RenderGraphDrawing.h"
#include <spdlog/fmt/fmt.h>

namespace zen::editor
{
using namespace graph_drawing;
namespace
{
struct CanvasRect
{
    ImVec2 min;
    ImVec2 max;
    bool   Overlaps(const CanvasRect& other) const
    {
        return min.x < other.max.x && max.x > other.min.x && min.y < other.max.y && max.y > other.min.y;
    }
};

float LineDistance(ImVec2 point, ImVec2 a, ImVec2 b)
{
    const float dx     = b.x - a.x;
    const float dy     = b.y - a.y;
    const float length = dx * dx + dy * dy;
    const float t      = length > 0 ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length, 0.0f, 1.0f) : 0;
    const float x      = point.x - a.x - t * dx;
    const float y      = point.y - a.y - t * dy;
    return std::sqrt(x * x + y * y);
}

ImVec2 CurvePoint(ImVec2 a, ImVec2 b, float t)
{
    const float u      = 1 - t;
    const float middle = (a.y + b.y) * 0.5f;
    return ImVec2(a.x * (u * u * u + 3 * u * u * t) + b.x * (3 * u * t * t + t * t * t),
                  a.y * u * u * u + middle * (3 * u * u * t + 3 * u * t * t) + b.y * t * t * t);
}

void DashedLine(ImDrawList& draw, ImVec2 a, ImVec2 b, ImU32 color, float width, float dash = 5)
{
    const float dx     = b.x - a.x;
    const float dy     = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    for (float offset = 0; offset < length; offset += dash * 1.7f)
    {
        const float end = std::min(offset + dash, length);
        draw.AddLine(ImVec2(a.x + dx * offset / length, a.y + dy * offset / length),
                     ImVec2(a.x + dx * end / length, a.y + dy * end / length), color, width);
    }
}

bool DrawCurve(ImDrawList& draw, ImVec2 a, ImVec2 b, ImU32 color, float width, bool dashed, ImVec2 mouse)
{
    const float middle = (a.y + b.y) * 0.5f;
    if (!dashed)
    {
        draw.AddBezierCubic(a, ImVec2(a.x, middle), ImVec2(b.x, middle), b, color, width);
    }
    bool   hovered  = false;
    ImVec2 previous = a;
    float  distance = 0;
    for (uint32_t step = 1; step <= 12; ++step)
    {
        const ImVec2 point  = CurvePoint(a, b, float(step) / 12);
        hovered            |= LineDistance(mouse, previous, point) < 5;
        if (dashed)
        {
            const float dx     = point.x - previous.x;
            const float dy     = point.y - previous.y;
            const float length = std::sqrt(dx * dx + dy * dy);
            float       offset = 0;
            while (offset < length)
            {
                const float phase = std::fmod(distance + offset, 10.0f);
                const float end   = std::min(length, offset + (phase < 6 ? 6 : 10) - phase + 0.001f);
                if (phase < 6)
                {
                    draw.AddLine(ImVec2(previous.x + dx * offset / length, previous.y + dy * offset / length),
                                 ImVec2(previous.x + dx * end / length, previous.y + dy * end / length), color, width);
                }
                offset = end;
            }
            distance += length;
        }
        previous = point;
    }
    return hovered;
}

CanvasRect MinimapRect(ImVec2 origin, ImVec2 size)
{
    const float  scale = EditorScale();
    const ImVec2 end(origin.x + size.x - 12 * scale, origin.y + size.y - 12 * scale);
    return {ImVec2(end.x - std::min(156 * scale, size.x * 0.28f), end.y - std::min(104 * scale, size.y * 0.23f)), end};
}

void GroupFrames(ImDrawList&                 draw,
                 const rc::RDGDebugSnapshot& snapshot,
                 const RenderGraphLayout&    layout,
                 ImVec2                      origin,
                 ImVec2                      pan,
                 float                       scale,
                 ImFont*                     font)
{
    const char* prefixes[] = {"Hybrid", "Voxel", "Shadow"};
    for (const char* prefix : prefixes)
    {
        for (uint32_t lane = 0; lane < 4; ++lane)
        {
            CanvasRect bounds{ImVec2(FLT_MAX, FLT_MAX), ImVec2(-FLT_MAX, -FLT_MAX)};
            uint32_t   count = 0;
            for (const rc::RDGDebugPass& pass : snapshot.passes)
            {
                if (pass.name.starts_with(prefix) && layout.positions[pass.id].x >= 0 && layout.passLanes[pass.id] == lane)
                {
                    const ImVec2 p = ScreenPoint(origin, pan, layout.positions[pass.id], scale);
                    bounds.min.x   = std::min(bounds.min.x, p.x);
                    bounds.min.y   = std::min(bounds.min.y, p.y);
                    bounds.max.x   = std::max(bounds.max.x, p.x + RenderGraphStyle::NodeWidth * scale);
                    bounds.max.y   = std::max(bounds.max.y, p.y + RenderGraphStyle::NodeHeight * scale);
                    ++count;
                }
            }
            if (count > 1)
            {
                bounds.min.x -= 7 * scale;
                bounds.min.y -= 7 * scale;
                bounds.max.x += 7 * scale;
                bounds.max.y += 7 * scale;
                draw.AddRectFilled(bounds.min, bounds.max, Alpha(RenderGraphStyle::Queues[lane], 0.025f), 8 * scale);
                draw.AddRect(bounds.min, bounds.max, Alpha(RenderGraphStyle::Queues[lane], 0.14f), 8 * scale);
                if (scale / EditorScale() >= 0.75f)
                {
                    Text(draw, font, ImVec2(bounds.min.x + 4, bounds.min.y - TextSize(font, prefix).y - 2),
                         RenderGraphStyle::Secondary, prefix);
                }
            }
        }
    }
}
} // namespace

void RenderGraphView::UpdateFocus(int32_t id)
{
    if (m_focus != id || m_focusFingerprint != m_layout.fingerprint || m_focusTrace != m_trace)
    {
        m_focus            = id;
        m_focusFingerprint = m_layout.fingerprint;
        m_focusTrace       = m_trace;
        m_focusNodes       = HeapVector<bool>(m_layout.positions.size(), id < 0);
        m_focusEdges       = HeapVector<bool>(m_layout.edges.size(), id < 0);
        if (id >= 0)
        {
            m_focusNodes[id] = true;
            for (uint32_t direction = 0; direction < 2; ++direction)
            {
                HeapVector<bool>    visited(m_layout.positions.size(), false);
                HeapVector<int32_t> pending{id};
                visited[id] = true;
                for (size_t cursor = 0; cursor < pending.size(); ++cursor)
                {
                    const HeapVector<uint32_t>& links =
                        direction == 0 ? m_layout.incoming[pending[cursor]] : m_layout.outgoing[pending[cursor]];
                    for (uint32_t index : links)
                    {
                        const RenderGraphEdge& edge      = m_layout.edges[index];
                        const int32_t          neighbour = direction == 0 ? edge.source : edge.destination;
                        m_focusNodes[neighbour]          = true;
                        m_focusEdges[index]              = true;
                        if (m_trace && !visited[neighbour])
                        {
                            pending.push_back(neighbour);
                            visited[neighbour] = true;
                        }
                    }
                }
            }
        }
    }
}

void RenderGraphView::DrawNode(const rc::RDGDebugPass& pass, ImVec2 a, float scale, float alpha)
{
    ImDrawList&  draw = *ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + RenderGraphStyle::NodeWidth * scale, a.y + RenderGraphStyle::NodeHeight * scale);
    const ImU32  accent   = RenderGraphStyle::Queues[m_layout.passLanes[pass.id]];
    const bool   selected = pass.id == m_selected;
    const bool   host     = IsRenderGraphHostPass(pass.name);
    const float  radius   = std::clamp(8 * scale, 2.0f, 18.0f);
    const float  stroke   = std::clamp(scale, 0.7f, 2.0f);
    if (selected)
    {
        draw.AddRect(ImVec2(a.x - stroke * 2, a.y - stroke * 2), ImVec2(b.x + stroke * 2, b.y + stroke * 2),
                     Alpha(RenderGraphStyle::Brand, 0.25f), radius + 2, 0, stroke * 4);
    }
    draw.AddRectFilled(a, b, Alpha(selected ? RenderGraphStyle::SelectedBody : RenderGraphStyle::Body, alpha), radius);
    if (m_navigation.zoom < 0.4f)
    {
        draw.AddRectFilled(a, b, Alpha(accent, 0.45f * alpha), radius);
    }
    if (selected || (!host && pass.live))
    {
        draw.AddRect(a, b, Alpha(selected ? RenderGraphStyle::Brand : accent, selected ? 1 : alpha * 0.6f), radius, 0,
                     stroke * (selected ? 2 : 1));
    }
    else
    {
        const ImU32 outline = Alpha(accent, alpha * 0.6f);
        DashedLine(draw, a, ImVec2(b.x, a.y), outline, stroke, 4 * stroke);
        DashedLine(draw, ImVec2(b.x, a.y), b, outline, stroke, 4 * stroke);
        DashedLine(draw, b, ImVec2(a.x, b.y), outline, stroke, 4 * stroke);
        DashedLine(draw, ImVec2(a.x, b.y), a, outline, stroke, 4 * stroke);
    }
    const float ruleBottom = a.y + 9 * scale;
    if (pass.live)
    {
        draw.AddRectFilled(ImVec2(a.x + 8 * scale, a.y + 6 * scale), ImVec2(b.x - 8 * scale, ruleBottom), Alpha(accent, alpha),
                           1.5f * scale);
    }
    // Text never scales with zoom, so the card's on-screen height decides what fits:
    // title and detail line, title alone, or (below 0.4) a plain coloured block.
    const float titleHeight  = TextSize(m_titleFont, "Ag").y;
    const float detailHeight = TextSize(m_detailFont, "Ag").y;
    const float titleTop     = ruleBottom + std::max(2.0f, 3 * scale);
    const float detailTop    = b.y - std::max(3.0f, 7 * scale) - detailHeight;
    const bool  details      = detailTop >= titleTop + titleHeight + 1;
    if (m_navigation.zoom >= 0.4f && b.y - ruleBottom >= titleHeight + 2)
    {
        const float padding       = std::max(4.0f, 10 * scale);
        const float badgeRadius   = std::max(8.0f, TextSize(m_detailFont, "99").x * 0.62f);
        const bool  badge         = details && pass.live;
        float       titleWidth    = b.x - a.x - padding * 2 - (badge ? badgeRadius * 2 + 6 : 0);
        const char* chip          = !pass.live                                                     ? "culled"
                                  : host                                                           ? "host"
                                  : pass.preference == rc::RDGQueuePreference::ePreferAsyncCompute ? "async"
                                                                                                   : nullptr;
        const bool  showChip      = details && chip != nullptr && titleWidth > ChipWidth(m_detailFont, chip) * 2.5f;
        titleWidth               -= showChip ? ChipWidth(m_detailFont, chip) + 6 : 0;
        const std::string title   = Shorten(m_titleFont, pass.name, titleWidth);
        const float       titleY  = details ? titleTop : std::round((ruleBottom + b.y - titleHeight) * 0.5f);
        Text(draw, m_titleFont, ImVec2(a.x + padding, titleY), Alpha(RenderGraphStyle::Text, alpha), title.c_str());
        if (showChip)
        {
            const float chipX = a.x + padding + TextSize(m_titleFont, title.c_str()).x + 6;
            Chip(draw, m_detailFont, ImVec2(chipX, titleY + (titleHeight - detailHeight - 4) * 0.5f), chip,
                 host || !pass.live ? RenderGraphStyle::Secondary : accent, host || !pass.live ? 0 : 0.2f, alpha);
        }
        if (details)
        {
            const std::string detail    = fmt::format("#{} \xc2\xb7 {} in \xc2\xb7 {} out", pass.id,
                                                      m_layout.incoming[pass.id].size(), m_layout.outgoing[pass.id].size());
            const std::string shortened = Shorten(m_detailFont, detail, b.x - a.x - padding * 2);
            Text(draw, m_detailFont, ImVec2(a.x + padding, detailTop), Alpha(RenderGraphStyle::Secondary, alpha),
                 shortened.c_str());
        }
        if (badge)
        {
            const std::string order = std::to_string(pass.order);
            const ImVec2      text  = TextSize(m_detailFont, order.c_str());
            const ImVec2      center(std::round(b.x - padding - badgeRadius), std::round(titleY + titleHeight * 0.5f));
            draw.AddCircleFilled(center, badgeRadius, Alpha(RenderGraphStyle::Pill, alpha));
            Text(draw, m_detailFont, ImVec2(center.x - text.x * 0.5f, center.y - text.y * 0.5f),
                 Alpha(selected ? RenderGraphStyle::Text : RenderGraphStyle::Secondary, alpha), order.c_str());
        }
    }
}

void RenderGraphView::DrawLegend(ImVec2 origin, ImVec2 maximum, float right)
{
    // The mockup's legend: queue swatches, the dashed card and the dashed wait edge,
    // then the zoom level. Trailing items drop out when the minimap leaves no room.
    ImDrawList&       draw     = *ImGui::GetWindowDrawList();
    const float       ui       = EditorScale();
    const float       swatch   = std::round(10 * ui);
    const float       gap      = std::round(14 * ui);
    const float       spacing  = std::round(6 * ui);
    const std::string zoom     = fmt::format("{}%", int(std::round(m_navigation.zoom * 100)));
    const char*       labels[] = {"Graphics", "Async compute", "Transfer", "Host or culled", "Cross-queue wait"};
    float             widths[5]{};
    for (uint32_t item = 0; item < 5; ++item)
    {
        widths[item] = (item == 4 ? swatch * 2.4f : swatch) + spacing + TextSize(m_detailFont, labels[item]).x;
    }
    const float zoomWidth = TextSize(m_detailFont, zoom.c_str()).x;
    const float padding   = std::round(8 * ui);
    float       total     = zoomWidth + padding * 2;
    uint32_t    count     = 0;
    while (count < 5 && origin.x + padding + total + widths[count] + gap < right)
    {
        total += widths[count] + gap;
        ++count;
    }
    const float height = TextSize(m_detailFont, "Ag").y + padding;
    const float top    = maximum.y - height - std::round(10 * ui);
    if (origin.x + padding + total < right)
    {
        draw.AddRectFilled(ImVec2(origin.x + padding, top), ImVec2(origin.x + padding + total, top + height),
                           Alpha(RenderGraphStyle::Pill, 0.92f), 6 * ui);
        float       x      = origin.x + padding * 2;
        const float middle = std::round(top + height * 0.5f);
        for (uint32_t item = 0; item < count; ++item)
        {
            if (item < 3)
            {
                draw.AddRectFilled(ImVec2(x, middle - swatch * 0.5f), ImVec2(x + swatch, middle + swatch * 0.5f),
                                   RenderGraphStyle::Queues[item], 3 * ui);
            }
            else if (item == 3)
            {
                const ImVec2 a(x, middle - swatch * 0.5f);
                const ImVec2 b(x + swatch, middle + swatch * 0.5f);
                DashedLine(draw, a, ImVec2(b.x, a.y), RenderGraphStyle::Secondary, 1, 2.5f);
                DashedLine(draw, ImVec2(b.x, a.y), b, RenderGraphStyle::Secondary, 1, 2.5f);
                DashedLine(draw, b, ImVec2(a.x, b.y), RenderGraphStyle::Secondary, 1, 2.5f);
                DashedLine(draw, ImVec2(a.x, b.y), a, RenderGraphStyle::Secondary, 1, 2.5f);
            }
            else
            {
                DashedLine(draw, ImVec2(x, middle), ImVec2(x + swatch * 2.4f, middle), RenderGraphStyle::Secondary, 2, 4);
            }
            const float label = x + (item == 4 ? swatch * 2.4f : swatch) + spacing;
            Text(draw, m_detailFont, ImVec2(label, middle - TextSize(m_detailFont, labels[item]).y * 0.5f),
                 RenderGraphStyle::Secondary, labels[item]);
            x += widths[item] + gap;
        }
        Text(draw, m_detailFont, ImVec2(x, middle - TextSize(m_detailFont, zoom.c_str()).y * 0.5f), RenderGraphStyle::Text,
             zoom.c_str());
    }
}

void RenderGraphView::DrawMinimap(ImVec2 origin, ImVec2 size, bool hovered)
{
    ImDrawList&      draw   = *ImGui::GetWindowDrawList();
    const CanvasRect bounds = MinimapRect(origin, size);
    const float      scale  = std::min((bounds.max.x - bounds.min.x - 16) / std::max(m_layout.extent.x, 1.0f),
                                       (bounds.max.y - bounds.min.y - 16) / std::max(m_layout.extent.y, 1.0f));
    const ImVec2     start((bounds.min.x + bounds.max.x - m_layout.extent.x * scale) * 0.5f,
                           (bounds.min.y + bounds.max.y - m_layout.extent.y * scale) * 0.5f);
    draw.AddRectFilled(bounds.min, bounds.max, Alpha(RenderGraphStyle::Pill, 0.96f), 6);
    draw.AddRect(bounds.min, bounds.max, Alpha(RenderGraphStyle::Secondary, 0.35f), 6);
    draw.PushClipRect(bounds.min, bounds.max, true);
    for (uint32_t id = 0; id < m_layout.positions.size(); ++id)
    {
        if (m_layout.positions[id].x >= 0)
        {
            const ImVec2 a = ScreenPoint(start, ImVec2(0, 0), m_layout.positions[id], scale);
            draw.AddRectFilled(
                a,
                ImVec2(a.x + RenderGraphStyle::NodeWidth * scale, a.y + std::max(2.0f, RenderGraphStyle::NodeHeight * scale)),
                id == uint32_t(m_selected) ? RenderGraphStyle::Brand : RenderGraphStyle::Queues[m_layout.passLanes[id]]);
        }
    }
    const float  ratio = scale / (m_navigation.zoom * EditorScale());
    const ImVec2 view(start.x - m_navigation.pan.x * ratio, start.y - m_navigation.pan.y * ratio);
    draw.AddRectFilled(view, ImVec2(view.x + size.x * ratio, view.y + size.y * ratio), Alpha(RenderGraphStyle::Brand, 0.08f));
    draw.AddRect(view, ImVec2(view.x + size.x * ratio, view.y + size.y * ratio), RenderGraphStyle::Brand, 0, 0, 1.5f);
    draw.PopClipRect();
    if (hovered && ImGui::IsMouseHoveringRect(bounds.min, bounds.max) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        m_navigation.Stop();
        m_autoFit        = false;
        m_navigation.pan = ImVec2(size.x * 0.5f - (mouse.x - start.x) / ratio, size.y * 0.5f - (mouse.y - start.y) / ratio);
    }
}

void RenderGraphView::DrawCanvas(const rc::RDGDebugSnapshot& snapshot)
{
    const ImVec2 origin    = ImGui::GetCursorScreenPos();
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(available.x, 1.0f), std::max(available.y, 1.0f));
    const ImVec2 maximum(origin.x + size.x, origin.y + size.y);
    ImGui::InvisibleButton("CanvasInput", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle
                               | ImGuiButtonFlags_MouseButtonRight);
    const bool       hovered   = ImGui::IsItemHovered();
    const ImGuiIO&   io        = ImGui::GetIO();
    const float      uiScale   = EditorScale();
    const CanvasRect minimap   = MinimapRect(origin, size);
    const bool       onMinimap = hovered && ImGui::IsMouseHoveringRect(minimap.min, minimap.max);
    if (ImGui::IsItemActive()
        && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)))
    {
        m_navigation.Stop();
        m_autoFit           = false;
        m_navigation.pan.x += io.MouseDelta.x;
        m_navigation.pan.y += io.MouseDelta.y;
    }
    if (hovered && io.MouseWheel != 0 && !onMinimap)
    {
        m_navigation.Stop();
        m_autoFit          = false;
        const float zoom   = std::clamp(m_navigation.zoom * std::pow(1.15f, io.MouseWheel), 0.02f, 2.5f);
        const float ratio  = zoom / m_navigation.zoom;
        m_navigation.pan.x = io.MousePos.x - origin.x - (io.MousePos.x - origin.x - m_navigation.pan.x) * ratio;
        m_navigation.pan.y = io.MousePos.y - origin.y - (io.MousePos.y - origin.y - m_navigation.pan.y) * ratio;
        m_navigation.zoom  = zoom;
    }
    // Pan is measured from the canvas's top-left corner, so a resize (maximizing the window,
    // or the inspector changing width) would leave the graph where it was. An unnavigated
    // view is fitted again at once; a navigated one keeps the same graph point at its centre.
    if (m_canvasSize.x > 0 && (size.x != m_canvasSize.x || size.y != m_canvasSize.y))
    {
        if (m_autoFit)
        {
            m_fit     = true;
            m_snapFit = true;
        }
        else
        {
            m_navigation.Offset(ImVec2((size.x - m_canvasSize.x) * 0.5f, (size.y - m_canvasSize.y) * 0.5f));
        }
    }
    m_canvasSize         = size;
    float       fitZoom  = std::clamp(std::min((size.x - 48 * uiScale) / (m_layout.extent.x * uiScale),
                                               (size.y - 70 * uiScale) / (m_layout.extent.y * uiScale)),
                                      0.02f, 1.0f);
    const float viewZoom = m_navigation.IsActive() ? m_navigation.targetZoom : m_navigation.zoom;
    // Shrinking captures keep the camera steady, so temporary work cannot make the zoom
    // pulse. Once a much smaller graph has stayed unchanged for two more captures (for
    // example after the first scene frame's environment bake), it is fitted again.
    if (m_autoFit && !m_fit && !m_layoutChanged && m_stableCaptures >= 2 && fitZoom > viewZoom * 1.5f)
    {
        m_fit = true;
    }
    // Update already rebuilds major replacements and retains small changes. Do not lay
    // out a second time here. Expand the view at once when new work outgrows it.
    if (m_autoFit && m_layoutChanged && !m_fit)
    {
        const ImVec2 pan = m_navigation.IsActive() ? m_navigation.targetPan : m_navigation.pan;
        m_fit            = pan.x + m_layout.extent.x * viewZoom * uiScale > size.x - 12 * uiScale
                        || pan.y + m_layout.extent.y * viewZoom * uiScale > size.y - 24 * uiScale;
        fitZoom          = std::min(fitZoom, viewZoom);
    }
    m_layoutChanged = false;
    if (m_fit)
    {
        const ImVec2 pan((size.x - m_layout.extent.x * fitZoom * uiScale) * 0.5f, 36 * uiScale);
        if (m_snapFit)
        {
            m_navigation.Jump(pan, fitZoom);
        }
        else
        {
            m_navigation.Start(pan, fitZoom);
        }
        m_fit     = false;
        m_snapFit = false;
    }
    if (m_focusSelection && m_selected >= 0 && m_layout.positions[m_selected].x >= 0)
    {
        m_autoFit         = false;
        const ImVec2 p    = m_layout.positions[m_selected];
        const float  zoom = std::max(m_navigation.zoom, 1.0f);
        m_navigation.Start(ImVec2(size.x * 0.5f - (p.x + RenderGraphStyle::NodeWidth * 0.5f) * zoom * uiScale,
                                  size.y * 0.5f - (p.y + RenderGraphStyle::NodeHeight * 0.5f) * zoom * uiScale),
                           zoom);
        m_focusSelection = false;
    }
    m_navigation.Advance(io.DeltaTime);
    const float scale = m_navigation.zoom * uiScale;
    ImDrawList& draw  = *ImGui::GetWindowDrawList();
    draw.PushClipRect(origin, maximum, true);
    draw.AddRectFilled(origin, maximum, RenderGraphStyle::Canvas);
    const float gridAlpha = std::min((m_navigation.zoom - 0.4f) * 0.3f, 0.16f);
    if (gridAlpha >= 0.03f)
    {
        float step = 24 * scale;
        // Decimate the graph-anchored grid to keep at least 24 logical screen pixels
        // between dots, and omit the nearly transparent part of its fade entirely.
        while (step < 24 * uiScale)
        {
            step *= 2;
        }
        const ImU32 color = Alpha(RenderGraphStyle::Secondary, gridAlpha);
        for (float x = std::fmod(m_navigation.pan.x, step); x < size.x; x += step)
        {
            for (float y = std::fmod(m_navigation.pan.y, step); y < size.y; y += step)
            {
                draw.AddCircleFilled(ImVec2(origin.x + x, origin.y + y), std::clamp(scale, 0.6f, 1.5f), color, 4);
            }
        }
    }
    for (uint32_t lane = 0; lane < 4; ++lane)
    {
        if (m_layout.lanes[lane])
        {
            const float left  = origin.x + m_navigation.pan.x + (m_layout.laneX[lane] - 12) * scale;
            const float right = left + (m_layout.laneWidth[lane] + 24) * scale;
            draw.AddRectFilled(ImVec2(left, origin.y), ImVec2(right, maximum.y), Alpha(RenderGraphStyle::Queues[lane], 0.035f),
                               8 * scale);
            draw.AddRect(ImVec2(left, origin.y), ImVec2(right, maximum.y), Alpha(RenderGraphStyle::Queues[lane], 0.18f),
                         8 * scale);
        }
    }
    GroupFrames(draw, snapshot, m_layout, origin, m_navigation.pan, scale, m_detailFont);
    HeapVector<CanvasRect> occupied;
    occupied.push_back(minimap);
    occupied.push_back({ImVec2(origin.x, maximum.y - 30 * uiScale), ImVec2(minimap.min.x, maximum.y)});
    occupied.push_back({origin, ImVec2(maximum.x, origin.y + 30 * uiScale)});
    int32_t hoveredPass = -1;
    for (const rc::RDGDebugPass& pass : snapshot.passes)
    {
        if (m_layout.positions[pass.id].x >= 0)
        {
            const ImVec2 a = ScreenPoint(origin, m_navigation.pan, m_layout.positions[pass.id], scale);
            const ImVec2 b(a.x + RenderGraphStyle::NodeWidth * scale, a.y + RenderGraphStyle::NodeHeight * scale);
            occupied.push_back({ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4)});
            if (hovered && !onMinimap && io.MousePos.y > origin.y + 30 * uiScale && ImGui::IsMouseHoveringRect(a, b))
            {
                hoveredPass = pass.id;
            }
        }
    }
    if (hovered && !onMinimap && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        SelectPass(hoveredPass);
    }
    UpdateFocus(hoveredPass >= 0 ? hoveredPass : m_selected);
    // Unrelated edges first and faint; the focused subgraph is drawn over them at full
    // strength, and its curves are reserved so labels never sit on a highlighted line.
    const RenderGraphEdge* hoveredEdge = nullptr;
    const bool             focused     = m_focus >= 0;
    const float            lineScale   = std::clamp(scale, 0.65f, 1.5f);
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        for (uint32_t index = 0; index < m_layout.edges.size(); ++index)
        {
            const RenderGraphEdge& edge        = m_layout.edges[index];
            const bool             highlighted = focused && m_focusEdges[index];
            if (!edge.route.empty() && highlighted == (layer == 1))
            {
                const uint32_t lane       = m_layout.passLanes[edge.destination];
                const bool     crossQueue = m_layout.passLanes[edge.source] != lane;
                const float    strength   = highlighted ? 1.0f
                                          : focused     ? RenderGraphStyle::EdgeDimAlpha
                                                        : RenderGraphStyle::EdgeIdleAlpha;
                const ImU32    color =
                    Alpha(edge.reasons.empty() && !crossQueue ? RenderGraphStyle::Secondary : RenderGraphStyle::Queues[lane],
                          strength);
                const float width = (highlighted ? 2.2f : focused ? 1.0f : 1.4f) * lineScale;
                for (size_t point = 1; point < edge.route.size(); ++point)
                {
                    const ImVec2 a = ScreenPoint(origin, m_navigation.pan, edge.route[point - 1], scale);
                    const ImVec2 b = ScreenPoint(origin, m_navigation.pan, edge.route[point], scale);
                    if (DrawCurve(draw, a, b, color, width, crossQueue, io.MousePos) && hovered && !onMinimap
                        && (hoveredEdge == nullptr || highlighted))
                    {
                        hoveredEdge = &edge;
                    }
                    for (uint32_t sample = 0; highlighted && sample <= 8; ++sample)
                    {
                        const ImVec2 p = CurvePoint(a, b, float(sample) / 8);
                        occupied.push_back({ImVec2(p.x - 3, p.y - 3), ImVec2(p.x + 3, p.y + 3)});
                    }
                }
                const ImVec2 end   = ScreenPoint(origin, m_navigation.pan, edge.route.back(), scale);
                const float  arrow = std::clamp((highlighted ? 5.5f : 4.0f) * scale, 2.5f, 9.0f);
                // Vertical endpoint controls give the final tangent its downward direction.
                draw.AddTriangleFilled(end, ImVec2(end.x - arrow * 0.6f, end.y - arrow),
                                       ImVec2(end.x + arrow * 0.6f, end.y - arrow), color);
            }
        }
    }
    for (const rc::RDGDebugPass& pass : snapshot.passes)
    {
        if (m_layout.positions[pass.id].x >= 0)
        {
            const ImVec2     a = ScreenPoint(origin, m_navigation.pan, m_layout.positions[pass.id], scale);
            const CanvasRect bounds{
                a, ImVec2(a.x + RenderGraphStyle::NodeWidth * scale, a.y + RenderGraphStyle::NodeHeight * scale)};
            if (bounds.Overlaps({origin, maximum}))
            {
                const bool  emphasized = m_focus >= 0 ? m_focusNodes[pass.id] : m_search.PassFilter(pass.name.c_str());
                const float alpha      = emphasized ? 1 : RenderGraphStyle::DimAlpha;
                DrawNode(pass, a, scale, alpha);
            }
        }
    }
    // Only direct focus connections get labels. Reserve every visible card and
    // previously placed pill; suppress a label if no collision-free slot exists.
    if (m_focus >= 0 && m_navigation.zoom >= 0.4f)
    {
        for (const RenderGraphEdge& edge : m_layout.edges)
        {
            if (!edge.route.empty() && (edge.source == m_focus || edge.destination == m_focus))
            {
                const uint32_t              lane       = m_layout.passLanes[edge.destination];
                const bool                  crossQueue = m_layout.passLanes[edge.source] != lane;
                const rc::RDGDebugResource* resource =
                    edge.reasons.empty() ? nullptr : FindResource(snapshot, snapshot.dependencies[edge.reasons[0]].resourceId);
                std::string       label = crossQueue          ? QueueConnectionName(snapshot, edge)
                                        : resource != nullptr ? resource->name
                                                              : "Group order";
                const std::string suffix =
                    edge.reasons.size() > 1 && !crossQueue ? fmt::format(" +{}", edge.reasons.size() - 1) : "";
                label             = Shorten(m_detailFont, label,
                                            std::min(180 * uiScale, size.x * 0.4f) - TextSize(m_detailFont, suffix.c_str()).x)
                                  + suffix;
                const ImVec2 text = TextSize(m_detailFont, label.c_str());
                const ImVec2 pillSize(text.x + text.y * 0.8f, text.y + 4);
                bool         placed = false;
                for (size_t point = 1; point < edge.route.size() && !placed; ++point)
                {
                    const ImVec2 a = ScreenPoint(origin, m_navigation.pan, edge.route[point - 1], scale);
                    const ImVec2 b = ScreenPoint(origin, m_navigation.pan, edge.route[point], scale);
                    // Beside the curve first (steep curves), then above or below it (flat curves).
                    for (uint32_t attempt = 0; attempt < 12 && !placed; ++attempt)
                    {
                        const ImVec2     p    = CurvePoint(a, b, 0.25f + 0.25f * float(attempt / 4));
                        const uint32_t   side = attempt % 4;
                        const float      gap  = 8 * uiScale;
                        const ImVec2     start(side == 0   ? p.x + gap
                                               : side == 1 ? p.x - pillSize.x - gap
                                                           : p.x - pillSize.x * 0.5f,
                                               side == 2   ? p.y - pillSize.y - gap
                                               : side == 3 ? p.y + gap
                                                           : p.y - pillSize.y * 0.5f);
                        const CanvasRect bounds{start, ImVec2(start.x + pillSize.x, start.y + pillSize.y)};
                        bool collision = bounds.min.x < origin.x || bounds.max.x > maximum.x || bounds.min.y < origin.y
                                      || bounds.max.y > maximum.y;
                        for (const CanvasRect& reserved : occupied)
                        {
                            collision |= bounds.Overlaps(reserved);
                        }
                        if (!collision)
                        {
                            Pill(draw, m_detailFont, start, label.c_str(), RenderGraphStyle::Queues[lane]);
                            occupied.push_back(
                                {ImVec2(bounds.min.x - 3, bounds.min.y - 3), ImVec2(bounds.max.x + 3, bounds.max.y + 3)});
                            placed = true;
                        }
                    }
                }
            }
        }
    }
    // Lane headers stay at the canvas top while the graph pans underneath.
    for (uint32_t lane = 0; lane < 4; ++lane)
    {
        if (m_layout.lanes[lane])
        {
            const float left  = std::max(origin.x, origin.x + m_navigation.pan.x + (m_layout.laneX[lane] - 12) * scale);
            const float right = std::min(maximum.x, origin.x + m_navigation.pan.x
                                                        + (m_layout.laneX[lane] + m_layout.laneWidth[lane] + 12) * scale);
            if (right > left)
            {
                // Lane name in the queue colour, then the pass count in muted text.
                draw.AddRectFilled(ImVec2(left, origin.y), ImVec2(right, origin.y + 28 * uiScale),
                                   Mix(RenderGraphStyle::Canvas, RenderGraphStyle::Queues[lane], 0.05f), 5);
                const std::string name  = Shorten(m_titleFont, LaneName(lane), right - left - 12 * uiScale);
                const float       textY = std::round(origin.y + (28 * uiScale - TextSize(m_titleFont, "Ag").y) * 0.5f);
                Text(draw, m_titleFont, ImVec2(left + 8 * uiScale, textY),
                     Mix(RenderGraphStyle::Text, RenderGraphStyle::Queues[lane], 0.6f), name.c_str());
                const std::string count  = fmt::format("\xc2\xb7  {} passes", m_layout.laneCounts[lane]);
                const float       countX = left + 8 * uiScale + TextSize(m_titleFont, name.c_str()).x + 8 * uiScale;
                if (countX + TextSize(m_detailFont, count.c_str()).x < right - 6 * uiScale)
                {
                    Text(draw, m_detailFont,
                         ImVec2(countX, textY + TextSize(m_titleFont, "Ag").y - TextSize(m_detailFont, "Ag").y - 1),
                         RenderGraphStyle::Secondary, count.c_str());
                }
            }
        }
    }
    DrawLegend(origin, maximum, minimap.min.x - 8 * uiScale);
    DrawMinimap(origin, size, hovered);
    draw.PopClipRect();
    if (hoveredPass >= 0)
    {
        ImGui::SetTooltip("%s (#%d)\n%s | %s", snapshot.passes[hoveredPass].name.c_str(), hoveredPass,
                          LaneName(m_layout.passLanes[hoveredPass]), snapshot.passes[hoveredPass].live ? "Live" : "Culled");
    }
    else if (hoveredEdge != nullptr)
    {
        ImGui::BeginTooltip();
        DrawEdgeDetails(snapshot, *hoveredEdge);
        ImGui::EndTooltip();
    }
}
} // namespace zen::editor
