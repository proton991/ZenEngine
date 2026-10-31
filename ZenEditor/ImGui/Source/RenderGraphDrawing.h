#pragma once
#include "Editor/ImGui/RenderGraphView.h"
#include <algorithm>
#include <cmath>

namespace zen::editor::graph_drawing
{
inline ImU32 Alpha(ImU32 color, float alpha)
{
    return (color & ~IM_COL32_A_MASK) | (ImU32(float((color >> IM_COL32_A_SHIFT) & 255) * alpha) << IM_COL32_A_SHIFT);
}

inline ImVec2 ScreenPoint(ImVec2 origin, ImVec2 pan, ImVec2 position, float scale)
{
    return ImVec2(origin.x + pan.x + position.x * scale, origin.y + pan.y + position.y * scale);
}

inline ImVec2 TextSize(ImFont* font, const char* text)
{
    return font != nullptr ? font->CalcTextSizeA(font->LegacySize, FLT_MAX, 0, text) : ImGui::CalcTextSize(text);
}

// Trim on UTF-8 boundaries. Text is always drawn at the font's baked size.
inline std::string Shorten(ImFont* font, std::string text, float width)
{
    const char* ellipsis = "\xe2\x80\xa6";
    if (TextSize(font, text.c_str()).x > width)
    {
        const float suffix = TextSize(font, ellipsis).x;
        while (!text.empty() && TextSize(font, text.c_str()).x + suffix > width)
        {
            size_t last = text.size() - 1;
            while (last > 0 && (static_cast<unsigned char>(text[last]) & 0xc0) == 0x80)
            {
                --last;
            }
            text.resize(last);
        }
        text = suffix <= width ? text + ellipsis : std::string{};
    }
    return text;
}

inline void Text(ImDrawList& draw, ImFont* font, ImVec2 position, ImU32 color, const char* text)
{
    position.x = std::round(position.x);
    position.y = std::round(position.y);
    draw.AddText(font, font != nullptr ? font->LegacySize : ImGui::GetFontSize(), position, color, text);
}

inline void Pill(ImDrawList& draw, ImFont* font, ImVec2 position, const char* text, ImU32 color, float alpha = 1)
{
    const ImVec2 size    = TextSize(font, text);
    const float  padding = size.y * 0.4f;
    const ImVec2 end(position.x + size.x + padding * 2, position.y + size.y + 4);
    draw.AddRectFilled(position, end, Alpha(RenderGraphStyle::Pill, alpha), size.y * 0.5f);
    draw.AddRect(position, end, Alpha(color, alpha * 0.6f), size.y * 0.5f);
    Text(draw, font, ImVec2(position.x + padding, position.y + 2), Alpha(color, alpha), text);
}

inline ImU32 Mix(ImU32 base, ImU32 color, float amount)
{
    ImU32 result = IM_COL32_A_MASK;
    for (uint32_t shift = 0; shift < 24; shift += 8)
    {
        const float a  = float((base >> shift) & 255);
        const float b  = float((color >> shift) & 255);
        result        |= ImU32(a + (b - a) * amount) << shift;
    }
    return result;
}

// Filled chip, as in the mockup: a dark tint of its colour behind the text.
// Neutral chips pass a zero tint and keep the chip body colour.
inline ImVec2 Chip(ImDrawList& draw, ImFont* font, ImVec2 position, const char* text, ImU32 color, float tint, float alpha = 1)
{
    const ImVec2 size    = TextSize(font, text);
    const float  padding = std::round(size.y * 0.55f);
    const ImVec2 end(position.x + size.x + padding * 2, position.y + size.y + 4);
    const ImU32  fill = tint > 0 ? Mix(RenderGraphStyle::Pill, color, tint) : RenderGraphStyle::ChipBody;
    draw.AddRectFilled(position, end, Alpha(fill, alpha), (end.y - position.y) * 0.5f);
    Text(draw, font, ImVec2(position.x + padding, position.y + 2), Alpha(color, alpha), text);
    return ImVec2(end.x - position.x, end.y - position.y);
}

inline float ChipWidth(ImFont* font, const char* text)
{
    const ImVec2 size = TextSize(font, text);
    return size.x + std::round(size.y * 0.55f) * 2;
}

inline const char* LaneName(uint32_t lane)
{
    const char* names[] = {"Graphics", "Async compute", "Transfer", "Culled"};
    return names[std::min(lane, 3u)];
}

inline const char* ReasonName(rc::RDGDependencyReason reason)
{
    const char* result = "Producer";
    switch (reason)
    {
        case rc::RDGDependencyReason::eWriteAfterRead: result = "Write after read"; break;
        case rc::RDGDependencyReason::eWriteAfterWrite: result = "Write after write"; break;
        case rc::RDGDependencyReason::eLayoutChange: result = "Layout change"; break;
        default: break;
    }
    return result;
}

inline const rc::RDGDebugResource* FindResource(const rc::RDGDebugSnapshot& snapshot, int32_t id)
{
    return id >= 0 && size_t(id) < snapshot.resources.size() ? &snapshot.resources[id] : nullptr;
}

inline const char* QueueConnectionName(const rc::RDGDebugSnapshot& snapshot, const RenderGraphEdge& edge)
{
    const uint32_t source      = snapshot.passes[edge.source].group;
    const uint32_t destination = snapshot.passes[edge.destination].group;
    const bool     shared      = source < snapshot.groups.size() && destination < snapshot.groups.size()
                              && snapshot.groups[source].queueEquivalenceId == snapshot.groups[destination].queueEquivalenceId;
    return shared ? "Shared queue order" : "Semaphore wait";
}
} // namespace zen::editor::graph_drawing
