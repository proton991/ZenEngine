#include "SceneGraph/Camera.h"
#include "Platform/InputController.h"
#include "Utils/Errors.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

using namespace zen::platform;
namespace zen::sg
{
UniquePtr<Camera> Camera::CreateUniqueOnAABB(const Vec3& minPos,
                                             const Vec3& maxPos,
                                             float aspect,
                                             CameraType type)
{
    const Vec3 diag         = maxPos - minPos;
    const float maxDistance = glm::length(diag);
    float near              = 0.001f * maxDistance;
    float far               = 100.f * maxDistance;
    float fov               = 70.0f;
    const Vec3 center       = 0.5f * (maxPos + minPos);
    const Vec3 up           = Vec3(0, 1, 0);
    //  const auto eye    = diag.z > 0 ? center + 1.5f * diag : center + 2.f * glm::cross(diag, up);
    // place camera at the bbx corner
    const Vec3 eye    = Vec3(maxPos.x, maxPos.y + 0.5f * maxDistance, maxPos.z);
    const float speed = maxDistance;
    return MakeUnique<Camera>(eye, center, aspect, fov, near, far, speed, type);
}

UniquePtr<Camera> Camera::CreateOrthoOnAABB(const sg::AABB& aabb)
{
    const float radius = aabb.GetScale() * 0.5f;
    Vec3 target        = aabb.GetCenter();
    float near         = -radius;
    float far          = 2.0f * radius;
    float fov          = 70.0f;

    Vec3 direction = aabb.GetMax() - target;
    const Vec3 eye = target + direction * radius;

    Camera* pCamera = new Camera(eye, target, 1.0f, fov, near, far, radius,
                                 CameraType::eFirstPerson, CameraProjectionType::eOrthographic);
    pCamera->SetOrthoRect(Vec4(-radius, radius, -radius, radius));
    return UniquePtr<Camera>(pCamera);
}

UniquePtr<Camera> Camera::CreateUnique(const Vec3& eye,
                                       const Vec3& target,
                                       float aspect,
                                       CameraType type,
                                       CameraProjectionType projectionType)
{
    float fov   = 70.0f;
    float near  = 0.001f;
    float far   = 100.0f;
    float speed = 2.0f;
    return MakeUnique<Camera>(eye, target, aspect, fov, near, far, speed, type, projectionType);
}

Camera::Camera(const Vec3& eye,
               const Vec3& target,
               float aspect,
               float fov,
               float near,
               float far,
               float speed,
               CameraType type,
               CameraProjectionType projectionType) :
    m_type{type},
    m_projectionType(projectionType),
    m_position{eye},
    m_aspect{aspect},
    m_fov{fov},
    m_near{near},
    m_far{far},
    m_speed(speed)
{
    SetViewDirection(target - m_position);

    m_target = target;

    UpdateBaseVectors();

    SetProjectionMatrix();

    m_viewMatrix = glm::lookAt(m_position, m_position + m_front, m_up);

    m_cameraData.projViewMatrix = GetProjectionMatrix() * GetViewMatrix();

    m_cameraData.proj = GetProjectionMatrix();

    m_cameraData.view = GetViewMatrix();

    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

void Camera::SetPosition(const Vec3& position)
{
    m_position = position;

    SetViewDirection(m_target - m_position);

    UpdateBaseVectors();

    SetProjectionMatrix();

    m_viewMatrix = glm::lookAt(m_position, m_position + m_front, m_up);

    m_cameraData.projViewMatrix = GetProjectionMatrix() * GetViewMatrix();

    m_cameraData.proj = GetProjectionMatrix();

    m_cameraData.view = GetViewMatrix();

    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

void Camera::SetViewDirection(const Vec3& direction)
{
    const float lengthSquared = glm::dot(direction, direction);

    if (std::isfinite(lengthSquared) && lengthSquared > 1e-20f)
    {
        const Vec3 front = glm::normalize(direction);

        const float horizontalLength = glm::length(Vec2(front.x, front.z));

        m_pitch = glm::degrees(std::atan2(front.y, horizontalLength));

        if (horizontalLength > 1e-10f)
        {
            m_yaw = glm::degrees(std::atan2(front.z, front.x));
        }
    }
}

void Camera::UpdateBaseVectors()
{
    const Vec3 yawFront(glm::cos(glm::radians(m_yaw)), 0.0f, glm::sin(glm::radians(m_yaw)));

    m_front = glm::normalize(glm::cos(glm::radians(m_pitch)) * yawFront +
                             glm::sin(glm::radians(m_pitch)) * m_worldUp);

    // The yaw plane keeps strafe defined even when the view points straight up/down.
    const Vec3 levelRight = glm::normalize(glm::cross(yawFront, m_worldUp));

    const Vec3 levelUp = glm::normalize(glm::cross(levelRight, m_front));

    m_right = glm::normalize(glm::cos(glm::radians(m_roll)) * levelRight +
                             glm::sin(glm::radians(m_roll)) * levelUp);

    m_up = glm::normalize(glm::cross(m_right, m_front));
}

void Camera::SetProjectionMatrix()
{
    assert(glm::abs(m_aspect - std::numeric_limits<float>::epsilon()) > 0.f);
    if (m_projectionType == CameraProjectionType::ePerspective)
    {
        m_projMatrix = glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
        if (m_infiniteFar)
        {
            // Right-handed, zero-to-one depth with an infinite far plane.
            m_projMatrix[2][2] = -1.0f;
            m_projMatrix[3][2] = -m_near;
        }
    }
    else
    {
        m_projMatrix =
            glm::ortho(m_orthoRect.x, m_orthoRect.y, m_orthoRect.z, m_orthoRect.w, m_near, m_far);
    }
    m_projMatrix[1][1] *= -1;
}

void Camera::UpdatePosition(float velocity)
{
    if (KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_UP))
    {
        m_speed *= 2;
    }
    if (KeyboardMouseInput::GetInstance().WasKeyPressedOnce(GLFW_KEY_DOWN))
    {
        m_speed /= 2;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_SPACE))
    {
        // reset position
        m_position = {0.0f, 0.0f, 0.0f};
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_W))
    {
        m_position += m_front * velocity;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_S))
    {
        m_position -= m_front * velocity;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_A))
    {
        m_position -= m_right * velocity;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_D))
    {
        m_position += m_right * velocity;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_LEFT_SHIFT))
    {
        m_position += m_worldUp * velocity;
    }
    if (KeyboardMouseInput::GetInstance().IsKeyPressed(GLFW_KEY_LEFT_CONTROL))
    {
        m_position -= m_worldUp * velocity;
    }
}

