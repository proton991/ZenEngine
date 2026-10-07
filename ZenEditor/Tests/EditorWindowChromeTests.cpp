#include "Editor/Platform/EditorWindowChrome.h"
#include "Platform/NativeWindow.h"
#include "NativeWindowTestAccess.h"
#include <gtest/gtest.h>

namespace zen::editor
{
namespace
{
LRESULT HitTest(HWND handle, int x, int y)
{
    POINT point{x, y};

    ClientToScreen(handle, &point);

    return SendMessageW(handle, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
}

TEST(EditorWindowChrome, MenusAndControlsStayClientWhileCaptionAndEdgesUseNativeHitTesting)
{
    platform::NativeWindow window({"Editor chrome hit testing", true, 800, 600});

    window.Hide();

    const HWND handle = GetTestWindowHandle(window);

    {
        EditorWindowChrome chrome(window);

        ASSERT_TRUE(chrome.Initialize());

        chrome.SetTitleBarRegion({300, 36, 138, false});

        // Windows provides no caption buttons in the client area; the editor draws them.
        EXPECT_TRUE(chrome.GetTitleBarLayout().drawsControls);

        EXPECT_EQ(chrome.GetTitleBarLayout().leadingInset, 0.0f);

        // SDL retains native style bits for OS integration while removing the frame.
        EXPECT_FALSE(window.IsDecorated());

        EXPECT_NE(GetWindowLongPtrW(handle, GWL_STYLE) & WS_THICKFRAME, 0);

        RECT client{};

        GetClientRect(handle, &client);

        EXPECT_EQ(client.right, 800);

        EXPECT_EQ(client.bottom, 600);

        EXPECT_EQ(HitTest(handle, 20, 18), HTCLIENT);

        EXPECT_EQ(HitTest(handle, 400, 18), HTCAPTION);

        EXPECT_EQ(HitTest(handle, 778, 18), HTCLIENT);

        EXPECT_EQ(HitTest(handle, 400, 100), HTCLIENT);

        EXPECT_EQ(HitTest(handle, 1, 1), HTTOPLEFT);

        EXPECT_EQ(HitTest(handle, 799, 599), HTBOTTOMRIGHT);

        EXPECT_EQ(HitTest(handle, 799, 1), HTTOPRIGHT);

        EXPECT_EQ(HitTest(handle, 1, 599), HTBOTTOMLEFT);

        EXPECT_EQ(HitTest(handle, 400, 1), HTTOP);

        EXPECT_EQ(HitTest(handle, 400, 599), HTBOTTOM);

        EXPECT_EQ(HitTest(handle, 1, 300), HTLEFT);

        EXPECT_EQ(HitTest(handle, 799, 300), HTRIGHT);

        chrome.SetTitleBarRegion({300, 36, 138, true});

        EXPECT_EQ(HitTest(handle, 400, 18), HTCLIENT);

        // Window size remains the full client size after decorations are removed.
        window.SetSize(1100, 700);

        GetClientRect(handle, &client);

        EXPECT_EQ(client.right, 1100);

        EXPECT_EQ(client.bottom, 700);
    }

    EXPECT_EQ(window.IsDecorated(), true);

    EXPECT_FALSE(window.GetTitleBarLayout().drawsControls);

    EXPECT_EQ(GetWindowLongPtrW(handle, GWL_STYLE) & WS_CAPTION, WS_CAPTION);
}

TEST(EditorWindowChrome, WindowActionsAndDoubleClickPreserveMaximizedWorkAreaAndRestore)
{
    platform::NativeWindow window({"Editor chrome window actions", true, 800, 600});

    EditorWindowChrome chrome(window);

    ASSERT_TRUE(chrome.Initialize());

    chrome.SetTitleBarRegion({300, 36, 138, false});

    const HWND handle = GetTestWindowHandle(window);

    chrome.RequestAction(EditorWindowAction::ToggleMaximize);

    EXPECT_FALSE(chrome.IsMaximized());

    chrome.ProcessPendingAction();

    platform::NativeWindow::PollEvents();

    EXPECT_TRUE(chrome.IsMaximized());

    MONITORINFO monitor{sizeof(MONITORINFO)};

    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor));

    RECT client{};

    GetClientRect(handle, &client);

    EXPECT_EQ(client.right, monitor.rcWork.right - monitor.rcWork.left);

    EXPECT_EQ(client.bottom, monitor.rcWork.bottom - monitor.rcWork.top);

    POINT origin{};

    ClientToScreen(handle, &origin);

    EXPECT_EQ(origin.x, monitor.rcWork.left);

    EXPECT_EQ(origin.y, monitor.rcWork.top);

    EXPECT_EQ(HitTest(handle, 1, 100), HTCLIENT);

    // A double click in the draggable strip is handled by the normal Windows procedure.
    SendMessageW(handle, WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM(origin.x + 400, origin.y + 18));

    platform::NativeWindow::PollEvents();

    EXPECT_FALSE(chrome.IsMaximized());

    chrome.RequestAction(EditorWindowAction::Minimize);

    chrome.ProcessPendingAction();

    platform::NativeWindow::PollEvents();

    EXPECT_TRUE(IsIconic(handle));

    window.Restore();

    platform::NativeWindow::PollEvents();

    EXPECT_FALSE(IsIconic(handle));

    window.Hide();

    chrome.RequestAction(EditorWindowAction::Close);

    EXPECT_FALSE(window.ShouldClose());

    chrome.ProcessPendingAction();

    EXPECT_TRUE(window.ShouldClose());
}
} // namespace
} // namespace zen::editor
