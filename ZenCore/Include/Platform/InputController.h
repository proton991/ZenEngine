#pragma once
#include "InputTypes.h"
#include <array>
#include <shared_mutex>
#include <mutex>

namespace zen::platform
{
class KeyboardMouseInput
{
public:
    static KeyboardMouseInput& GetInstance()
    {
        static KeyboardMouseInput input;
        return input;
    }

    KeyboardMouseInput(const KeyboardMouseInput&)            = delete;

    KeyboardMouseInput(KeyboardMouseInput&&)                 = delete;

    ~KeyboardMouseInput()                                    = default;

    KeyboardMouseInput& operator=(const KeyboardMouseInput&) = delete;

    KeyboardMouseInput& operator=(KeyboardMouseInput&&)      = delete;

    /// @brief Change the key's state to pressed.
    /// @param key the key which was pressed and greater or equal to 0
    void PressKey(Key key);

    /// @brief Change the key's state to unpressed.
    /// @param key the key which was released
    void ReleaseKey(Key key);

    /// @brief Check if the given key is currently pressed.
    /// @param key the key index
    /// @return ``true`` if the key is pressed
    [[nodiscard]] bool IsKeyPressed(Key key) const;

    /// @brief Consumes a key press, even if the key was released before this check.
    /// @param key The key index
    /// @return ``true`` if the key was pressed
    [[nodiscard]] bool WasKeyPressedOnce(Key key);

    /// @brief Change the mouse button's state to pressed.
    /// @param button the mouse button which was pressed
    void PressMouseButton(MouseButton button);

    /// @brief Change the mouse button's release state.
    /// @param button the mouse button which was released
    void SetMouseButtonRelease(MouseButton button, bool released);

    /// @brief Change the mouse button's state to unpressed.
    /// @param button the mouse button which was released
    void ReleaseMouseButton(MouseButton button);

    /// @brief Check if the given mouse button is currently pressed.
    /// @param button the mouse button index
    /// @return ``true`` if the mouse button is pressed
    [[nodiscard]] bool IsMouseButtonPressed(MouseButton button) const;

    /// @brief Check if the given mouse button is currently being held.
    /// @param button the mouse button index
    /// @return ``true`` if the mouse button is being held.
    bool IsMouseButtonReleased(MouseButton button) const;

    /// @brief Checks if a mouse button was pressed once.
    /// @param button the mouse button index
    /// @return ``true`` if the mouse button was pressed
    [[nodiscard]] bool WasMouseButtonPressedOnce(MouseButton button);

    /// @brief Set the current cursor position.
    /// @param pos_x the current x-coordinate of the cursor
    /// @param pos_y the current y-coordinate of the cursor
    void SetCursorPos(double pos_x, double pos_y);

    [[nodiscard]] std::array<std::int64_t, 2> GetCursorPos() const;

    /// @brief Calculate the change in x- and y-position of the cursor.
    /// @return a std::array of size 2 which contains the change in x-position in index 0 and the change in y-position
    /// in index 1
    [[nodiscard]] std::array<float, 2> CalculateCursorPositionDelta();

    void Resume();

    void Pause();

    // Filter application actions, never event ingestion. A captured press remains
    // suppressed until release, even when UI focus changes during the gesture.
    void SetUICapture(bool mouse, bool keyboard);

    void Reset();

    bool IsDirty() const
    {
        return m_dirty;
    }

    void SetDirty(bool flag)
    {
        m_dirty = flag;
    }

private:
    KeyboardMouseInput() = default;
    std::array<std::int64_t, 2>                               m_previousCursorPos{0, 0}; // [x, y]
    std::array<std::int64_t, 2>                               m_currentCursorPos{0, 0};  // [x, y]
    std::array<bool, static_cast<size_t>(Key::Count)>         m_keyPressed{};
    std::array<bool, static_cast<size_t>(Key::Count)>         m_pendingKeyPresses{};
    std::array<bool, static_cast<size_t>(Key::Count)>         m_suppressedKeys{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseButtonPressed{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_mouseButtonReleased{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_suppressedButtons{};
    bool                                                      m_captureMouse{false};
    bool                                                      m_captureKeyboard{false};
    bool                                                      m_mouseButtonsUpdated{false};
    bool                                                      m_firstMouse{true};
    mutable std::shared_mutex                                 m_inputMutex;
    bool                                                      m_mousePaused{false};
    bool                                                      m_dirty{false};
};
} // namespace zen::platform
