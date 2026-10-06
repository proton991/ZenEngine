#include "LightMoveWidget.h"
#include "Editor/ImGui/EditorTheme.h"
#include <cmath>

namespace zen::editor
{
namespace
{
bool IsFinite(Vec3 value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool ProjectLight(const Mat4& matrix, Vec3 position, Vec3& projected)
{
    const Vec4 clip    = matrix * Vec4(position, 1.0f);
    bool       visible = clip.w > 0.0f;
    if (visible)
    {
        projected = Vec3(clip) / clip.w;
        visible   = IsFinite(projected) && std::abs(projected.x) <= 1.0f && std::abs(projected.y) <= 1.0f && projected.z >= 0.0f
                 && projected.z <= 1.0f;
    }
    return visible;
}

bool UnprojectMouse(const Mat4& inverse, ImVec2 origin, ImVec2 extent, float depth, Vec3& position)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const Vec2   ndc((mouse.x - origin.x) / extent.x * 2.0f - 1.0f, (mouse.y - origin.y) / extent.y * 2.0f - 1.0f);
    const Vec4   world = inverse * Vec4(ndc, depth, 1.0f);
    bool         valid = std::isfinite(world.w) && std::abs(world.w) > 1e-7f;
    if (valid)
    {
        position = Vec3(world) / world.w;
        valid    = IsFinite(position);
    }
    return valid;
}

rc::RenderingLight* FindLight(HeapVector<rc::RenderingLight>& lights, uint64_t id)
{
    rc::RenderingLight* result = nullptr;
    for (rc::RenderingLight& entry : lights)
    {
        if (entry.id == id && id != 0)
        {
            result = &entry;
            break;
        }
    }
    return result;
}

void DrawLightHandle(const rc::RenderingLight& entry,
                     const Mat4&               matrix,
                     ImVec2                    origin,
                     ImVec2                    extent,
                     float                     radius,
                     bool                      highlight)
{
    Vec3 projected;
    if (entry.light.type != rc::SceneLightType::eDirectional && ProjectLight(matrix, entry.light.position, projected))
    {
        const ImVec2 screen(origin.x + (projected.x + 1.0f) * 0.5f * extent.x,
                            origin.y + (projected.y + 1.0f) * 0.5f * extent.y);
        const ImU32  color =
            ImGui::GetColorU32(highlight ? ImGuiCol_PlotHistogram : ImGuiCol_Text, entry.light.enabled ? 1.0f : 0.55f);
        ImDrawList& draw = *ImGui::GetWindowDrawList();
        draw.AddCircleFilled(screen, radius, ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f));
        draw.AddCircle(screen, radius, color, 0, 2.0f * EditorScale());
        draw.AddLine(ImVec2(screen.x - radius * 0.5f, screen.y), ImVec2(screen.x + radius * 0.5f, screen.y), color);
        draw.AddLine(ImVec2(screen.x, screen.y - radius * 0.5f), ImVec2(screen.x, screen.y + radius * 0.5f), color);
    }
}
} // namespace

bool LightMoveWidget::Cancel(HeapVector<rc::RenderingLight>& lights, uint64_t generation)
{
    rc::RenderingLight* entry   = FindLight(lights, m_lightId);
    const bool          changed = generation == m_generation && entry != nullptr && entry->light.position == m_lastPosition
                               && entry->light.position != m_startPosition;
    if (changed)
    {
        entry->light.position = m_startPosition;
    }
    m_lightId = 0;
    return changed;
}

bool LightMoveWidget::IsActive() const
{
    return m_lightId != 0;
}

bool LightMoveWidget::OwnsMouse() const
{
    return m_ownsMouse;
}

