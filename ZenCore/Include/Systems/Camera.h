#pragma once
#include "Common/Math.h"
#include "Common/UniquePtr.h"

#include <functional>

namespace zen::sys
{
namespace directions
{
/// The default value of the camera's front vector.
constexpr Vec3 DEFAULT_FRONT{0.0f, 0.0f, 1.0f};
/// The default value of the camera's right vector.
constexpr Vec3 DEFAULT_RIGHT{1.0f, 0.0f, 0.0f};
/// The default value of the camera's up vector.
constexpr Vec3 DEFAULT_UP{0.0f, 1.0f, 0.0f};
} // namespace directions

struct CameraUniformData
{
    Mat4 projViewMatrix{1.0f};
};

class Camera
{
public:
    static UniquePtr<Camera> CreateUniqueOnAABB(const Vec3& minPos,
                                                const Vec3& maxPos,
                                                float aspect);
    static UniquePtr<Camera> CreateUnique(const Vec3& eye, const Vec3& target, float aspect);

    Camera(const Vec3& eye,
           const Vec3& target,
           float aspect,
           float fov   = 70.0f,
           float near  = 0.001f,
           float far   = 100.0f,
           float speed = 2.0f);

    Mat4 GetViewMatrix() const;
    Mat4 GetProjectionMatrix() const;
    auto GetPos() const
    {
        return m_position;
    }
    void SetSpeed(float speed)
    {
        m_speed = speed;
    }

    void Update(float deltaTime);
    void UpdateAspect(float aspect);

    void SetFarPlane(float far);
    void SetNearPlane(float near);

    void SetOnUpdate(std::function<void()> updateFunc)
    {
        m_onUpdate = std::move(updateFunc);
    }

    const uint8_t* GetUniformData() const
    {
        return reinterpret_cast<const uint8_t*>(&m_cameraData);
    }

private:
    void SetProjectionMatrix();
    void UpdateBaseVectors();
    void UpdatePosition(float velocity);
    void UpdateView();
    // camera attributes
    Vec3 m_position{0.0f, 0.0f, 0.0f};
    Vec3 m_front{directions::DEFAULT_FRONT};
    Vec3 m_up{directions::DEFAULT_UP};
    Vec3 m_right{directions::DEFAULT_RIGHT};

    const Vec3 m_worldUp{directions::DEFAULT_UP};

    Vec3 m_target{};

    float m_aspect;
    float m_fov{70.0f};

    float m_yaw{0.0f};
    float m_pitch{0.0f};
    /// The camera's minimum pitch angle.
    /// Looking straight downwards is the maximum pitch angle.
    float m_pitchMin{-89.0f};
    /// The camera's maximum pitch angle.
    /// Looking straight upwards is the maximum pitch angle.
    float m_pitchMax{+89.0f};

    float m_near{0.001f};
    float m_far{100.0f};

    Mat4 m_projMatrix{1.f};
    Mat4 m_viewMatrix{1.f};

    float m_speed{1.f};
    float m_sensitivity{0.2f};

    CameraUniformData m_cameraData;
    std::function<void()> m_onUpdate;
};
} // namespace zen::sys