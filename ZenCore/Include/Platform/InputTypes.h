#pragma once
#include <cstdint>
#include <string>

namespace zen::platform
{
// Physical keyboard positions; text and logical symbols are delivered separately.
enum class Key : uint16_t
{
    Unknown,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Digit0,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Digit7,
    Digit8,
    Digit9,
    Space,
    Apostrophe,
    Comma,
    Minus,
    Period,
    Slash,
    Semicolon,
    Equal,
    LeftBracket,
    Backslash,
    RightBracket,
    GraveAccent,
    Escape,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right,
    Left,
    Down,
    Up,
    PageUp,
    PageDown,
    Home,
    End,
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    Keypad0,
    Keypad1,
    Keypad2,
    Keypad3,
    Keypad4,
    Keypad5,
    Keypad6,
    Keypad7,
    Keypad8,
    Keypad9,
    KeypadDecimal,
    KeypadDivide,
    KeypadMultiply,
    KeypadSubtract,
    KeypadAdd,
    KeypadEnter,
    KeypadEqual,
    LeftShift,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift,
    RightControl,
    RightAlt,
    RightSuper,
    Menu,
    Count
};

enum class MouseButton : uint8_t
{
    Left,
    Right,
    Middle,
    Extra1,
    Extra2,
    Extra3,
    Extra4,
    Extra5,
    Count
};

enum class InputEventType
{
    KeyDown,
    KeyUp,
    PointerMove,
    ButtonDown,
    ButtonUp,
    Wheel,
    Text,
    Composition,
    FileDrop,
    FocusLost,
    FocusGained
};

enum class KeyModifier : uint16_t
{
    None     = 0,
    Shift    = 1,
    Control  = 2,
    Alt      = 4,
    Super    = 8,
    CapsLock = 16,
    NumLock  = 32
};

struct InputEvent
{
    InputEventType type{InputEventType::PointerMove};
    Key            key{Key::Unknown};
    // Printable Unicode symbol when supplied by the backend; use Text for text entry.
    char32_t    symbol{0};
    KeyModifier modifiers{KeyModifier::None};
    MouseButton button{MouseButton::Left};
    bool        repeat{false};
    float       x{0};
    float       y{0};
    float       deltaX{0};
    float       deltaY{0};
    std::string text;
    int         compositionStart{0};
    int         compositionLength{0};
};
} // namespace zen::platform