void Camera::UpdateViewFirstPerson(float velocity)
{
    const std::array<float, 2> delta =
        KeyboardMouseInput::GetInstance().CalculateCursorPositionDelta();

    if (delta[0] != 0.0f || delta[1] != 0.0f)
    {
        m_yaw += delta[0] * m_sensitivity;

        if (delta[1] != 0.0f)
        {
            // Authored vertical views may start beyond the interactive limits.
            // Let them move inward smoothly without snapping on outward input.
            const float pitchMin = std::min(m_pitchMin, m_pitch);

            const float pitchMax = std::max(m_pitchMax, m_pitch);

            m_pitch = std::clamp(m_pitch + delta[1] * m_sensitivity, pitchMin, pitchMax);
        }

        UpdateBaseVectors();
    }

    // Movement uses the current mouse orientation; keyboard-only input preserves
    // the existing basis instead of rebuilding or clamping an imported camera.
    UpdatePosition(velocity);

    m_viewMatrix = glm::lookAt(m_position, m_position + m_front, m_up);
}

void Camera::UpdateViewOrbit(const Vec3& rotation)
{
    m_rotation += rotation;
    glm::mat4 rotM{1.0f};
    rotM = glm::rotate(rotM, glm::radians(m_rotation.x), Vec3(1.0f, 0.0f, 0.0f));
    rotM = glm::rotate(rotM, glm::radians(m_rotation.y * (m_flipY ? -1.0f : 1.0f)),
                       Vec3(0.0f, 1.0f, 0.0f));
    rotM = glm::rotate(rotM, glm::radians(m_rotation.z), Vec3(0.0f, 0.0f, 1.0f));

    glm::vec3 translation = m_position;
    if (m_flipY)
    {
        translation.y *= -1.0f;
    }
    glm::mat4 transM = glm::translate(glm::mat4(1.0f), translation);

    m_cameraData.view = transM * rotM;
}

void Camera::Update(float deltaTime)
{
    if (m_type == CameraType::eFirstPerson)
    {
        if (KeyboardMouseInput::GetInstance().IsDirty())
        {
            const float velocity = m_speed * deltaTime;

            UpdateViewFirstPerson(velocity);

            m_cameraData.view = GetViewMatrix();

            m_cameraData.projViewMatrix = GetProjectionMatrix() * m_cameraData.view;

            m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);

            if (m_onUpdate)
            {
                m_onUpdate();
            }
        }
    }
    else
    {
        if (!KeyboardMouseInput::GetInstance().IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT))
        {
            const std::array<float, 2> delta =
                KeyboardMouseInput::GetInstance().CalculateCursorPositionDelta();
            UpdateViewOrbit({delta[1] * m_rotationSpeed, delta[0] * m_rotationSpeed, 0.0f});
            m_cameraData.projViewMatrix = GetProjectionMatrix() * m_cameraData.view;
            m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
            if (m_onUpdate)
            {
                m_onUpdate();
            }
        }
    }
}

