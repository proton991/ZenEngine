#pragma once
#include "Common/Math.h"
#include "Common/UniquePtr.h"
#include "Graphics/Types/Frustum.h"
#include "SceneGraph/AABB.h"
#include <functional>

namespace zen::sg
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
    Mat4 proj{1.0f};
    Mat4 view{1.0f};
};

enum class CameraType : uint32_t
{
    eFirstPerson = 0,
    eOrbit       = 1
};

enum class CameraProjectionType : uint32_t
{
    ePerspective  = 0,
    eOrthographic = 1
};
// todo: fix bug in orthographic camera
class Camera
{
public:
    static UniquePtr<Camera> CreateUniqueOnAABB(const Vec3& minPos,
                                                const Vec3& maxPos,
                                                float aspect,
                                                CameraType type = CameraType::eFirstPerson);

    static UniquePtr<Camera> CreateOrthoOnAABB(const sg::AABB& aabb);

    static UniquePtr<Camera> CreateUnique(
        const Vec3& eye,
        const Vec3& target,
        float aspect,
        CameraType type                     = CameraType::eFirstPerson,
        CameraProjectionType projectionType = CameraProjectionType::ePerspective);

    Camera(const Vec3& eye,
           const Vec3& target,
           float aspect,
           float fov       = 70.0f,
           float near      = 0.001f,
           float far       = 100.0f,
           float speed     = 2.0f,
           CameraType type = CameraType::eFirstPerson,
           // use perspective projection by default
           CameraProjectionType projectionType = CameraProjectionType::ePerspective);

    void SetPosition(const Vec3& position);

    void SetProjectionType(CameraProjectionType type)
    {
        m_projectionType = type;
    }

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

    void SetOrthoRect(const Vec4& rect);

    void SetupOnAABB(const AABB& aabb);

    void SetOnUpdate(std::function<void()> updateFunc)
    {
        m_onUpdate = std::move(updateFunc);
    }

    const uint8_t* GetUniformData() const
    {
        return reinterpret_cast<const uint8_t*>(&m_cameraData);
    }

    const Frustum& GetFrustum() const
    {
        return m_frustum;
    }

private:
    void SetProjectionMatrix();
    void UpdateBaseVectors();
    void UpdatePosition(float velocity);
    void UpdateViewFirstPerson();
    void UpdateViewOrbit(const Vec3& rotation);
    // camera attributes
    CameraType m_type;
    CameraProjectionType m_projectionType{CameraProjectionType::ePerspective};

    Vec3 m_rotation{0.0f, 0.0f, 0.0f};
    float m_rotationSpeed{1.0f};

    Vec3 m_position{0.0f, 0.0f, 0.0f};
    Vec3 m_front{directions::DEFAULT_FRONT};
    Vec3 m_up{directions::DEFAULT_UP};
    Vec3 m_right{directions::DEFAULT_RIGHT};

    const Vec3 m_worldUp{directions::DEFAULT_UP};

    bool m_flipY{false};

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

    Vec4 m_orthoRect;

    Mat4 m_projMatrix{1.f};
    Mat4 m_viewMatrix{1.f};

    float m_speed{1.f};
    float m_sensitivity{0.2f};

    CameraUniformData m_cameraData;
    std::function<void()> m_onUpdate;

    Frustum m_frustum;
};
} // namespace zen::sg