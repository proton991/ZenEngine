#pragma once
#include "Math/Math.h"
#include <cstdint>

namespace zen::editor
{
// One end of a world axis as the camera sees it, for orientation gizmos.
struct ViewAxisEnd
{
    // 0 = X, 1 = Y, 2 = Z.
    uint32_t axis{0};
    bool     negative{false};
    // Screen-space direction scaled by how much the axis faces the screen plane:
    // +x is right and +y is down. Length 1 lies in the screen plane, 0 points at the viewer.
    Vec2 offset{0.0f};
    // Positive toward the viewer.
    float depth{0.0f};

    Vec3 GetDirection() const;
};

// The six ends of the world axes, ordered back to front so later ends draw on top.
struct ViewAxes
{
    ViewAxisEnd ends[6];
};

// These use only the rotation of a right-handed view matrix, so projection type and
// camera position do not matter.

// A world direction in gizmo space: +x right, +y down and +z toward the viewer.
Vec3 ToViewAxesSpace(const Mat4& view, Vec3 direction);

ViewAxes ProjectViewAxes(const Mat4& view);
} // namespace zen::editor
