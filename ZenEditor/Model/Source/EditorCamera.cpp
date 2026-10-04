#include "Editor/Model/EditorCamera.h"
#include <algorithm>
#include <cmath>

namespace zen::editor
{
namespace
{
// Keeps the view direction away from the world up vector used by the view matrix.
constexpr float kMaxPitch = 1.55f;
} // namespace

EditorCamera::EditorCamera() : m_camera(Vec3(0, 0, 2), Vec3(0), 1.0f) {}

Vec3 EditorCamera::Forward() const
{
    return Vec3(std::cos(m_pitch) * std::cos(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::sin(m_yaw));
}

void EditorCamera::Publish()
{
    m_camera.SetProjectionType(m_orthographic ? sg::CameraProjectionType::eOrthographic
                                              : sg::CameraProjectionType::ePerspective);

    m_camera.SetNearPlane(std::max(0.0001f, m_scale * 0.001f));

    m_camera.SetFarPlane(std::max(100.0f, m_scale * 100.0f));

    m_camera.SetOrthoRect(Vec4(-m_distance * m_aspect, m_distance * m_aspect, -m_distance, m_distance));

    m_camera.UpdateAspect(m_aspect);

    m_camera.SetPose(m_eye, m_target);

    ++m_revision;
}

void EditorCamera::Frame(const sg::AABB& bounds)
{
    if (glm::all(glm::lessThanEqual(bounds.GetMin(), bounds.GetMax())))
    {
        m_scale    = std::max(bounds.GetScale(), 0.02f);

        m_distance = m_scale * std::max(1.0f, 1.0f / m_aspect);

        m_target   = bounds.GetCenter();

        m_eye      = m_target - Forward() * m_distance;

        Publish();
    }
}

void EditorCamera::Apply(const CameraInput& input)
{
    const bool changed =
        input.look != Vec2(0) || input.orbit != Vec2(0) || input.pan != Vec2(0) || input.move != Vec3(0) || input.dolly != 0;

    if (changed)
    {
        const Vec2 rotation  = input.look + input.orbit;

        m_yaw               += rotation.x * 0.004f;

        m_pitch              = std::clamp(m_pitch - rotation.y * 0.004f, -kMaxPitch, kMaxPitch);

        const Vec3 forward   = Forward();

        const Vec3 right     = glm::normalize(glm::cross(forward, Vec3(0, 1, 0)));

        const Vec3 up        = glm::normalize(glm::cross(right, forward));

        if (input.orbit != Vec2(0))
        {
            m_eye = m_target - forward * m_distance;
        }
        else
        {
            m_target = m_eye + forward * m_distance;
        }

        const float speed = m_scale * std::clamp(input.seconds, 0.0f, 0.1f) * (input.fast ? 3.0f : 1.0f);

        const Vec3 shift  = (right * input.move.x + Vec3(0, 1, 0) * input.move.y + forward * input.move.z) * speed
                         + (-right * input.pan.x + up * input.pan.y) * m_distance * 0.002f;

        m_eye      += shift;

        m_target   += shift;

        m_distance  = std::clamp(m_distance * std::exp(-input.dolly * 0.15f), m_scale * 0.005f, m_scale * 50.0f);

        m_eye       = m_target - forward * m_distance;

        Publish();
    }
}

void EditorCamera::OrbitBy(Vec2 radians)
{
    if (radians != Vec2(0.0f))
    {
        m_yaw   += radians.x;

        m_pitch  = std::clamp(m_pitch - radians.y, -kMaxPitch, kMaxPitch);

        m_eye    = m_target - Forward() * m_distance;

        Publish();
    }
}

void EditorCamera::LookAlong(Vec3 direction)
{
    const float length = glm::length(direction);

    if (length > 0.0f)
    {
        const Vec3 forward = direction / length;

        // Vertical views have no yaw of their own; facing -Z keeps X to the right.
        m_yaw   = std::abs(forward.y) > 0.999f ? -glm::half_pi<float>() : std::atan2(forward.z, forward.x);

        m_pitch = std::clamp(std::asin(std::clamp(forward.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);

        m_eye   = m_target - Forward() * m_distance;

        Publish();
    }
}

void EditorCamera::SetExtent(uint32_t width, uint32_t height)
{
    if (width > 0 && height > 0 && m_aspect != float(width) / float(height))
    {
        m_aspect = float(width) / float(height);

        Publish();
    }
}

void EditorCamera::SetOrthographic(bool enabled)
{
    if (enabled != m_orthographic)
    {
        m_orthographic = enabled;

        Publish();
    }
}

bool EditorCamera::IsOrthographic() const
{
    return m_orthographic;
}

sg::Camera& EditorCamera::GetCamera()
{
    return m_camera;
}

const sg::Camera& EditorCamera::GetCamera() const
{
    return m_camera;
}

uint64_t EditorCamera::GetRevision() const
{
    return m_revision;
}

void EditorCamera::MakeRay(Vec2 normalized, Vec3& origin, Vec3& direction) const
{
    const Mat4 inverse   = glm::inverse(m_camera.GetProjectionMatrix() * m_camera.GetViewMatrix());

    const Vec2 ndc       = normalized * 2.0f - Vec2(1.0f);

    const Vec4 nearPoint = inverse * Vec4(ndc, 0.0f, 1.0f);

    const Vec4 farPoint  = inverse * Vec4(ndc, 1.0f, 1.0f);

    origin               = Vec3(nearPoint) / nearPoint.w;

    direction            = glm::normalize(Vec3(farPoint) / farPoint.w - origin);
}
} // namespace zen::editor
