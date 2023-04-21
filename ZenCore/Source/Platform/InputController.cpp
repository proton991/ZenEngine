#include "Platform/InputController.h"

namespace zen::platform {

void KeyboardMouseInput::press_key(const std::int32_t key) {

  std::scoped_lock lock(m_input_mutex);
  m_key_pressed[key] = true;
  m_keyboard_updated = true;
}

void KeyboardMouseInput::release_key(const std::int32_t key) {
  std::scoped_lock lock(m_input_mutex);
  m_key_pressed[key] = false;
  m_keyboard_updated = true;
}

bool KeyboardMouseInput::is_key_pressed(const std::int32_t key) const {
  std::shared_lock lock(m_input_mutex);
  return m_key_pressed[key];
}
bool KeyboardMouseInput::was_key_pressed_once(const std::int32_t key) {
  std::scoped_lock lock(m_input_mutex);
  if (!m_key_pressed[key] || !m_keyboard_updated) {
    return false;
  }

  m_key_pressed[key] = false;
  return true;
}

void KeyboardMouseInput::press_mouse_button(const std::int32_t button) {

  std::scoped_lock lock(m_input_mutex);
  m_mouse_button_pressed[button] = true;
  m_mouse_buttons_updated        = true;
}

void KeyboardMouseInput::release_mouse_button(const std::int32_t button) {
  std::scoped_lock lock(m_input_mutex);
  m_mouse_button_pressed[button] = false;
  m_mouse_buttons_updated        = true;
}

bool KeyboardMouseInput::is_mouse_button_pressed(const std::int32_t button) const {
  std::shared_lock lock(m_input_mutex);
  return m_mouse_button_pressed[button];
}

bool KeyboardMouseInput::was_mouse_button_pressed_once(const std::int32_t button) {
  std::scoped_lock lock(m_input_mutex);
  if (!m_mouse_button_pressed[button] || !m_mouse_buttons_updated) {
    return false;
  }

  m_mouse_button_pressed[button] = false;
  return true;
}

void KeyboardMouseInput::set_cursor_pos(const double pos_x, const double pos_y) {
  std::scoped_lock lock(m_input_mutex);
  if (m_first_mouse) {
    m_previous_cursor_pos[0] = pos_x;
    m_previous_cursor_pos[1] = pos_y;
    m_first_mouse            = false;
  }
  if (!m_mouse_paused) {
    m_current_cursor_pos[0] = static_cast<std::int64_t>(pos_x);
    m_current_cursor_pos[1] = static_cast<std::int64_t>(pos_y);
  }
}

std::array<std::int64_t, 2> KeyboardMouseInput::get_cursor_pos() const {
  std::shared_lock lock(m_input_mutex);
  return m_current_cursor_pos;
}

std::array<double, 2> KeyboardMouseInput::calculate_cursor_position_delta() {
  std::scoped_lock lock(m_input_mutex);
  // Calculate the change in cursor position in x- and y-axis.
  const std::array m_cursor_pos_delta{
      static_cast<double>(m_current_cursor_pos[0]) - static_cast<double>(m_previous_cursor_pos[0]),
      static_cast<double>(m_previous_cursor_pos[1]) - static_cast<double>(m_current_cursor_pos[1])};

  m_previous_cursor_pos = m_current_cursor_pos;

  return m_cursor_pos_delta;
}

void KeyboardMouseInput::resume() {
  std::scoped_lock lock(m_input_mutex);
  m_current_cursor_pos = m_previous_cursor_pos;
  m_first_mouse        = true;
  m_mouse_paused       = false;
}

void KeyboardMouseInput::pause() {
  std::scoped_lock lock(m_input_mutex);
  m_mouse_paused = true;
}
}  // namespace zen::platform