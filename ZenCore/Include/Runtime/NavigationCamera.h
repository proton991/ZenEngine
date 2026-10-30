#pragma once
#include "SceneGraph/Camera.h"

namespace zen::runtime
{
constexpr float kDefaultNavigationCameraMoveSpeed = 1.0f;
constexpr float kMinNavigationCameraMoveSpeed     = 0.001f;
constexpr float kMaxNavigationCameraMoveSpeed     = 100.0f;

// Non-finite values restore the default; finite values stay within the UI's range.
float ClampNavigationCameraMoveSpeed(float speed);

struct CameraInput
{
    Vec2  look{0.0f};
    Vec2  orbit{0.0f};
    Vec2  pan{0.0f};
    Vec3  move{0.0f};
    float dolly{0.0f};
    float seconds{0.0f};
    // World units per second, independent of the last framed bounds. Imported
    // editor scenes have a longest extent of one, regardless of glTF source units.
    float moveSpeed{kDefaultNavigationCameraMoveSpeed};
    bool  fast{false};
};

struct NavigationCameraState
{
    Vec3  eye{0.0f, 0.0f, 2.0f};
    Vec3  target{0.0f};
    float sceneScale{1.0f};
    float aspect{1.0f};
    float verticalFovDegrees{70.0f};
    float nearPlane{0.001f};
    float farPlane{100.0f};
    bool  orthographic{false};
};

// Reads platform input; callers decide whether their window/UI owns navigation.
CameraInput ReadNavigationCameraInput(float seconds, float moveSpeed, float wheel);

bool ValidateNavigationCameraState(const NavigationCameraState& state);

class NavigationCamera
{
public:
    NavigationCamera();

    NavigationCameraState GetState() const;

    // Invalid input leaves the current camera unchanged.
    bool SetState(const NavigationCameraState& state);

    // Adopt a pose written by scene animation without republishing or changing its projection.
    void SynchronizeFromCamera();

    void Frame(const sg::AABB& bounds);

    void Apply(const CameraInput& input);

    // Orbits around the current target by angles in radians, in the same directions as
    // an orbit drag: +x matches dragging right and +y dragging down.
    void OrbitBy(Vec2 radians);

    // Orbits around the current target to look along direction, keeping the distance.
    // Straight down or up views stay within the pitch limit, with X to the right.
    void LookAlong(Vec3 direction);

    void SetExtent(uint32_t width, uint32_t height);

    void SetOrthographic(bool enabled);

    bool IsOrthographic() const;

    sg::Camera& GetCamera();

    const sg::Camera& GetCamera() const;

    uint64_t GetRevision() const;

    void MakeRay(Vec2 normalized, Vec3& origin, Vec3& direction) const;

private:
    void Publish();

    Vec3 Forward() const;

    sg::Camera m_camera;
    Vec3       m_eye{0.0f, 0.0f, 2.0f};
    Vec3       m_target{0.0f};
    float      m_yaw{-glm::half_pi<float>()};
    float      m_pitch{0.0f};
    float      m_distance{2.0f};
    float      m_scale{1.0f};
    uint64_t   m_revision{1};
};
} // namespace zen::runtime
