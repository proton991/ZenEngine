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
#include "ScopedVulkanCall.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanRHI.h"
#include "Platform/GlfwWindow.h"
#include <gtest/gtest.h>
#include <memory>

namespace
{
using namespace zen;

struct SurfaceExecutionMode
{
    RHIExecutionMode execution;
    bool forceFences;
};

class RHIWindowSurfaceIntegrationTest : public testing::TestWithParam<SurfaceExecutionMode>
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

    static VKAPI_ATTR VkResult VKAPI_CALL CountSubmit(VkQueue queue,
                                                      uint32_t count,
                                                      const VkSubmitInfo* infos,
                                                      VkFence fence)
    {
        ++submitCalls;

        const VkResult result = submitCalls == failSubmitAt ?
            VK_ERROR_OUT_OF_HOST_MEMORY :
            originalSubmit(queue, count, infos, fence);

        return result;
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CountPresent(VkQueue queue, const VkPresentInfoKHR* info)
    {
        ++presentCalls;

        return originalPresent(queue, info);
    }

    static VKAPI_ATTR VkResult VKAPI_CALL CountAcquire(VkDevice device,
                                                       VkSwapchainKHR swapchain,
                                                       uint64_t timeout,
                                                       VkSemaphore semaphore,
                                                       VkFence fence,
                                                       uint32_t* image)
    {
        ++acquireCalls;

        return originalAcquire(device, swapchain, timeout, semaphore, fence, image);
    }

    static inline PFN_vkQueueSubmit originalSubmit{};
    static inline PFN_vkQueuePresentKHR originalPresent{};
    static inline uint32_t submitCalls{0};
    static inline uint32_t failSubmitAt{0};
    static inline uint32_t presentCalls{0};
    static inline uint32_t acquireCalls{0};

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

        // Switch while idle, before the executor snapshots capabilities or a viewport submits.
        // The real queue and completion code now take the fence path on timeline-capable GPUs.
        backend->WaitDeviceIdle();

        if (GetParam().forceFences)
        {
            backend->GetDevice()->GetExtensionFlags().hasTimelineSemaphore = 0;
        }

        executor    = ZEN_NEW() RHICommandListExecutor(backend, GetParam().execution);
        GDynamicRHI = executor;
        GetRHIThread().Invoke(&RHIFrameState::Init, &GRHIFrameState, 3);
        originalCreate  = vkCreateSwapchainKHR;
        originalAcquire = vkAcquireNextImageKHR;

        originalSubmit = vkQueueSubmit;

        originalPresent = vkQueuePresentKHR;

        submitCalls = failSubmitAt = presentCalls = acquireCalls = 0;

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

        // Publish the backbuffer initialization submission before snapshotting cached serials.
        executor->WaitDeviceIdle();
    }

    RHICommandListPtr RecordFrame()
    {
        RHICommandListPtr commands(
            RHICommandList::Create(executor->GetCommandContext(RHICommandContextType::eGraphics)));

        RHIRenderingLayout layout{};

        layout.SetRenderArea(0, 0, viewport->GetWidth(), viewport->GetHeight());

        layout.AddColorRenderTarget(viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(),
                                    RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);

        commands->BeginRendering(&layout);

        commands->EndRendering();

        return commands;
    }

