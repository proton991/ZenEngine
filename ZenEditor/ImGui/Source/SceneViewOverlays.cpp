#include "SceneViewOverlays.h"
#include "Editor/ImGui/EditorTheme.h"
#include "Editor/Model/ViewAxes.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace zen::editor
{
namespace
{
const char* const kAxisNames[] = {"X", "Y", "Z"};

constexpr int kNoEnd           = -1;

constexpr int kCircleSegments  = 48;

struct SphereLayout
{
    bool   visible{false};
    ImVec2 center;
    float  sphereRadius{0.0f};
    float  endRadius{0.0f};
};

struct HintRow
{
    std::string keys;
    std::string action;
};

SphereLayout LayoutSphere(ImVec2 imageMin, ImVec2 imageMax)
{
    const float scale  = EditorScale();

    const float margin = 10 * scale;

    SphereLayout layout;

    layout.sphereRadius = 38 * scale;

    layout.endRadius    = 9 * scale;

    const float extent  = 2 * (layout.sphereRadius + layout.endRadius);

    // Leave most of a small image to the scene.
    layout.visible = imageMax.x - imageMin.x >= 3 * extent && imageMax.y - imageMin.y >= 2 * extent;

    layout.center  = ImVec2(imageMax.x - margin - extent * 0.5f, imageMin.y + margin + extent * 0.5f);

    return layout;
}

ImVec2 ToScreen(const SphereLayout& layout, Vec2 offset)
{
    return ImVec2(layout.center.x + offset.x * layout.sphereRadius, layout.center.y + offset.y * layout.sphereRadius);
}

float EndRadius(const SphereLayout& layout, const ViewAxisEnd& end)
{
    return end.negative ? layout.endRadius * 0.75f : layout.endRadius;
}

float DistanceSquared(ImVec2 first, ImVec2 second)
{
    const float x = first.x - second.x;

    const float y = first.y - second.y;

    return x * x + y * y;
}

ImU32 Fade(ImVec4 color, float alpha)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.x, color.y, color.z, color.w * alpha));
}

// A translucent ball with a soft highlight and its three great circles, brighter on the
// front half, so turning the camera visibly turns the sphere.
void DrawSphere(ImDrawList& draw, const SphereLayout& layout, const Mat4& view, bool highlighted)
{
    const float scale  = EditorScale();

    const float radius = layout.sphereRadius;

    draw.AddCircleFilled(layout.center, radius, ImGui::GetColorU32(ImGuiCol_WindowBg, highlighted ? 0.8f : 0.5f),
                         kCircleSegments);

    // Shrinking layers offset toward the top left suggest a light; each stays inside.
    for (int layer = 1; layer <= 3; ++layer)
    {
        const float step = float(layer) / 3;

        const ImVec2 center(layout.center.x - radius * 0.3f * step, layout.center.y - radius * 0.3f * step);

        draw.AddCircleFilled(center, radius * (1 - 0.45f * step), ImGui::GetColorU32(ImGuiCol_Text, 0.035f), kCircleSegments);
    }

    for (uint32_t normal = 0; normal < 3; ++normal)
    {
        Vec3 first(0.0f);

        Vec3 second(0.0f);

        first[(normal + 1) % 3]  = 1;

        second[(normal + 2) % 3] = 1;

        Vec3 previous            = ToViewAxesSpace(view, first);

        for (int segment = 1; segment <= kCircleSegments; ++segment)
        {
            const float angle = glm::two_pi<float>() * float(segment) / kCircleSegments;

            const Vec3 next   = ToViewAxesSpace(view, first * std::cos(angle) + second * std::sin(angle));

            const bool front  = previous.z + next.z >= 0;

            const float alpha = front ? (highlighted ? 0.55f : 0.35f) : 0.12f;

            draw.AddLine(ToScreen(layout, Vec2(previous)), ToScreen(layout, Vec2(next)),
                         ImGui::GetColorU32(ImGuiCol_Text, alpha), scale);

            previous = next;
        }
    }

    draw.AddCircle(layout.center, radius, ImGui::GetColorU32(ImGuiCol_Text, highlighted ? 0.5f : 0.25f), kCircleSegments,
                   scale);
}

