#include "Platform/InputController.h"
#include "Utils/Errors.h"

namespace zen::platform
{
void KeyboardMouseInput::PressKey(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key <= GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    // Keep press events independently of held state: glfwPollEvents can deliver both
    // press and release before the application checks its shortcuts.
    m_pendingKeyPresses[key] =
        m_pendingKeyPresses[key] || (!m_keyPressed[key] && !m_captureKeyboard);

    m_suppressedKeys[key] = m_suppressedKeys[key] || m_captureKeyboard;

    m_keyPressed[key] = true;
}

void KeyboardMouseInput::ReleaseKey(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key <= GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_keyPressed[key] = false;

    m_suppressedKeys[key] = false;
}

bool KeyboardMouseInput::IsKeyPressed(std::int32_t key) const
{
    ASSERT(key >= 0);
    ASSERT(key <= GLFW_KEY_LAST);

    std::shared_lock lock(m_inputMutex);
    return m_keyPressed[key] && !m_captureKeyboard && !m_suppressedKeys[key];
}

bool KeyboardMouseInput::WasKeyPressedOnce(std::int32_t key)
{
    ASSERT(key >= 0);
    ASSERT(key <= GLFW_KEY_LAST);

    std::scoped_lock lock(m_inputMutex);
    const bool pressed = m_pendingKeyPresses[key] && !m_captureKeyboard && !m_suppressedKeys[key];

    m_pendingKeyPresses[key] = false;

    return pressed;
}

void KeyboardMouseInput::PressMouseButton(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonPressed[button] = true;

    m_suppressedButtons[button] = m_suppressedButtons[button] || m_captureMouse;

    m_mouseButtonsUpdated = true;
}

void KeyboardMouseInput::SetMouseButtonRelease(std::int32_t button, bool released)
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonReleased[button] = released;
}

void KeyboardMouseInput::ReleaseMouseButton(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    m_mouseButtonPressed[button] = false;

    m_suppressedButtons[button] = false;

    m_mouseButtonsUpdated = true;
}

bool KeyboardMouseInput::IsMouseButtonPressed(std::int32_t button) const
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::shared_lock lock(m_inputMutex);
    return m_mouseButtonPressed[button] && !m_captureMouse && !m_suppressedButtons[button];
}

bool KeyboardMouseInput::IsMouseButtonReleased(std::int32_t button) const
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::shared_lock lock(m_inputMutex);
    return !m_mouseButtonPressed[button] || m_mouseButtonReleased[button] || m_captureMouse ||
        m_suppressedButtons[button];
}

bool KeyboardMouseInput::WasMouseButtonPressedOnce(std::int32_t button)
{
    ASSERT(button >= 0);
    ASSERT(button <= GLFW_MOUSE_BUTTON_LAST);

    std::scoped_lock lock(m_inputMutex);
    const bool pressed = m_mouseButtonPressed[button] && m_mouseButtonsUpdated && !m_captureMouse &&
        !m_suppressedButtons[button];

    if (pressed)
    {
        m_mouseButtonPressed[button] = false;
    }

    return pressed;
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
    std::array<float, 2> m_cursor_pos_delta{
        static_cast<float>(m_currentCursorPos[0]) - static_cast<float>(m_previousCursorPos[0]),
        static_cast<float>(m_previousCursorPos[1]) - static_cast<float>(m_currentCursorPos[1])};

    m_previousCursorPos = m_currentCursorPos;

    if (m_captureMouse)
    {
        m_cursor_pos_delta = {0.0f, 0.0f};
    }

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

void KeyboardMouseInput::SetUICapture(bool mouse, bool keyboard)
{
    std::scoped_lock<std::shared_mutex> lock(m_inputMutex);

    if (mouse || m_captureMouse != mouse)
    {
        m_previousCursorPos = m_currentCursorPos;
    }

    m_captureMouse = mouse;

    m_captureKeyboard = keyboard;

    if (keyboard)
    {
        m_pendingKeyPresses.fill(false);

        for (size_t index = 0; index < m_keyPressed.size(); ++index)
        {
            m_suppressedKeys[index] = m_suppressedKeys[index] || m_keyPressed[index];
        }
    }

    if (mouse)
    {
        for (size_t index = 0; index < m_mouseButtonPressed.size(); ++index)
        {
            m_suppressedButtons[index] = m_suppressedButtons[index] || m_mouseButtonPressed[index];
        }
    }
}

void KeyboardMouseInput::Reset()
{
    std::scoped_lock<std::shared_mutex> lock(m_inputMutex);

    m_keyPressed.fill(false);

    m_pendingKeyPresses.fill(false);

    m_suppressedKeys.fill(false);

    m_mouseButtonPressed.fill(false);

    m_mouseButtonReleased.fill(true);

    m_suppressedButtons.fill(false);

    m_previousCursorPos = m_currentCursorPos;

    m_firstMouse = true;

    m_mouseButtonsUpdated = false;
}
} // namespace zen::platform
