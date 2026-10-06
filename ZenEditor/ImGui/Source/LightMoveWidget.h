#pragma once
#include "Editor/Model/EditorCamera.h"
#include "Graphics/RenderCore/V2/RenderingSettings.h"
#include "imgui.h"

namespace zen::editor
{
// Screen-space handles for positional lights. Edits go through the same rendering
// draft as the numeric controls; no scene nodes or renderer state are changed here.
class LightMoveWidget
{
public:
    bool Draw(HeapVector<rc::RenderingLight>& lights,
              const EditorCamera&             camera,
              uint64_t                        generation,
              ImVec2                          origin,
              ImVec2                          extent,
              bool                            enabled,
              bool                            allowed,
              bool                            canStart);

    // Restores only the position this drag still owns, never an external edit or a
    // light in a replacement scene that happens to reuse the same ID.
    bool Cancel(HeapVector<rc::RenderingLight>& lights, uint64_t generation);

    bool IsActive() const;

    // Includes the release/cancel frame, so it cannot also pick or move the camera.
    bool OwnsMouse() const;

private:
    uint64_t m_lightId{0};
    uint64_t m_generation{0};
    uint64_t m_cameraRevision{0};
    Vec3     m_startPosition{0.0f};
    Vec3     m_lastPosition{0.0f};
    Vec3     m_grab{0.0f};
    float    m_depth{0.0f};
    bool     m_ownsMouse{false};
};
} // namespace zen::editor
