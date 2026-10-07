#pragma once
#include "ObjectBase.h"
#include "InputTypes.h"
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace zen::platform
{
struct WindowConfig
{
    std::string title{"ZenEngine"};
    bool        resizable{false};
    uint32_t    width{1280};
    uint32_t    height{720};
    float       aspect{0.0f};
    bool        visible{true};
};

struct WindowExtent
{
    uint32_t width{0};
    uint32_t height{0};
};

struct WindowPosition
{
    int x{0};
    int y{0};
};

// All geometry is in window coordinates, never framebuffer pixels.
struct WindowTitleBarRegion
{
    float menuEnd{0};
    float height{0};
    float controlsWidth{0};
    bool  inputBlocked{false};
};

// How an integrated title bar shares its row with the platform, in window coordinates.
// On Windows the application draws the window controls; macOS keeps its native
// buttons at the leading edge.
struct WindowTitleBarLayout
{
    // The native title-bar height, or 0 when the application chooses the row height.
    float height{0};
    float leadingInset{0};
    bool  drawsControls{false};
};

struct FileDialogFilter
{
    std::string name;
    // Semicolon-separated extensions without dots, for example "gltf;glb".
    std::string pattern;
};

enum class WindowHit
{
    Client,
    Caption,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight
};

class WindowBackend;

// Owner-thread window API. Desktop services are acquired by the first window and
// released after the last one. Creating a headless RenderDevice does not start video.
class NativeWindow
{
public:
    explicit NativeWindow(const WindowConfig& config);

    ZEN_NO_COPY_MOVE(NativeWindow)

    ~NativeWindow();

    void CheckThreadOwnership() const;

    // Polls the application event queue once and dispatches deferred resize callbacks.
    void Update(bool processInputShortcuts = true);

    static void PollEvents();

    static void WaitEvents(double seconds);

    WindowExtent GetExtent2D() const;

    WindowExtent GetFramebufferExtent() const;

    float GetAspect() const;

    // Pixel density and UI coordinate scale differ on Retina displays.
    float GetDisplayScale() const;

    float GetUIScale() const;

    bool ShouldClose() const;

    bool IsFocused() const;

    bool IsMinimized() const;

    bool IsMaximized() const;

    bool IsDecorated() const;

    void RequestClose();

    void Show();

    void Hide();

    void Focus();

    void Minimize();

    void Maximize();

    void Restore();

    void SetSize(uint32_t width, uint32_t height);

    void SetMinimumSize(uint32_t width, uint32_t height);

    WindowPosition GetPosition() const;

    void SetPosition(WindowPosition position);

    void ShowCursor() const;

    void HideCursor() const;

    void SetOnResize(std::function<void(uint32_t, uint32_t)> callback);

    // Text/composition/drop strings are owned by the event, valid during the callback.
    void SetOnInput(std::function<void(const InputEvent&)> callback);

    void SetTextInputEnabled(bool enabled);

    void SetTextInputArea(int x, int y, int width, int height, int cursorOffset);

    // Extends the client area into the title bar. Windows removes the native frame;
    // macOS keeps it, with a transparent title bar over full-size content.
    bool SetCustomFrame(bool enabled);

    WindowTitleBarLayout GetTitleBarLayout() const;

    void SetTitleBarRegion(const WindowTitleBarRegion& region);

    WindowHit HitTest(float x, float y) const;

    static bool SupportsFileDialogs();

    // Opens the platform file picker without blocking the frame loop. Returns false when
    // the backend has no picker or a picker is already open.
    bool ShowOpenFileDialog(const FileDialogFilter& filter, const std::string& defaultFolder);

    // Returns true once per finished picker; an empty path means it was cancelled.
    bool TakeFileDialogResult(std::string& path);

private:
    friend class WindowBackend;

    void*                                   m_handle{nullptr};
    void*                                   m_frameData{nullptr};
    const std::thread::id                   m_ownerThread{std::this_thread::get_id()};
    WindowTitleBarRegion                    m_titleBar;
    std::function<void(uint32_t, uint32_t)> m_onResize;
    std::function<void(const InputEvent&)>  m_onInput;
    void                                    (*m_rawObserver)(void*, const void*){nullptr};
    void*                                   m_observerContext{nullptr};
    bool                                    m_shouldClose{false};
    bool                                    m_resizePending{false};
    mutable bool                            m_cursorVisible{true};
    bool                                    m_customFrame{false};
    bool                                    m_resizable{false};
    double                                  m_cursorX{0};
    double                                  m_cursorY{0};
};
} // namespace zen::platform