void Camera::UpdateAspect(float aspect)
{
    if (!m_fixedAspect && aspect > 0.0f)
    {
        m_aspect = aspect;
    }
    SetProjectionMatrix();
    // Resize must publish projection changes even when input-driven Update is idle.
    m_cameraData.proj           = m_projMatrix;
    m_cameraData.projViewMatrix = m_projMatrix * m_cameraData.view;
    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

void Camera::SetFarPlane(float far)
{
    m_far = far;
    SetProjectionMatrix();
}

void Camera::SetNearPlane(float near)
{
    m_near = near;
    SetProjectionMatrix();
}

void Camera::SetOrthoRect(const Vec4& rect)
{
    if (m_orthoRect != rect)
    {
        m_orthoRect = rect;
        SetProjectionMatrix();
    }
}

void Camera::SetupOnAABB(const AABB& aabb)
{
    const Vec3 sceneMax    = aabb.GetMax();
    const Vec3 sceneCenter = aabb.GetCenter();
    const float radius     = std::max(aabb.GetScale() * 0.5f, 0.01f);
    if (m_projectionType == CameraProjectionType::eOrthographic)
    {
        Vec4 rect = Vec4(-radius, radius, -radius, radius);
        if (m_orthoRect != rect)
        {
            m_orthoRect = rect;
        }
        m_near = -2.0f * radius;
        m_far  = 5.0f * radius;
    }
    m_target = sceneCenter;
    // A flat model must be viewed from outside its plane; a point-sized model
    // still needs a nonzero eye-to-target distance.
    const Vec3 direction = glm::normalize(glm::max(sceneMax - sceneCenter, Vec3(radius * 0.5f)));
    const Vec3 eye       = sceneCenter + direction * radius * 2.0f;

    SetPosition(eye);
}

void Camera::SetupFromSceneCamera(const SceneCamera& camera, float viewportAspect)
{
    m_type = CameraType::eFirstPerson;

    m_projectionType = camera.orthographic ? CameraProjectionType::eOrthographic :
                                             CameraProjectionType::ePerspective;

    m_position = Vec3(camera.worldMatrix[3]);

    const Vec3 authoredFront = -Vec3(camera.worldMatrix[2]);

    m_front = glm::dot(authoredFront, authoredFront) > 1e-20f ? glm::normalize(authoredFront) :
                                                                Vec3(0, 0, -1);

    const Vec3 authoredUp = Vec3(camera.worldMatrix[1]);

    const Vec3 right = glm::cross(m_front, authoredUp);

    const Vec3 fallbackUp = std::abs(m_front.y) < 0.99f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);

    m_right = glm::dot(right, right) > 1e-20f ? glm::normalize(right) :
                                                glm::normalize(glm::cross(m_front, fallbackUp));

    m_up = glm::normalize(glm::cross(m_right, m_front));

    // At a pole, front has no horizontal heading. The authored right axis
    // supplies a stable yaw plane before extracting pitch and camera roll.
    m_yaw = glm::degrees(std::atan2(-m_right.x, m_right.z));

    SetViewDirection(m_front);

    const Vec3 yawFront(glm::cos(glm::radians(m_yaw)), 0.0f, glm::sin(glm::radians(m_yaw)));

    const Vec3 levelRight = glm::normalize(glm::cross(yawFront, m_worldUp));

    const Vec3 levelUp = glm::normalize(glm::cross(levelRight, m_front));

    m_roll = glm::degrees(std::atan2(glm::dot(m_right, levelUp), glm::dot(m_right, levelRight)));

    m_target = m_position + m_front;

    m_fixedAspect = camera.fixedAspect;

    m_aspect = camera.fixedAspect ? camera.aspect : viewportAspect;

    m_fov = glm::degrees(camera.verticalFov);

    m_near = camera.nearPlane;

    m_far = camera.farPlane;

    m_infiniteFar = camera.infiniteFar;

    m_orthoRect = Vec4(-camera.xmag, camera.xmag, -camera.ymag, camera.ymag);

    SetProjectionMatrix();

    m_viewMatrix = glm::lookAt(m_position, m_target, m_up);

    m_cameraData = {m_projMatrix * m_viewMatrix, m_projMatrix, m_viewMatrix};

    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

Mat4 Camera::GetViewMatrix() const
{
    return m_viewMatrix;
}

Mat4 Camera::GetProjectionMatrix() const
{
    return m_projMatrix;
}
} // namespace zen::sg
