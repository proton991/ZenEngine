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

#include "Graphics/RHI/RHICommandListExecutor.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Platform/GlfwWindow.h"
#include <gtest/gtest.h>
#include <memory>

namespace
{
using namespace zen;

class RHIWindowSurfaceIntegrationTest : public testing::TestWithParam<RHIExecutionMode>
{
protected:
    static VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device,
                                                          const VkSwapchainCreateInfoKHR* info,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkSwapchainKHR* output)
    {
        creationThread = std::this_thread::get_id();
        oldSwapchain   = info->oldSwapchain;
        ++creations;
        return originalCreate(device, info, allocator, output);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL
    LoseSurface(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)
    {
        return VK_ERROR_SURFACE_LOST_KHR;
    }

    void SetUp() override
    {
        ASSERT_TRUE(glfwInit());
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        platform::WindowConfig config;
        config.width     = 96;
        config.height    = 80;
        config.resizable = true;
        window           = std::make_unique<platform::GlfwWindowImpl>(config);
        backend          = static_cast<VulkanRHI*>(DynamicRHI::Create(RHIAPIType::eVulkan));
        executor         = ZEN_NEW() RHICommandListExecutor(backend, GetParam());
        GDynamicRHI      = executor;
        GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, 3);
        originalCreate       = vkCreateSwapchainKHR;
        originalAcquire      = vkAcquireNextImageKHR;
        creations            = 0;
        vkCreateSwapchainKHR = &CreateSwapchain;
        ownerThread          = std::this_thread::get_id();
        rhiThread            = GetRHIThread().Invoke([] { return std::this_thread::get_id(); });
    }

    void TearDown() override
    {
        if (executor != nullptr)
        {
            executor->WaitDeviceIdle();
            if (viewport != nullptr)
            {
                executor->DestroyViewport(viewport);
            }
            vkCreateSwapchainKHR  = originalCreate;
            vkAcquireNextImageKHR = originalAcquire;
            executor->Destroy();
            ZEN_DELETE(executor);
        }
        GDynamicRHI = nullptr;
        GVulkanRHI  = nullptr;
        window.reset();
    }

    void CreateViewport(uint32_t width = 96, uint32_t height = 80)
    {
        viewport = executor->CreateViewport(window.get(), width, height, false);
    }

    static inline PFN_vkCreateSwapchainKHR originalCreate{};
    static inline std::thread::id creationThread;
    static inline VkSwapchainKHR oldSwapchain{};
    static inline uint32_t creations{0};
    PFN_vkAcquireNextImageKHR originalAcquire{};
    std::thread::id ownerThread;
    std::thread::id rhiThread;
    std::unique_ptr<platform::GlfwWindowImpl> window;
    VulkanRHI* backend{nullptr};
    RHICommandListExecutor* executor{nullptr};
    RHIViewport* viewport{nullptr};
};
} // namespace

TEST_P(RHIWindowSurfaceIntegrationTest, CreatesSurfaceOnWindowThreadAndSwapchainOnRHI)
{
    if (GetParam() == RHIExecutionMode::eThreaded)
    {
        EXPECT_NE(ownerThread, rhiThread);
    }
    CreateViewport();
    ASSERT_NE(viewport, nullptr);
    EXPECT_EQ(creations, 1u);
    EXPECT_EQ(creationThread, rhiThread);
    EXPECT_EQ(oldSwapchain, VK_NULL_HANDLE);
    viewport->Resize(112, 96);
    EXPECT_EQ(creations, 2u);
    EXPECT_EQ(creationThread, rhiThread);
    EXPECT_NE(oldSwapchain, VK_NULL_HANDLE);
}

TEST_P(RHIWindowSurfaceIntegrationTest, SurfaceLossReturnsToWindowThreadBeforeRebuilding)
{
    CreateViewport();
    ASSERT_NE(viewport, nullptr);
    vkAcquireNextImageKHR = &LoseSurface;
    GetRHIThread().Invoke(&RHIViewport::PrepareForPresent, viewport, nullptr);
    vkAcquireNextImageKHR = originalAcquire;
    ASSERT_TRUE(GetRHIThread().Invoke(&RHIViewport::NeedsRecreation, viewport));
    executor->WaitDeviceIdle();
    viewport->Resize(96, 80);
    EXPECT_EQ(creations, 2u);
    EXPECT_EQ(creationThread, rhiThread);
    EXPECT_EQ(oldSwapchain, VK_NULL_HANDLE);
    EXPECT_FALSE(GetRHIThread().Invoke(&RHIViewport::NeedsRecreation, viewport));
    EXPECT_FALSE(executor->AreSubmissionsBlocked());
}

TEST_P(RHIWindowSurfaceIntegrationTest, ZeroExtentDefersSwapchainUntilRestore)
{
    CreateViewport(0, 0);
    ASSERT_NE(viewport, nullptr);
    EXPECT_EQ(creations, 0u);
    viewport->Resize(96, 80);
    EXPECT_EQ(creations, 1u);
    EXPECT_EQ(creationThread, rhiThread);
    viewport->Resize(0, 0);
    EXPECT_EQ(creations, 1u);
    viewport->Resize(96, 80);
    EXPECT_EQ(creations, 2u);
    EXPECT_NE(oldSwapchain, VK_NULL_HANDLE);
}

TEST_P(RHIWindowSurfaceIntegrationTest, BlockedResizeKeepsViewportWithoutThrowing)
{
    CreateViewport();
    ASSERT_NE(viewport, nullptr);
    RHITexture* color     = viewport->GetColorBackBuffer();
    const uint32_t width  = viewport->GetWidth();
    const uint32_t height = viewport->GetHeight();
    GetRHIThread().Invoke(&VulkanRHI::BlockSubmissions, backend);
    EXPECT_NO_THROW(viewport->Resize(112, 96));
    EXPECT_NO_THROW(viewport->Resize(0, 0));
    EXPECT_EQ(viewport->GetColorBackBuffer(), color);
    EXPECT_EQ(viewport->GetWidth(), width);
    EXPECT_EQ(viewport->GetHeight(), height);
    EXPECT_EQ(creations, 1u);
    EXPECT_TRUE(GetRHIThread().Invoke(&VulkanRHI::AreSubmissionsBlocked, backend));
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         RHIWindowSurfaceIntegrationTest,
                         testing::Values(RHIExecutionMode::eInline, RHIExecutionMode::eThreaded));
