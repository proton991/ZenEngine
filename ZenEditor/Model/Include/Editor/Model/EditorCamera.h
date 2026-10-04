#pragma once
#include "SceneGraph/Camera.h"

namespace zen::editor
{
struct CameraInput
{
    Vec2  look{0.0f};
    Vec2  orbit{0.0f};
    Vec2  pan{0.0f};
    Vec3  move{0.0f};
    float dolly{0.0f};
    float seconds{0.0f};
    bool  fast{false};
};

class EditorCamera
{
public:
    EditorCamera();

    void Frame(const sg::AABB& bounds);

    void Apply(const CameraInput& input);

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
