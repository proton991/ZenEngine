#pragma once
#include "SceneGraph/Camera.h"

namespace zen::editor
{
constexpr float kDefaultEditorCameraMoveSpeed = 1.0f;
constexpr float kMinEditorCameraMoveSpeed     = 0.001f;
constexpr float kMaxEditorCameraMoveSpeed     = 100.0f;

// Non-finite values restore the default; finite values stay within the UI's range.
float ClampEditorCameraMoveSpeed(float speed);

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
    float moveSpeed{kDefaultEditorCameraMoveSpeed};
    bool  fast{false};
};

class EditorCamera
{
public:
    EditorCamera();

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
    float      m_aspect{1.0f};
    bool       m_orthographic{false};
    uint64_t   m_revision{1};
};
} // namespace zen::editor
