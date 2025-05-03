#include "Systems/Camera.h"
#include "Platform/InputController.h"
#include "Common/Errors.h"
#include <glm/gtc/matrix_transform.hpp>

using namespace zen::platform;
namespace zen::sys
{
UniquePtr<Camera> Camera::CreateUniqueOnAABB(const Vec3& minPos,
                                             const Vec3& maxPos,
                                             float aspect,
                                             CameraType type)
{
    const auto diag   = maxPos - minPos;
    auto maxDistance  = glm::length(diag);
    float near        = 0.001f * maxDistance;
    float far         = 100.f * maxDistance;
    float fov         = 70.0f;
    const auto center = 0.5f * (maxPos + minPos);
    const auto up     = Vec3(0, 1, 0);
    //  const auto eye    = diag.z > 0 ? center + 1.5f * diag : center + 2.f * glm::cross(diag, up);
    // place camera at the bbx corner
    const auto eye   = Vec3(maxPos.x, maxPos.y + 0.5f * maxDistance, maxPos.z);
    const auto speed = maxDistance;
    return MakeUnique<Camera>(eye, center, aspect, fov, near, far, speed, type);
}

UniquePtr<Camera> Camera::CreateUnique(const Vec3& eye,
                                       const Vec3& target,
                                       float aspect,
                                       CameraType type)
{
    float fov   = 70.0f;
    float near  = 0.001f;
    float far   = 100.0f;
    float speed = 2.0f;
    return MakeUnique<Camera>(eye, target, aspect, fov, near, far, speed, type);
}

Camera::Camera(const Vec3& eye,
               const Vec3& target,
               float aspect,
               float fov,
               float near,
               float far,
               float speed,
               CameraType type) :
    m_type{type},
    m_position{eye},
    m_aspect{aspect},
    m_fov{fov},
    m_near{near},
    m_far{far},
    m_speed(speed)
{
    Vec3 direction = glm::normalize(target - m_position);

    m_pitch  = glm::degrees(asin(direction.y));
    m_yaw    = glm::degrees(atan2(direction.z, direction.x));
    m_target = target;

    UpdateBaseVectors();
    SetProjectionMatrix();

    m_cameraData.projViewMatrix = GetProjectionMatrix() * GetViewMatrix();
    m_cameraData.proj           = GetProjectionMatrix();
    m_cameraData.view           = glm::lookAt(m_position, m_position + m_front, m_up);

    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

void Camera::SetPosition(const Vec3& position)
{
    m_position     = position;
    Vec3 direction = glm::normalize(Vec3{0.0f, 0.0f, 0.0f} - m_position);

    m_pitch = glm::degrees(asin(direction.y));
    m_yaw   = glm::degrees(atan2(direction.z, direction.x));

    UpdateBaseVectors();
    SetProjectionMatrix();

    m_cameraData.projViewMatrix = GetProjectionMatrix() * GetViewMatrix();
    m_cameraData.proj           = GetProjectionMatrix();
    m_cameraData.view           = glm::lookAt(m_position, m_position + m_front, m_up);

    m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
}

void Camera::UpdateBaseVectors()
{
    m_front.x = glm::cos(glm::radians(m_yaw)) * glm::cos(glm::radians(m_pitch));
    m_front.y = glm::sin(glm::radians(m_pitch));
    m_front.z = glm::sin(glm::radians(m_yaw)) * glm::cos(glm::radians(m_pitch));
    // Calculate the new Front vector

    m_front = glm::normalize(m_front);
    m_right = glm::normalize(glm::cross(m_front, m_worldUp));
    m_up    = glm::normalize(glm::cross(m_right, m_front));
}

void Camera::SetProjectionMatrix()
{
    assert(glm::abs(m_aspect - std::numeric_limits<float>::epsilon()) > 0.f);
    m_projMatrix = glm::perspective(glm::radians(m_fov), m_aspect, m_near, m_far);
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

void Camera::UpdateViewFirstPerson()
{
    auto delta = KeyboardMouseInput::GetInstance().CalculateCursorPositionDelta();
    m_yaw += delta[0] * m_sensitivity;
    m_pitch += delta[1] * m_sensitivity;
    m_pitch = std::clamp(m_pitch, m_pitchMin, m_pitchMax);
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
            UpdatePosition(velocity);
            UpdateViewFirstPerson();
            UpdateBaseVectors();
            m_cameraData.view           = GetViewMatrix();
            m_cameraData.projViewMatrix = GetProjectionMatrix() * m_cameraData.view;
            m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
            m_onUpdate();
        }
    }
    else
    {
        if (!KeyboardMouseInput::GetInstance().IsMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT))
        {
            auto delta = KeyboardMouseInput::GetInstance().CalculateCursorPositionDelta();
            UpdateViewOrbit({delta[1] * m_rotationSpeed, delta[0] * m_rotationSpeed, 0.0f});
            m_cameraData.projViewMatrix = GetProjectionMatrix() * m_cameraData.view;
            m_frustum.ExtractPlanes(m_cameraData.projViewMatrix);
            m_onUpdate();
        }
    }
}

void Camera::UpdateAspect(float aspect)
{
    m_aspect = aspect;
    SetProjectionMatrix();
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

Mat4 Camera::GetViewMatrix() const
{
    return glm::lookAt(m_position, m_position + m_front, m_up);
}

Mat4 Camera::GetProjectionMatrix() const
{
    return m_projMatrix;
}
} // namespace zen::sys