// Positive ends are labeled bubbles joined to the center; negative ends are rings.
// Ends behind the sphere's center are faded.
void DrawEnd(ImDrawList& draw, const SphereLayout& layout, const ViewAxisEnd& end, bool hovered)
{
    const EditorPalette& palette = GetEditorPalette();

    const ImVec4 color           = palette.axis[end.axis];

    const float fade             = end.depth < 0 ? 0.6f : 1.0f;

    const ImVec2 point           = ToScreen(layout, end.offset);

    const float radius           = EndRadius(layout, end);

    const float scale            = EditorScale();

    if (end.negative)
    {
        draw.AddCircleFilled(point, radius, Fade(color, 0.3f * fade));

        draw.AddCircle(point, radius, Fade(color, fade), 0, 1.5f * scale);
    }
    else
    {
        draw.AddLine(layout.center, point, Fade(color, fade), 2 * scale);

        draw.AddCircleFilled(point, radius, Fade(color, fade));

        const ImVec2 size = ImGui::CalcTextSize(kAxisNames[end.axis]);

        draw.AddText(ImVec2(point.x - size.x * 0.5f, point.y - size.y * 0.5f), palette.axisLabel, kAxisNames[end.axis]);
    }

    if (hovered)
    {
        draw.AddCircle(point, radius + 1.5f * scale, ImGui::GetColorU32(ImGuiCol_Text), 0, 2 * scale);
    }
}

// Front-most first, so the end drawn on top wins.
int FindHoveredEnd(const SphereLayout& layout, const ViewAxes& axes, ImVec2 mouse)
{
    int hovered = kNoEnd;

    for (int index = 5; hovered == kNoEnd && index >= 0; --index)
    {
        const float radius = EndRadius(layout, axes.ends[index]);

        if (DistanceSquared(mouse, ToScreen(layout, axes.ends[index].offset)) <= radius * radius)
        {
            hovered = index;
        }
    }

    return hovered;
}

void AddActionRow(const EditorActions& registry, const char* id, HeapVector<HintRow>& rows)
{
    const EditorAction* action = registry.Find(id);

    if (action != nullptr && action->shortcut.key != platform::Key::Unknown)
    {
        rows.push_back({FormatShortcut(action->shortcut), action->label});
    }
}
} // namespace

ViewAxesHover DrawViewAxes(const Mat4& view, ImVec2 imageMin, ImVec2 imageMax, bool interactive, bool active)
{
    ViewAxesHover hover;

    const SphereLayout layout = LayoutSphere(imageMin, imageMax);

    if (layout.visible)
    {
        const ViewAxes axes = ProjectViewAxes(view);

        const ImVec2 mouse  = ImGui::GetIO().MousePos;

        const int hit       = interactive ? FindHoveredEnd(layout, axes, mouse) : kNoEnd;

        const float radius  = layout.sphereRadius;

        hover.sphere        = interactive && (hit != kNoEnd || DistanceSquared(mouse, layout.center) <= radius * radius);

        hover.radius        = radius;

        ImDrawList& draw    = *ImGui::GetWindowDrawList();

        DrawSphere(draw, layout, view, hover.sphere || active);

        for (int index = 0; index < 6; ++index)
        {
            DrawEnd(draw, layout, axes.ends[index], index == hit);
        }

        if (hit != kNoEnd)
        {
            const ViewAxisEnd& end = axes.ends[hit];

            hover.axis             = end.GetDirection();

            ImGui::SetTooltip("View from %s%s", end.negative ? "-" : "+", kAxisNames[end.axis]);
        }

        if (hover.sphere || active)
        {
            ImGui::SetMouseCursor(hit != kNoEnd ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_ResizeAll);
        }
    }

    return hover;
}

bool GetViewAxisPosition(const Mat4& view, ImVec2 imageMin, ImVec2 imageMax, Vec3 direction, ImVec2& position)
{
    const SphereLayout layout = LayoutSphere(imageMin, imageMax);

    bool found                = false;

    if (layout.visible)
    {
        const ViewAxes axes = ProjectViewAxes(view);

        for (const ViewAxisEnd& end : axes.ends)
        {
            if (end.GetDirection() == direction)
            {
                position = ToScreen(layout, end.offset);

                found    = true;
            }
        }
    }

    return found;
}

