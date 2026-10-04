#pragma once
// Overlays drawn over the Scene view's image. Private to the ImGui frontend; the
// orientation math lives in the model's ViewAxes.
#include "Editor/Model/EditorActions.h"
#include "Editor/Model/EditorCamera.h"
#include "imgui.h"

namespace zen::editor
{
// What the mouse is over in the orientation sphere this frame.
struct ViewAxesHover
{
    // The sphere, including its axis ends.
    bool sphere{false};
    // World direction of the axis end under the mouse, or zero.
    Vec3 axis{0.0f};
    // Sphere radius in pixels; dragging this far turns the view by one radian.
    float radius{0.0f};
};

// Draws the orientation sphere in the top-right corner of the image: a ball with world
// X/Y/Z that turns with the camera. Only an interactive sphere tests the mouse; active
// keeps the highlight during a drag. The caller decides what clicks and drags do.
// Images too small to keep the sphere clear of the scene skip it.
ViewAxesHover DrawViewAxes(const Mat4& view, ImVec2 imageMin, ImVec2 imageMax, bool interactive, bool active);

// Where DrawViewAxes places the end of an axis direction such as -Z; false when the
// image is too small for the sphere.
bool GetViewAxisPosition(const Mat4& view, ImVec2 imageMin, ImVec2 imageMax, Vec3 direction, ImVec2& position);

// The sphere's center and radius in pixels; false when the image is too small for it.
bool GetViewSphereCenter(ImVec2 imageMin, ImVec2 imageMax, ImVec2& center, float& radius);

// The orientation sphere's mouse handling, binding it to the camera. A left drag that
// starts on the sphere and passes ImGui's drag threshold orbits the camera, so the
// sphere's surface follows the cursor. A left click on an axis end without dragging
// views the scene from that side. Call once per frame after DrawViewAxes.
class ViewSphereInput
{
public:
    // canStart allows a left press over the sphere to start an interaction.
    void Update(EditorCamera& camera, const ViewAxesHover& hover, bool canStart);

    // While active the sphere owns the left button, including outside the sphere.
    bool IsActive() const;

    void Cancel();

private:
    bool  m_active{false};
    bool  m_dragged{false};
    Vec3  m_axis{0.0f};
    float m_radius{1.0f};
};

// Lists the Scene view's mouse and keyboard controls in the bottom-left corner of the
// image. Frame commands show their registered labels and shortcuts. Skipped when the
// image is too small to hold the list.
void DrawSceneControlsHint(const EditorActions& registry, ImVec2 imageMin, ImVec2 imageMax);
} // namespace zen::editor
