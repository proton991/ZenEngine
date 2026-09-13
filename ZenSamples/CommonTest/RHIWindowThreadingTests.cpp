#if defined(ZEN_WIN32)
#    include "Graphics/RHI/RHIThread.h"
#    include "Platform/GlfwWindow.h"
#    include <Windows.h>
#    define GLFW_EXPOSE_NATIVE_WIN32
#    include <GLFW/glfw3native.h>
#    include <gtest/gtest.h>

namespace
{
using namespace zen;

void RHIWindowTestFence() {}

struct RHIResizeObserver
{
    explicit RHIResizeObserver(RHIThread& worker) : thread(worker) {}

    void OnResize(uint32_t newWidth, uint32_t newHeight)
    {
        ++calls;
        duringWait |= waiting;
        width  = newWidth;
        height = newHeight;
        // Application resize handlers can synchronously recreate their viewport.
        // This would re-enter the RHI wait if called from its sent window message.
        thread.Invoke(&RHIWindowTestFence);
    }

    RHIThread& thread;
    uint32_t calls{0};
    uint32_t width{0};
    uint32_t height{0};
    bool waiting{false};
    bool duringWait{false};
};

uint32_t SendRHIResizeMessages(HWND window)
{
    uint32_t delivered = 0;
    for (const LPARAM size : {MAKELPARAM(120, 100), MAKELPARAM(140, 110)})
    {
        DWORD_PTR reply = 0;
        if (SendMessageTimeoutW(window, WM_SIZE, SIZE_RESTORED, size, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                2000, &reply) != 0)
        {
            ++delivered;
        }
    }
    return delivered;
}
} // namespace

TEST(RHIWindowThreadingTest, SentResizesDeferAndCoalesceCallbacksUntilWindowUpdate)
{
    ASSERT_TRUE(glfwInit());
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    platform::WindowConfig config;
    config.width     = 96;
    config.height    = 80;
    config.resizable = true;
    platform::GlfwWindowImpl window(config);
    window.Update();

    RHIThread thread;
    thread.Start(RHIExecutionMode::eThreaded);
    RHIResizeObserver observer(thread);
    window.SetOnResize(std::bind_front(&RHIResizeObserver::OnResize, &observer));
    observer.waiting = true;
    const uint32_t delivered =
        thread.Invoke(&SendRHIResizeMessages, glfwGetWin32Window(window.GetHandle()));
    observer.waiting = false;
    EXPECT_EQ(delivered, 2u);
    EXPECT_EQ(observer.calls, 0u);
    EXPECT_EQ(window.GetExtent2D().width, 140u);
    EXPECT_EQ(window.GetExtent2D().height, 110u);

    window.Update();
    EXPECT_EQ(observer.calls, 1u);
    EXPECT_FALSE(observer.duringWait);
    EXPECT_EQ(observer.width, 140u);
    EXPECT_EQ(observer.height, 110u);
    window.Update();
    EXPECT_EQ(observer.calls, 1u);
    window.SetOnResize({});
    thread.Stop();
}
#endif