bool GetViewSphereCenter(ImVec2 imageMin, ImVec2 imageMax, ImVec2& center, float& radius)
{
    const SphereLayout layout = LayoutSphere(imageMin, imageMax);

    center                    = layout.center;

    radius                    = layout.sphereRadius;

    return layout.visible;
}

void ViewSphereInput::Update(EditorCamera& camera, const ViewAxesHover& hover, bool canStart)
{
    if (m_active)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            Vec2 drag(0.0f);

            if (m_dragged)
            {
                drag = Vec2(ImGui::GetIO().MouseDelta.x, ImGui::GetIO().MouseDelta.y);
            }
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                // Include the movement below the threshold, so the sphere does not lag.
                const ImVec2 total = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);

                drag               = Vec2(total.x, total.y);

                m_dragged          = true;
            }

            camera.OrbitBy(drag / m_radius);
        }
        else
        {
            if (!m_dragged && m_axis != Vec3(0.0f))
            {
                // Clicking +X views the scene from the +X side, looking toward -X.
                camera.LookAlong(-m_axis);
            }

            m_active = false;
        }
    }
    else if (canStart && hover.sphere && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        m_active  = true;

        m_dragged = false;

        m_axis    = hover.axis;

        m_radius  = std::max(hover.radius, 1.0f);
    }
}

bool ViewSphereInput::IsActive() const
{
    return m_active;
}

void ViewSphereInput::Cancel()
{
    m_active = false;
}

void DrawSceneControlsHint(const EditorActions& registry, ImVec2 imageMin, ImVec2 imageMax)
{
    HeapVector<HintRow> rows;

    rows.push_back({"Right drag", "Look"});

    rows.push_back({"+ W A S D / Q E", "Fly (Shift: faster)"});

    rows.push_back({"Alt + left drag", "Orbit"});

    rows.push_back({"Middle drag", "Pan"});

    rows.push_back({"Wheel", "Zoom"});

    rows.push_back({"Left click", "Select"});

    AddActionRow(registry, actions::FrameSelection, rows);

    AddActionRow(registry, actions::FrameAll, rows);

    rows.push_back({"Drag the sphere", "Orbit"});

    rows.push_back({"Click an axis", "View from that side"});

    rows.push_back({"Esc", "Stop navigating"});

    const float scale   = EditorScale();

    const float padding = 8 * scale;

    const float gap     = 14 * scale;

    const float margin  = 10 * scale;

    const float spacing = 2 * scale;

    const float line    = ImGui::GetTextLineHeight();

    float keysWidth     = 0;

    float actionsWidth  = 0;

    for (const HintRow& row : rows)
    {
        keysWidth    = std::max(keysWidth, ImGui::CalcTextSize(row.keys.c_str()).x);

        actionsWidth = std::max(actionsWidth, ImGui::CalcTextSize(row.action.c_str()).x);
    }

    const float count = float(rows.size());

    const ImVec2 size(padding * 2 + keysWidth + gap + actionsWidth, padding * 2 + count * line + (count - 1) * spacing);

    // The list never covers most of the view; small images skip it.
    if (size.x + 2 * margin <= imageMax.x - imageMin.x && size.y + 2 * margin <= (imageMax.y - imageMin.y) * 0.75f)
    {
        ImDrawList& draw = *ImGui::GetWindowDrawList();

        const ImVec2 start(imageMin.x + margin, imageMax.y - margin - size.y);

        const float corner = 5 * scale;

        draw.AddRectFilled(start, ImVec2(start.x + size.x, start.y + size.y), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f),
                           corner);

        draw.AddRect(start, ImVec2(start.x + size.x, start.y + size.y), ImGui::GetColorU32(ImGuiCol_Border), corner);

        const ImU32 keyColor    = ImGui::GetColorU32(ImGuiCol_Text);

        const ImU32 actionColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);

        float y                 = start.y + padding;

        for (const HintRow& row : rows)
        {
            draw.AddText(ImVec2(start.x + padding, y), keyColor, row.keys.c_str());

            draw.AddText(ImVec2(start.x + padding + keysWidth + gap, y), actionColor, row.action.c_str());

            y += line + spacing;
        }
    }
}
} // namespace zen::editor
