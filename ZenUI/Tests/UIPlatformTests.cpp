#include "ImGui/UIContext.h"
#include "Platform/NativeWindow.h"
#include "Platform/WindowBackend.h"
#include "imgui.h"
#include <gtest/gtest.h>
#if defined(ZEN_WIN32)
#    include <Windows.h>
#    include <dwmapi.h>
#endif

namespace zen::ui
{
namespace
{
void BuildStaticAtlas(UIContext& context)
{
    unsigned char* pixels = nullptr;

    int width             = 0;

    int height            = 0;

    context.GetFonts().GetTexDataAsRGBA32(&pixels, &width, &height);

    context.GetFonts().SetTexID(ImTextureID(1));
}

TEST(UIPlatform, SDLEventsReachEachWindowContextOnceAndUnsubscribeOnDestruction)
{
    platform::NativeWindow first({"first UI", true, 320, 240, 0, false});

    platform::NativeWindow second({"second UI", true, 320, 240, 0, false});

    platform::NativeWindow::PollEvents();

    {
        UIContext firstUI;

        ASSERT_TRUE(firstUI.Init(first));

        BuildStaticAtlas(firstUI);

        UIContext secondUI;

        ASSERT_TRUE(secondUI.Init(second));

        BuildStaticAtlas(secondUI);

        SDL_Event event{};

        event.type          = SDL_EVENT_TEXT_INPUT;

        event.text.windowID = SDL_GetWindowID(platform::WindowBackend::Borrow(first));

        event.text.text     = "A";

        ASSERT_TRUE(SDL_PushEvent(&event));

        event.text.windowID = SDL_GetWindowID(platform::WindowBackend::Borrow(second));

        event.text.text     = "B";

        ASSERT_TRUE(SDL_PushEvent(&event));

        platform::NativeWindow::PollEvents();

        firstUI.BeginFrame(1.0f / 60.0f, 320, 240);

        ASSERT_EQ(ImGui::GetIO().InputQueueCharacters.Size, 1);

        EXPECT_EQ(ImGui::GetIO().InputQueueCharacters[0], 'A');

        firstUI.EndFrame();

        secondUI.BeginFrame(1.0f / 60.0f, 320, 240);

        ASSERT_EQ(ImGui::GetIO().InputQueueCharacters.Size, 1);

        EXPECT_EQ(ImGui::GetIO().InputQueueCharacters[0], 'B');

        secondUI.EndFrame();
    }

    // A later pump and a replacement adapter must not call a destroyed context.
    platform::NativeWindow::PollEvents();

    UIContext replacement;

    EXPECT_TRUE(replacement.Init(first));
}

#if defined(ZEN_WIN32)
TEST(UIPlatform, BorderlessWindowKeepsWorkAreaAcrossRestoreAndMaximize)
{
    platform::NativeWindow window({"UI hidden maximized startup", true, 1440, 900, 0, false});

    ASSERT_TRUE(window.SetCustomFrame(true));

    window.SetMinimumSize(800, 500);

    window.Maximize();

    UIContext context;

    ASSERT_TRUE(context.Init(window));

    window.Show();

    const HWND handle = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(platform::WindowBackend::Borrow(window)), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));

    ASSERT_NE(handle, nullptr);

    for (int cycle = 0; cycle < 4; ++cycle)
    {
        SCOPED_TRACE(cycle);

        platform::NativeWindow::PollEvents();

        ASSERT_TRUE(window.IsMaximized());

        MONITORINFO monitor{sizeof(MONITORINFO)};

        ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor));

        // SDL can report the right client size while the outer window still covers the taskbar.
        RECT frame{};

        ASSERT_EQ(DwmGetWindowAttribute(handle, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)), S_OK);

        EXPECT_EQ(frame.left, monitor.rcWork.left);

        EXPECT_EQ(frame.top, monitor.rcWork.top);

        EXPECT_EQ(frame.right, monitor.rcWork.right);

        EXPECT_EQ(frame.bottom, monitor.rcWork.bottom);

        const platform::WindowExtent maximized = window.GetFramebufferExtent();

        EXPECT_EQ(maximized.width, uint32_t(monitor.rcWork.right - monitor.rcWork.left));

        EXPECT_EQ(maximized.height, uint32_t(monitor.rcWork.bottom - monitor.rcWork.top));

        window.Restore();

        platform::NativeWindow::PollEvents();

        EXPECT_FALSE(window.IsMaximized());

        const platform::WindowExtent restored = window.GetExtent2D();

        EXPECT_EQ(restored.width, 1440u);

        EXPECT_EQ(restored.height, 900u);

        window.Maximize();
    }
}
#endif
} // namespace
} // namespace zen::ui