    static inline PFN_vkCreateSwapchainKHR originalCreate{};
    static inline std::thread::id creationThread;
    static inline VkSwapchainKHR oldSwapchain{};
    static inline uint32_t creations{0};
    static inline PFN_vkAcquireNextImageKHR originalAcquire{};
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
    if (GetParam().execution == RHIExecutionMode::eThreaded)
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

TEST_P(RHIWindowSurfaceIntegrationTest, PresentationCopySharesTheLastGraphicsSubmission)
{
    CreateViewport();
    ASSERT_NE(viewport, nullptr);

    const RHICommandContextType graphics = RHICommandContextType::eGraphics;

    RHICommandListPtr commands = RecordFrame();

    const uint64_t before = executor->GetLastSubmittedSerial(graphics);

    submitCalls = 0;

    test::ScopedVulkanCall<PFN_vkQueueSubmit> counted(vkQueueSubmit, &CountSubmit);

    const RHIBatchResult result = executor->SubmitFrame(*commands, viewport).Wait();

    EXPECT_EQ(result.submission, RHISubmissionResult::eSuccess);
    EXPECT_TRUE(result.presented);
    ASSERT_EQ(result.groups.size(), 1u);
    // The copy has its own submit info and serial; the group's accepted point stays exact.
    EXPECT_GT(result.groups[0].accepted.serial, before);
    EXPECT_EQ(result.requiredSerials.Get(graphics), result.groups[0].accepted.serial + 1);

    // Without timeline semaphores, every workload is submitted with its own fence.
    const bool timeline = backend->GetDevice()->SupportsTimelineSemaphore();

    EXPECT_FALSE(GetParam().forceFences && timeline);

    EXPECT_EQ(submitCalls, timeline ? 1u : 2u);
}

TEST_P(RHIWindowSurfaceIntegrationTest, RejectedCombinedGroupKeepsAcquisitionForRetry)
{
    CreateViewport();

    ASSERT_NE(viewport, nullptr);

    const RHICommandContextType graphics = RHICommandContextType::eGraphics;

    const uint64_t before = executor->GetLastSubmittedSerial(graphics);

    test::ScopedVulkanCall<PFN_vkQueueSubmit> counted(vkQueueSubmit, &CountSubmit);

    test::ScopedVulkanCall<PFN_vkQueuePresentKHR> presented(vkQueuePresentKHR, &CountPresent);

    test::ScopedVulkanCall<PFN_vkAcquireNextImageKHR> acquired(vkAcquireNextImageKHR,
                                                               &CountAcquire);

    failSubmitAt = 1;

    RHICommandListPtr commands = RecordFrame();

    RHISubmissionGroup group{commands.get(), {}};

    const RHIBatchResult rejected = executor->SubmitGroups(MakeVecView(&group, 1), {}, viewport);

    EXPECT_EQ(rejected.submission, RHISubmissionResult::eRejected);

    EXPECT_EQ(rejected.requiredSerials.Get(graphics), before);

    EXPECT_FALSE(rejected.presented);

    EXPECT_FALSE(executor->AreSubmissionsBlocked());

    EXPECT_EQ(presentCalls, 0u);

    EXPECT_EQ(acquireCalls, 1u);

    commands = RecordFrame();

    group.commands = commands.get();

    const RHIBatchResult retried = executor->SubmitGroups(MakeVecView(&group, 1), {}, viewport);

    EXPECT_EQ(retried.submission, RHISubmissionResult::eSuccess);

    EXPECT_TRUE(retried.presented);

    EXPECT_EQ(presentCalls, 1u);

    EXPECT_EQ(acquireCalls, 1u);

    ASSERT_EQ(retried.groups.size(), 1u);

    EXPECT_EQ(retried.groups[0].accepted.serial, before + 1);

    EXPECT_EQ(retried.requiredSerials.Get(graphics), before + 2);
}

TEST_P(RHIWindowSurfaceIntegrationTest, FailedCombinedFrameRetainsItsAcceptedPrefix)
{
    CreateViewport();

    ASSERT_NE(viewport, nullptr);

    const RHICommandContextType graphics = RHICommandContextType::eGraphics;

    const bool timeline = backend->GetDevice()->SupportsTimelineSemaphore();

    const uint64_t before = executor->GetLastSubmittedSerial(graphics);

    RHITexture* color = viewport->GetColorBackBuffer();

    const uint32_t references = color->GetRefCount();

    test::ScopedVulkanCall<PFN_vkQueueSubmit> counted(vkQueueSubmit, &CountSubmit);

    test::ScopedVulkanCall<PFN_vkQueuePresentKHR> presented(vkQueuePresentKHR, &CountPresent);

    // Timeline submission rejects both infos atomically. Fence submission accepts rendering,
    // then rejects the copy. Preserve that accepted serial and block subsequent work.
    failSubmitAt = timeline ? 1 : 2;

    RHICommandListPtr commands = RecordFrame();

    const RHICommandContextType queues[] = {graphics};

    RefCountPtr<RHISubmissionState> state =
        MakeRefCountPtr<RHISubmissionState>(MakeVecView(queues, 1));

    const RHIBatchResult result = executor->SubmitFrame(*commands, viewport, state).Wait();

    EXPECT_EQ(result.submission,
              timeline ? RHISubmissionResult::eRejected : RHISubmissionResult::eFatal);

    EXPECT_FALSE(result.presented);

    EXPECT_EQ(presentCalls, 0u);

    EXPECT_EQ(submitCalls, failSubmitAt);

    ASSERT_EQ(result.groups.size(), 1u);

    const uint64_t accepted = before + (timeline ? 0 : 1);

    EXPECT_EQ(result.groups[0].accepted.serial, accepted);

    EXPECT_EQ(result.requiredSerials.Get(graphics), accepted);

    EXPECT_EQ(executor->GetLastSubmittedSerial(graphics), accepted);

    RHISubmissionDependency producer;

    EXPECT_EQ(state->Resolve(0, producer), RHISubmissionPointStatus::eFailed);

    EXPECT_TRUE(executor->AreSubmissionsBlocked());

    executor->WaitDeviceIdle();

    // Failed batches retain resources until teardown, even after accepted work has completed.
    EXPECT_GT(color->GetRefCount(), references);

    commands = RecordFrame();

    EXPECT_FALSE(executor->SubmitFrame(*commands, viewport).IsValid());

    EXPECT_EQ(submitCalls, failSubmitAt);
}

INSTANTIATE_TEST_SUITE_P(ExecutionModes,
                         RHIWindowSurfaceIntegrationTest,
                         testing::Values(SurfaceExecutionMode{RHIExecutionMode::eInline, false},
                                         SurfaceExecutionMode{RHIExecutionMode::eThreaded, false},
                                         SurfaceExecutionMode{RHIExecutionMode::eInline, true},
                                         SurfaceExecutionMode{RHIExecutionMode::eThreaded, true}));
