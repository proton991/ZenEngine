#include "Editor/Model/ViewAxes.h"
#include <algorithm>

namespace zen::editor
{
namespace
{
bool IsFarther(const ViewAxisEnd& left, const ViewAxisEnd& right)
{
    // Ties keep a stable X, Y, Z order with positive ends on top.
    return left.depth < right.depth
        || (left.depth == right.depth
            && (left.axis > right.axis || (left.axis == right.axis && left.negative && !right.negative)));
}
} // namespace

Vec3 ViewAxisEnd::GetDirection() const
{
    Vec3 direction(0.0f);

    direction[axis] = negative ? -1.0f : 1.0f;

    return direction;
}

Vec3 ToViewAxesSpace(const Mat4& view, Vec3 direction)
{
    // View space looks down -Z with +Y up; screen space has +Y down.
    const Vec3 eye = Mat3(view) * direction;

    return Vec3(eye.x, -eye.y, eye.z);
}

ViewAxes ProjectViewAxes(const Mat4& view)
{
    ViewAxes result;

    for (uint32_t index = 0; index < 6; ++index)
    {
        ViewAxisEnd& end = result.ends[index];

        end.axis         = index / 2;

        end.negative     = (index & 1) != 0;

        const Vec3 gizmo = ToViewAxesSpace(view, end.GetDirection());

        end.offset       = Vec2(gizmo);

        end.depth        = gizmo.z;
    }

    std::sort(std::begin(result.ends), std::end(result.ends), &IsFarther);

    return result;
}
} // namespace zen::editor
