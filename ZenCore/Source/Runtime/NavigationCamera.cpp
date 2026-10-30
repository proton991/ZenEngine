#include "Runtime/NavigationCamera.h"
#include "Math/Finite.h"
#include "Platform/InputController.h"
#include <algorithm>
#include <cmath>

namespace zen::runtime
{
namespace
{
// Keeps the view direction away from the world up vector used by the view matrix.
constexpr float kMaxPitch = 1.55f;
} // namespace

float ClampNavigationCameraMoveSpeed(float speed)
{
    return std::isfinite(speed) ? std::clamp(speed, kMinNavigationCameraMoveSpeed, kMaxNavigationCameraMoveSpeed)
                                : kDefaultNavigationCameraMoveSpeed;
}

CameraInput ReadNavigationCameraInput(float seconds, float moveSpeed, float wheel)
{
    platform::KeyboardMouseInput& keys  = platform::KeyboardMouseInput::GetInstance();
    const std::array<float, 2>    delta = keys.CalculateCursorPositionDelta();
    // Platform deltas use +Y up; CameraInput follows editor/ImGui screen coordinates (+Y down).
    const Vec2  screenDelta(delta[0], -delta[1]);
    CameraInput input;
    input.seconds   = seconds;
    input.moveSpeed = moveSpeed;
    input.dolly     = wheel;
    input.fast      = keys.IsKeyPressed(platform::Key::LeftShift) || keys.IsKeyPressed(platform::Key::RightShift);
    if (keys.IsMouseButtonPressed(platform::MouseButton::Right))
    {
        input.look = screenDelta;
        input.move = Vec3(float(keys.IsKeyPressed(platform::Key::D)) - float(keys.IsKeyPressed(platform::Key::A)),
                          float(keys.IsKeyPressed(platform::Key::E)) - float(keys.IsKeyPressed(platform::Key::Q)),
                          float(keys.IsKeyPressed(platform::Key::W)) - float(keys.IsKeyPressed(platform::Key::S)));
    }
    else if (keys.IsMouseButtonPressed(platform::MouseButton::Middle))
    {
        input.pan = screenDelta;
    }
    else if (keys.IsMouseButtonPressed(platform::MouseButton::Left)
             && (keys.IsKeyPressed(platform::Key::LeftAlt) || keys.IsKeyPressed(platform::Key::RightAlt)))
    {
        input.orbit = screenDelta;
    }
    return input;
}

bool ValidateNavigationCameraState(const NavigationCameraState& state)
{
    const Vec3  direction = state.target - state.eye;
    const float distance  = glm::length(direction);
    return IsFinite(state.eye) && IsFinite(state.target) && IsFinite(distance) && distance > 0.000001f
        && std::abs(direction.y / distance) <= std::sin(kMaxPitch) + 0.000001f && IsFinite(state.sceneScale)
        && state.sceneScale > 0.0f && IsFinite(state.aspect) && state.aspect > 0.0f && IsFinite(state.verticalFovDegrees)
        && state.verticalFovDegrees > 0.0f && state.verticalFovDegrees < 179.0f && IsFinite(state.nearPlane)
        && state.nearPlane > 0.0f && IsFinite(state.farPlane) && state.farPlane > state.nearPlane;
}

NavigationCameraState NavigationCamera::GetState() const
{
    const asset::CameraParameters parameters = m_camera.GetParameters();
    return {m_eye,
            m_target,
            m_scale,
            parameters.aspect,
            glm::degrees(parameters.verticalFov),
            parameters.nearPlane,
            parameters.farPlane,
            parameters.orthographic};
}

bool NavigationCamera::SetState(const NavigationCameraState& state)
{
    const bool valid = ValidateNavigationCameraState(state);
    if (valid)
    {
        m_eye                = state.eye;
        m_target             = state.target;
        m_distance           = glm::length(m_target - m_eye);
        const Vec3 direction = (m_target - m_eye) / m_distance;
        m_yaw                = std::atan2(direction.z, direction.x);
        m_pitch              = std::asin(std::clamp(direction.y, -1.0f, 1.0f));
        m_scale              = state.sceneScale;
        m_camera.SetProjectionType(state.orthographic ? sg::CameraProjectionType::eOrthographic
                                                      : sg::CameraProjectionType::ePerspective);
        m_camera.SetNearPlane(state.nearPlane);
        m_camera.SetFarPlane(state.farPlane);
        m_camera.SetFieldOfView(state.verticalFovDegrees);
        m_camera.SetOrthoRect(Vec4(-m_distance * state.aspect, m_distance * state.aspect, -m_distance, m_distance));
        m_camera.UpdateAspect(state.aspect);
        Publish();
    }
    return valid;
}

NavigationCamera::NavigationCamera() : m_camera(Vec3(0, 0, 2), Vec3(0), 1.0f) {}

void NavigationCamera::SynchronizeFromCamera()
{
    const Mat4 view    = m_camera.GetViewMatrix();
    const Vec3 forward = -Vec3(view[0][2], view[1][2], view[2][2]);
    const Vec2 angles  = m_camera.GetViewAngles();
    m_eye              = m_camera.GetPos();
    m_yaw              = angles.x;
    m_pitch            = angles.y;
    m_target           = m_eye + forward * m_distance;
    ++m_revision;
}

Vec3 NavigationCamera::Forward() const
{
    return Vec3(std::cos(m_pitch) * std::cos(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::sin(m_yaw));
}

void NavigationCamera::Publish()
{
    // Projection and authored roll remain owned by Camera across navigation updates.
    m_camera.SetPose(m_eye, Vec2(m_yaw, m_pitch), m_distance);

    ++m_revision;
}

void NavigationCamera::Frame(const sg::AABB& bounds)
{
    if (glm::all(glm::lessThanEqual(bounds.GetMin(), bounds.GetMax())))
    {
        m_scale = std::max(bounds.GetScale(), 0.02f);
        m_camera.SetNearPlane(std::max(0.0001f, m_scale * 0.001f));

        m_distance = m_scale * std::max(1.0f, 1.0f / m_camera.GetAspect());
        if (IsOrthographic())
        {
            const float width = m_distance * m_camera.GetAspect();
            m_camera.SetOrthoRect(Vec4(-width, width, -m_distance, m_distance));
        }
        else
        {
            // Fit the bounding sphere through the narrower axis of the retained lens.
            const asset::CameraParameters parameters = m_camera.GetParameters();
            const float halfAngle = std::atan(std::tan(parameters.verticalFov * 0.5f) * std::min(parameters.aspect, 1.0f));
            m_distance            = std::max(m_distance, 1.1f * m_scale * 0.5f / std::sin(halfAngle));
        }
        // Keep depth margin even when a narrow lens frames far beyond the scene scale.
        m_camera.SetFarPlane(std::max({100.0f, m_scale * 100.0f, 2.0f * m_distance + m_scale}));

        m_target = bounds.GetCenter();

        m_eye    = m_target - Forward() * m_distance;

        Publish();
    }
}

void NavigationCamera::Apply(const CameraInput& input)
{
    const bool changed =
        input.look != Vec2(0) || input.orbit != Vec2(0) || input.pan != Vec2(0) || input.move != Vec3(0) || input.dolly != 0;

    if (changed)
    {
        const Vec2 rotation  = input.look + input.orbit;

        m_yaw               += rotation.x * 0.004f;

        // Keep authored pole views stable on translation or horizontal look input.
        if (rotation.y != 0)
        {
            m_pitch = std::clamp(m_pitch - rotation.y * 0.004f, std::min(-kMaxPitch, m_pitch), std::max(kMaxPitch, m_pitch));
        }

        const Vec3 forward = Forward();

        if (input.orbit != Vec2(0))
        {
            m_eye = m_target - forward * m_distance;
        }
        else
        {
            m_target = m_eye + forward * m_distance;
        }

        // Resolve current-frame rotation through Camera so pan and strafe retain its roll.
        if (rotation != Vec2(0))
        {
            m_camera.SetPose(m_eye, Vec2(m_yaw, m_pitch), m_distance);
        }
        const Mat4 view = m_camera.GetViewMatrix();
        const Vec3 right(view[0][0], view[1][0], view[2][0]);
        const Vec3 up(view[0][1], view[1][1], view[2][1]);

        const float speed             = ClampNavigationCameraMoveSpeed(input.moveSpeed) * std::clamp(input.seconds, 0.0f, 0.1f)
                                      * (input.fast ? 3.0f : 1.0f);

        const Vec3 movement           = right * input.move.x + Vec3(0, 1, 0) * input.move.y + forward * input.move.z;

        const Vec3 shift              = movement / std::max(1.0f, glm::length(movement)) * speed
                                      + (-right * input.pan.x + up * input.pan.y) * m_distance * 0.002f;

        m_eye                        += shift;

        m_target                     += shift;

        const float previousDistance  = m_distance;
        // A narrow lens can frame beyond the normal zoom range; move inward without snapping.
        m_distance = std::clamp(previousDistance * std::exp(-input.dolly * 0.15f), std::min(m_scale * 0.005f, previousDistance),
                                std::max(m_scale * 50.0f, previousDistance));
        if (IsOrthographic() && m_distance != previousDistance)
        {
            const asset::CameraParameters parameters = m_camera.GetParameters();
            const float                   scale      = m_distance / previousDistance;
            m_camera.SetOrthoRect(Vec4(-parameters.xmag, parameters.xmag, -parameters.ymag, parameters.ymag) * scale);
        }

        m_eye = m_target - forward * m_distance;

        Publish();
    }
}

void NavigationCamera::OrbitBy(Vec2 radians)
{
    if (radians != Vec2(0.0f))
    {
        m_yaw += radians.x;

        if (radians.y != 0)
        {
            m_pitch = std::clamp(m_pitch - radians.y, std::min(-kMaxPitch, m_pitch), std::max(kMaxPitch, m_pitch));
        }

        m_eye = m_target - Forward() * m_distance;

        Publish();
    }
}

void NavigationCamera::LookAlong(Vec3 direction)
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

void NavigationCamera::SetExtent(uint32_t width, uint32_t height)
{
    const asset::CameraParameters parameters = m_camera.GetParameters();
    if (width > 0 && height > 0 && !parameters.fixedAspect && parameters.aspect != float(width) / float(height))
    {
        const float aspect = float(width) / float(height);
        if (parameters.orthographic)
        {
            const float xmag = parameters.xmag * aspect / parameters.aspect;
            m_camera.SetOrthoRect(Vec4(-xmag, xmag, -parameters.ymag, parameters.ymag));
        }
        m_camera.UpdateAspect(aspect);
        ++m_revision;
    }
}

void NavigationCamera::SetOrthographic(bool enabled)
{
    if (enabled != IsOrthographic())
    {
        m_camera.SetProjectionType(enabled ? sg::CameraProjectionType::eOrthographic : sg::CameraProjectionType::ePerspective);
        const float width = m_distance * m_camera.GetAspect();
        m_camera.SetOrthoRect(Vec4(-width, width, -m_distance, m_distance));
        m_camera.UpdateAspect(m_camera.GetAspect());
        ++m_revision;
    }
}

bool NavigationCamera::IsOrthographic() const
{
    return m_camera.GetParameters().orthographic;
}

sg::Camera& NavigationCamera::GetCamera()
{
    return m_camera;
}

const sg::Camera& NavigationCamera::GetCamera() const
{
    return m_camera;
}

uint64_t NavigationCamera::GetRevision() const
{
    return m_revision;
}

void NavigationCamera::MakeRay(Vec2 normalized, Vec3& origin, Vec3& direction) const
{
    const Mat4 inverse       = glm::inverse(m_camera.GetProjectionMatrix());

    const Vec2 ndc           = normalized * 2.0f - Vec2(1.0f);

    const Vec4 nearPoint     = inverse * Vec4(ndc, 0.0f, 1.0f);

    const Vec4 farPoint      = inverse * Vec4(ndc, 1.0f, 1.0f);

    const Vec3 viewOrigin    = Vec3(nearPoint) / nearPoint.w;
    const Mat3 worldRotation = glm::transpose(Mat3(m_camera.GetViewMatrix()));

    origin                   = m_camera.GetPos() + worldRotation * viewOrigin;

    // Keep the far point homogeneous: an infinite far plane has w == 0.
    // Form the direction in view space to avoid cancellation at a translated eye.
    direction = glm::normalize(worldRotation * (Vec3(farPoint) - viewOrigin * farPoint.w));
}
} // namespace zen::runtime
