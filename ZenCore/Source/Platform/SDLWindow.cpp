#include "Platform/WindowBackend.h"
#include "Utils/Errors.h"
#include <algorithm>
#include <mutex>

namespace zen::platform
{
namespace
{
Key TranslateKey(SDL_Scancode code)
{
    Key key = Key::Unknown;

    switch (code)
    {
#define ZEN_KEY(engine, sdl, glfw) \
    case sdl: key = Key::engine; break;
#include "KeyMapping.inl"
#undef ZEN_KEY
        default: break;
    }

    return key;
}

KeyModifier TranslateModifiers(SDL_Keymod value)
{
    uint16_t result = 0;

    if (value & SDL_KMOD_SHIFT)
    {
        result |= uint16_t(KeyModifier::Shift);
    }

    if (value & SDL_KMOD_CTRL)
    {
        result |= uint16_t(KeyModifier::Control);
    }

    if (value & SDL_KMOD_ALT)
    {
        result |= uint16_t(KeyModifier::Alt);
    }

    if (value & SDL_KMOD_GUI)
    {
        result |= uint16_t(KeyModifier::Super);
    }

    if (value & SDL_KMOD_CAPS)
    {
        result |= uint16_t(KeyModifier::CapsLock);
    }

    if (value & SDL_KMOD_NUM)
    {
        result |= uint16_t(KeyModifier::NumLock);
    }

    return KeyModifier(result);
}

SDL_HitTestResult SDLCALL HitTest(SDL_Window*, const SDL_Point* point, void* data)
{
    NativeWindow& window     = *static_cast<NativeWindow*>(data);

    SDL_HitTestResult result = SDL_HITTEST_NORMAL;

    switch (window.HitTest(float(point->x), float(point->y)))
    {
        case WindowHit::Caption: result = SDL_HITTEST_DRAGGABLE; break;
        case WindowHit::Left: result = SDL_HITTEST_RESIZE_LEFT; break;
        case WindowHit::Right: result = SDL_HITTEST_RESIZE_RIGHT; break;
        case WindowHit::Top: result = SDL_HITTEST_RESIZE_TOP; break;
        case WindowHit::Bottom: result = SDL_HITTEST_RESIZE_BOTTOM; break;
        case WindowHit::TopLeft: result = SDL_HITTEST_RESIZE_TOPLEFT; break;
        case WindowHit::TopRight: result = SDL_HITTEST_RESIZE_TOPRIGHT; break;
        case WindowHit::BottomLeft: result = SDL_HITTEST_RESIZE_BOTTOMLEFT; break;
        case WindowHit::BottomRight: result = SDL_HITTEST_RESIZE_BOTTOMRIGHT; break;
        case WindowHit::Client: break;
    }

    return result;
}
// SDL can finish a picker on its own thread, possibly after the window is gone,
// so the shared state lives for the process. One picker may be open at a time.
struct FileDialogState
{
    std::mutex           mutex;
    const NativeWindow*  owner{nullptr};
    bool                 open{false};
    bool                 finished{false};
    std::string          path;
    std::string          error;
    std::string          name;
    std::string          pattern;
    SDL_DialogFileFilter filter{};
};

FileDialogState& GetFileDialogState()
{
    static FileDialogState state;

    return state;
}

void SDLCALL OnFileDialog(void* userdata, const char* const* files, int)
{
    FileDialogState& state = *static_cast<FileDialogState*>(userdata);

    const std::lock_guard<std::mutex> lock(state.mutex);

    state.path     = files != nullptr && files[0] != nullptr ? files[0] : "";

    state.error    = files == nullptr ? SDL_GetError() : "";

    state.finished = state.owner != nullptr;

    state.open     = false;
}
} // namespace

SDL_Window* WindowBackend::Borrow(const NativeWindow& window)
{
    window.CheckThreadOwnership();

    return static_cast<SDL_Window*>(window.m_handle);
}

NativeWindow::NativeWindow(const WindowConfig& config) : m_resizable(config.resizable)
{
    WindowBackend::Acquire();

    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    if (config.resizable)
    {
        flags |= SDL_WINDOW_RESIZABLE;
    }

    if (!config.visible)
    {
        flags |= SDL_WINDOW_HIDDEN;
    }

    m_handle = SDL_CreateWindow(config.title.c_str(), int(config.width), int(config.height), flags);

    VERIFY_EXPR_MSG(m_handle != nullptr, SDL_GetError());

    SDL_SetWindowPosition(WindowBackend::Borrow(*this), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    WindowBackend::Register(*this);
}

NativeWindow::~NativeWindow()
{
    CheckThreadOwnership();

    ASSERT(m_rawObserver == nullptr);

    FileDialogState& dialog = GetFileDialogState();

    {
        const std::lock_guard<std::mutex> lock(dialog.mutex);

        if (dialog.owner == this)
        {
            dialog.owner    = nullptr;

            dialog.finished = false;
        }
    }

#if defined(ZEN_MACOS)
    // The title-bar event monitor refers to this window.
    if (m_customFrame)
    {
        WindowBackend::SetCocoaTitleBar(*this, false);
    }
#endif

    SDL_DestroyWindow(WindowBackend::Borrow(*this));

    WindowBackend::Unregister(*this);
}

WindowExtent NativeWindow::GetExtent2D() const
{
    int width  = 0;

    int height = 0;

    SDL_GetWindowSize(WindowBackend::Borrow(*this), &width, &height);

    return {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
}

WindowExtent NativeWindow::GetFramebufferExtent() const
{
    int width  = 0;

    int height = 0;

    if (!IsMinimized())
    {
        SDL_GetWindowSizeInPixels(WindowBackend::Borrow(*this), &width, &height);
    }

    return {uint32_t(std::max(width, 0)), uint32_t(std::max(height, 0))};
}

float NativeWindow::GetDisplayScale() const
{
    return SDL_GetWindowDisplayScale(WindowBackend::Borrow(*this));
}

float NativeWindow::GetUIScale() const
{
    const float density = SDL_GetWindowPixelDensity(WindowBackend::Borrow(*this));

    return GetDisplayScale() / std::max(density, 0.01f);
}

bool NativeWindow::IsFocused() const
{
    return (SDL_GetWindowFlags(WindowBackend::Borrow(*this)) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool NativeWindow::IsMinimized() const
{
    return (SDL_GetWindowFlags(WindowBackend::Borrow(*this)) & SDL_WINDOW_MINIMIZED) != 0;
}

bool NativeWindow::IsMaximized() const
{
    return (SDL_GetWindowFlags(WindowBackend::Borrow(*this)) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool NativeWindow::IsDecorated() const
{
    return (SDL_GetWindowFlags(WindowBackend::Borrow(*this)) & SDL_WINDOW_BORDERLESS) == 0;
}

void NativeWindow::Show()
{
    SDL_ShowWindow(WindowBackend::Borrow(*this));

    SDL_SyncWindow(WindowBackend::Borrow(*this));
}

void NativeWindow::Hide()
{
    SDL_HideWindow(WindowBackend::Borrow(*this));
}

void NativeWindow::Focus()
{
    SDL_RaiseWindow(WindowBackend::Borrow(*this));
}

void NativeWindow::Minimize()
{
    SDL_MinimizeWindow(WindowBackend::Borrow(*this));

    SDL_SyncWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Maximize()
{
    SDL_MaximizeWindow(WindowBackend::Borrow(*this));

    SDL_SyncWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::Restore()
{
    SDL_RestoreWindow(WindowBackend::Borrow(*this));

    SDL_SyncWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::SetSize(uint32_t width, uint32_t height)
{
    SDL_SetWindowSize(WindowBackend::Borrow(*this), int(width), int(height));

    SDL_SyncWindow(WindowBackend::Borrow(*this));

    WindowBackend::Resize(*this);
}

void NativeWindow::SetMinimumSize(uint32_t width, uint32_t height)
{
    SDL_SetWindowMinimumSize(WindowBackend::Borrow(*this), int(width), int(height));
}

WindowPosition NativeWindow::GetPosition() const
{
    WindowPosition position;

    SDL_GetWindowPosition(WindowBackend::Borrow(*this), &position.x, &position.y);

    return position;
}

void NativeWindow::SetPosition(WindowPosition position)
{
    SDL_SetWindowPosition(WindowBackend::Borrow(*this), position.x, position.y);
}

void NativeWindow::ShowCursor() const
{
    SDL_SetWindowRelativeMouseMode(WindowBackend::Borrow(*this), false);

    m_cursorVisible = true;
}

void NativeWindow::HideCursor() const
{
    SDL_SetWindowRelativeMouseMode(WindowBackend::Borrow(*this), true);

    m_cursorVisible = false;
}

bool NativeWindow::SetCustomFrame(bool enabled)
{
    SDL_Window* handle = WindowBackend::Borrow(*this);

    bool success       = SDL_SetWindowHitTest(handle, enabled ? &platform::HitTest : nullptr, enabled ? this : nullptr);

#if defined(ZEN_MACOS)
    // A borderless Cocoa window loses its buttons and title-bar behavior; keep the frame.
    if (success && !WindowBackend::SetCocoaTitleBar(*this, enabled))
    {
        SDL_SetWindowHitTest(handle, nullptr, nullptr);

        success = false;
    }
#else
    if (success)
    {
        SDL_SetWindowBordered(handle, !enabled);
    }
#endif

    if (success)
    {
        m_customFrame = enabled;
    }

    return success;
}

WindowTitleBarLayout NativeWindow::GetTitleBarLayout() const
{
    WindowTitleBarLayout layout;

#if defined(ZEN_MACOS)
    if (m_customFrame)
    {
        layout = WindowBackend::GetCocoaTitleBarLayout(*this);
    }
#else
    layout.drawsControls = m_customFrame;
#endif

    return layout;
}

bool NativeWindow::SupportsFileDialogs()
{
    return true;
}

bool NativeWindow::ShowOpenFileDialog(const FileDialogFilter& filter, const std::string& defaultFolder)
{
    SDL_Window* window     = WindowBackend::Borrow(*this);

    FileDialogState& state = GetFileDialogState();

    bool started           = false;

    {
        const std::lock_guard<std::mutex> lock(state.mutex);

        started = !state.open;

        if (started)
        {
            state.open     = true;

            state.finished = false;

            state.owner    = this;

            state.name     = filter.name;

            state.pattern  = filter.pattern;

            state.filter   = {state.name.c_str(), state.pattern.c_str()};
        }
    }

    // The callback may run before this call returns, so the lock is not held here.
    if (started)
    {
        SDL_ShowOpenFileDialog(&OnFileDialog, &state, window, &state.filter, 1,
                               defaultFolder.empty() ? nullptr : defaultFolder.c_str(), false);
    }

    return started;
}

bool NativeWindow::TakeFileDialogResult(std::string& path)
{
    CheckThreadOwnership();

    FileDialogState& state = GetFileDialogState();

    const std::lock_guard<std::mutex> lock(state.mutex);

    const bool finished = state.finished && state.owner == this;

    if (finished)
    {
        if (!state.error.empty())
        {
            LOGW("File dialog failed: {}", state.error);
        }

        path = std::move(state.path);

        state.path.clear();

        state.error.clear();

        state.finished = false;

        state.owner    = nullptr;
    }

    return finished;
}

void NativeWindow::SetTextInputEnabled(bool enabled)
{
    SDL_Window* window = WindowBackend::Borrow(*this);

    if (enabled)
    {
        SDL_StartTextInput(window);
    }
    else
    {
        SDL_StopTextInput(window);
    }
}

void NativeWindow::SetTextInputArea(int x, int y, int width, int height, int cursorOffset)
{
    const SDL_Rect area{x, y, width, height};

    SDL_SetTextInputArea(WindowBackend::Borrow(*this), &area, cursorOffset);
}

void WindowBackend::DispatchToWindow(NativeWindow& window, const SDL_Event& event)
{
    InputEvent input;

    bool deliver = true;

    switch (event.type)
    {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            Close(window);
            deliver = false;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            Resize(window);
            deliver = false;
            break;
#if defined(ZEN_MACOS)
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            // SDL restores its windowed style mask after a full-screen space.
            if (window.m_customFrame)
            {
                SetCocoaTitleBar(window, true);
            }
            deliver = false;
            break;
#endif
        case SDL_EVENT_WINDOW_FOCUS_LOST: input.type = InputEventType::FocusLost; break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: input.type = InputEventType::FocusGained; break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            input.type = event.type == SDL_EVENT_KEY_DOWN ? InputEventType::KeyDown : InputEventType::KeyUp;

            input.key  = TranslateKey(event.key.scancode);

            input.symbol =
                ((event.key.key >= 0x20 && event.key.key < 0x7F) || (event.key.key >= 0xA0 && event.key.key < 0x110000))
                    ? char32_t(event.key.key)
                    : 0;

            input.modifiers = TranslateModifiers(event.key.mod);

            input.repeat    = event.key.repeat;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input.type   = InputEventType::PointerMove;

            input.x      = event.motion.x;

            input.y      = event.motion.y;

            input.deltaX = event.motion.xrel;

            input.deltaY = event.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            input.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? InputEventType::ButtonDown : InputEventType::ButtonUp;

            switch (event.button.button)
            {
                case SDL_BUTTON_LEFT: input.button = MouseButton::Left; break;
                case SDL_BUTTON_RIGHT: input.button = MouseButton::Right; break;
                case SDL_BUTTON_MIDDLE: input.button = MouseButton::Middle; break;
                case SDL_BUTTON_X1: input.button = MouseButton::Extra1; break;
                case SDL_BUTTON_X2: input.button = MouseButton::Extra2; break;
                case 6: input.button = MouseButton::Extra3; break;
                case 7: input.button = MouseButton::Extra4; break;
                case 8: input.button = MouseButton::Extra5; break;
                default: deliver = false; break;
            }

            input.x = event.button.x;

            input.y = event.button.y;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input.type   = InputEventType::Wheel;

            input.deltaX = event.wheel.x;

            input.deltaY = event.wheel.y;

            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            {
                input.deltaX = -input.deltaX;

                input.deltaY = -input.deltaY;
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            input.type = InputEventType::Text;

            input.text = event.text.text;
            break;
        case SDL_EVENT_TEXT_EDITING:
            input.type              = InputEventType::Composition;

            input.text              = event.edit.text;

            input.compositionStart  = event.edit.start;

            input.compositionLength = event.edit.length;
            break;
        case SDL_EVENT_DROP_FILE:
            input.type = InputEventType::FileDrop;

            input.text = event.drop.data;
            break;
        default: deliver = false; break;
    }

    if (deliver)
    {
        Deliver(window, input);
    }
}
} // namespace zen::platform