bool LightMoveWidget::Draw(HeapVector<rc::RenderingLight>& lights,
                           const EditorCamera&             camera,
                           uint64_t                        generation,
                           ImVec2                          origin,
                           ImVec2                          extent,
                           bool                            enabled,
                           bool                            allowed,
                           bool                            canStart)
{
    const ImGuiIO& io          = ImGui::GetIO();
    const bool     escape      = ImGui::IsKeyPressed(ImGuiKey_Escape);
    const bool     validExtent = extent.x > 1.0f && extent.y > 1.0f;
    bool           changed     = false;
    m_ownsMouse                = IsActive();

    rc::RenderingLight* active = FindLight(lights, m_lightId);
    if (IsActive()
        && (!enabled || !allowed || !validExtent || escape || generation != m_generation
            || camera.GetRevision() != m_cameraRevision || active == nullptr
            || active->light.type == rc::SceneLightType::eDirectional || active->light.position != m_lastPosition))
    {
        changed = Cancel(lights, generation);
        active  = nullptr;
    }

    if (enabled && validExtent)
    {
        const Mat4          matrix  = camera.GetCamera().GetProjectionMatrix() * camera.GetCamera().GetViewMatrix();
        const Mat4          inverse = glm::inverse(matrix);
        const float         radius  = 9.0f * EditorScale();
        rc::RenderingLight* hovered = nullptr;
        float               nearest = 2.0f;

        // Choose the frontmost handle when lights overlap. These authoring handles
        // are deliberately visible through geometry, including disabled lights.
        for (rc::RenderingLight& entry : lights)
        {
            Vec3 projected;
            if (entry.light.type != rc::SceneLightType::eDirectional && ProjectLight(matrix, entry.light.position, projected))
            {
                const Vec2 screen = Vec2(origin.x, origin.y) + (Vec2(projected) + Vec2(1.0f)) * 0.5f * Vec2(extent.x, extent.y);
                const Vec2 delta  = screen - Vec2(io.MousePos.x, io.MousePos.y);
                if (entry.id != 0 && glm::dot(delta, delta) <= radius * radius && projected.z < nearest)
                {
                    nearest = projected.z;
                    hovered = &entry;
                }
            }
        }

        if (!IsActive() && !m_ownsMouse && allowed && canStart && !escape && !io.KeyAlt && !io.KeyCtrl && !io.KeySuper
            && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) && hovered != nullptr
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            if (UnprojectMouse(inverse, origin, extent, nearest, m_grab))
            {
                m_lightId        = hovered->id;
                m_generation     = generation;
                m_cameraRevision = camera.GetRevision();
                m_startPosition = m_lastPosition = hovered->light.position;
                m_depth                          = nearest;
                active                           = hovered;
                m_ownsMouse                      = true;
            }
        }

        if (IsActive())
        {
            // Include the final mouse position delivered with the release event.
            if ((ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Left))
                && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= io.MouseDragThreshold * io.MouseDragThreshold
                && ImGui::IsMousePosValid())
            {
                Vec3 position;
                if (UnprojectMouse(inverse, origin, extent, m_depth, position))
                {
                    // Preserve the grab offset so picking the edge never snaps the light.
                    position = m_startPosition + position - m_grab;
                    if (IsFinite(position) && position != m_lastPosition)
                    {
                        active->light.position = m_lastPosition = position;
                        changed                                 = true;
                    }
                }
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                m_lightId = 0;
            }
        }

        ImDrawList& draw = *ImGui::GetWindowDrawList();
        draw.PushClipRect(origin, ImVec2(origin.x + extent.x, origin.y + extent.y), true);
        const rc::RenderingLight* highlighted = IsActive() ? active : (allowed && canStart ? hovered : nullptr);
        for (const rc::RenderingLight& entry : lights)
        {
            if (&entry != highlighted)
            {
                DrawLightHandle(entry, matrix, origin, extent, radius, false);
            }
        }
        // Match hit testing even if an overlapping light is later in the settings list.
        if (highlighted != nullptr)
        {
            DrawLightHandle(*highlighted, matrix, origin, extent, radius, true);
        }
        draw.PopClipRect();

        if (IsActive() || (allowed && canStart && hovered != nullptr && !io.KeyAlt))
        {
            const rc::RenderingLight& entry = IsActive() ? *active : *hovered;
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetTooltip(
                "%s light %llu%s\nDrag to move across the view. Esc cancels.\nOrbit the camera to move in another plane.",
                entry.light.type == rc::SceneLightType::eSpot ? "Spot" : "Point", static_cast<unsigned long long>(entry.id),
                entry.light.enabled ? "" : " (disabled)");
        }
    }
    return changed;
}
} // namespace zen::editor
