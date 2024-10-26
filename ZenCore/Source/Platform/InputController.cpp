#include "Platform/InputController.h"
#include "Common/Errors.h"

namespace zen::platform
{
void KeyboardMouseInput::PressKey(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key < GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_keyPressed[key] = true;
    m_keyboardUpdated = true;
}

void KeyboardMouseInput::ReleaseKey(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key < GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_keyPressed[key] = false;
    m_keyboardUpdated = true;
}

bool KeyboardMouseInput::IsKeyPressed(std::int32_t key) const
{
    ASSERT(key >= 0);
    ASSERT(key < GLFW_KEY_LAST);

    std::shared_lock lock(m_inputMutex);
    return m_keyPressed[key];
}
bool KeyboardMouseInput::WasKeyPressedOnce(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key < GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    if (!m_keyPressed[key] || !m_keyboardUpdated)
    {
        return false;
    }

    m_keyPressed[key] = false;
    return true;
}

void KeyboardMouseInput::PressMouseButton(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonPressed[button] = true;
    m_mouseButtonsUpdated        = true;
}

void KeyboardMouseInput::SetMouseButtonRelease(std::int32_t button, bool released)
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonReleased[button] = released;
}

void KeyboardMouseInput::ReleaseMouseButton(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonPressed[button] = false;
    m_mouseButtonsUpdated        = true;
}

bool KeyboardMouseInput::IsMouseButtonPressed(std::int32_t button) const
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::shared_lock lock(m_inputMutex);
    return m_mouseButtonPressed[button];
}

bool KeyboardMouseInput::IsMouseButtonReleased(std::int32_t button) const
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::shared_lock lock(m_inputMutex);
    return m_mouseButtonReleased[button];
}

bool KeyboardMouseInput::WasMouseButtonPressedOnce(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button < GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    if (!m_mouseButtonPressed[button] || !m_mouseButtonsUpdated)
    {
        return false;
    }

    m_mouseButtonPressed[button] = false;
    return true;
}

void KeyboardMouseInput::SetCursorPos(const double pos_x, const double pos_y)
{
    std::scoped_lock lock(m_inputMutex);
    if (m_firstMouse)
    {
        m_previousCursorPos[0] = pos_x;
        m_previousCursorPos[1] = pos_y;
        m_firstMouse           = false;
    }
    if (!m_mousePaused)
    {
        m_currentCursorPos[0] = static_cast<std::int64_t>(pos_x);
        m_currentCursorPos[1] = static_cast<std::int64_t>(pos_y);
    }
}

std::array<std::int64_t, 2> KeyboardMouseInput::GetCursorPos() const
{
    std::shared_lock lock(m_inputMutex);
    return m_currentCursorPos;
}

std::array<float, 2> KeyboardMouseInput::CalculateCursorPositionDelta()
{
    std::scoped_lock lock(m_inputMutex);
    // Calculate the change in cursor position in x- and y-axis.
    const std::array m_cursor_pos_delta{
        static_cast<float>(m_currentCursorPos[0]) - static_cast<float>(m_previousCursorPos[0]),
        static_cast<float>(m_previousCursorPos[1]) - static_cast<float>(m_currentCursorPos[1])};

    m_previousCursorPos = m_currentCursorPos;

    return m_cursor_pos_delta;
}

void KeyboardMouseInput::Resume()
{
    std::scoped_lock lock(m_inputMutex);
    m_currentCursorPos = m_previousCursorPos;
    m_firstMouse       = true;
    m_mousePaused      = false;
}

void KeyboardMouseInput::Pause()
{
    std::scoped_lock lock(m_inputMutex);
    m_mousePaused = true;
}
} // namespace zen::platform