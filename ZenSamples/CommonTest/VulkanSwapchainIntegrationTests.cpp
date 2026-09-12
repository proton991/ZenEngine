#include "VulkanIntegrationFixture.h"
#include "ScopedVulkanCall.h"
#include "Templates/HeapVector.h"
#include "Graphics/VulkanRHI/VulkanCommandList.h"
#include "Graphics/VulkanRHI/VulkanDevice.h"
#include "Graphics/VulkanRHI/VulkanQueue.h"
#include "Graphics/VulkanRHI/VulkanSwapchain.h"
#include "Graphics/VulkanRHI/VulkanViewport.h"
#include "Graphics/VulkanRHI/VulkanSynchronization.h"
#include "Platform/GlfwWindow.h"
#include <gtest/gtest.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
using namespace zen;

// Synthetic WSI exercises negotiation and driver failures. Native mode forwards
// to the driver, optionally exposing extra image slots mapped to real images.
struct WSIDriver
{
    static inline PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities;
    static inline PFN_vkGetPhysicalDeviceSurfaceSupportKHR support;
    static inline PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats;
    static inline PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes;
    static inline PFN_vkCreateSwapchainKHR create;
    static inline PFN_vkDestroySwapchainKHR destroy;
    static inline PFN_vkGetSwapchainImagesKHR images;
    static inline PFN_vkAcquireNextImageKHR acquire;
    static inline PFN_vkQueuePresentKHR present;
    static inline PFN_vkQueueSubmit submit;
    static inline PFN_vkCreateSemaphore createSemaphore;
    static inline PFN_vkDestroySemaphore destroySemaphore;
    static inline PFN_vkCreateFence createFence;
    static inline PFN_vkDestroyFence destroyFence;
    static inline PFN_vkWaitForFences waitForFences;
    static inline HeapVector<VkSemaphore> liveSemaphores;
    static inline HeapVector<VkFence> liveFences;
    static inline uint32_t semaphoreDestructions{}, fenceCreationFailures{}, presentFenceCalls{};
    static inline uint32_t semaphoreFailureCountdown{};
    static inline VkFence heldPresentFence{};
    static inline bool holdPresentation{}, rejectPresentWait{};
    static inline bool native{}, incomplete{}, canPresent{true};
    static inline VkResult acquireResult{VK_SUCCESS}, presentResult{VK_SUCCESS},
        createResult{VK_SUCCESS};
    static inline VkSurfaceCapabilitiesKHR caps{};
    static inline HeapVector<VkPresentModeKHR> presentModes;
    static inline uint32_t count{12}, acquireIndex{11}, acquiredCalls{}, presentedCalls{},
        createdCalls{}, destroyedCalls{};
    static inline uint32_t rejectSubmissions{};
    static inline VkSwapchainCreateInfoKHR lastCreate{};
    static inline VkSemaphore lastAcquire{}, lastPresent{};
    static inline std::unordered_map<VkSwapchainKHR, uint32_t> nativeCounts, exposedCounts;
    static inline std::unordered_set<VkSemaphore> presentationSemaphores;

    static void Reset()
    {
        native = incomplete = false;
        canPresent          = true;
        acquireResult = presentResult = createResult = VK_SUCCESS;
        count                                        = 12;
        acquireIndex                                 = 11;
        acquiredCalls = presentedCalls = createdCalls = destroyedCalls = rejectSubmissions = 0;
        caps                                                                               = {};
        caps.minImageCount                                                                 = 2;
        caps.maxImageCount                                                                 = 0;
        caps.currentExtent           = {96, 80};
        caps.minImageExtent          = {32, 32};
        caps.maxImageExtent          = {512, 512};
        caps.maxImageArrayLayers     = 1;
        caps.supportedTransforms     = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
        caps.currentTransform        = VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR;
        caps.supportedCompositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
        caps.supportedUsageFlags     = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        presentModes                 = {VK_PRESENT_MODE_FIFO_KHR};
        lastCreate                   = {};
        lastAcquire = lastPresent = VK_NULL_HANDLE;
        nativeCounts.clear();
        exposedCounts.clear();
        presentationSemaphores.clear();
        liveSemaphores.clear();
        liveFences.clear();
        semaphoreDestructions = fenceCreationFailures = presentFenceCalls = 0;
        semaphoreFailureCountdown                                         = 0;
        heldPresentFence                                                  = VK_NULL_HANDLE;
        holdPresentation = rejectPresentWait = false;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Capabilities(VkPhysicalDevice gpu,
                                                       VkSurfaceKHR surface,
                                                       VkSurfaceCapabilitiesKHR* output)
    {
        if (native)
        {
            return capabilities(gpu, surface, output);
        }
        *output = caps;
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Support(VkPhysicalDevice gpu,
                                                  uint32_t family,
                                                  VkSurfaceKHR surface,
                                                  VkBool32* output)
    {
        if (native)
        {
            return support(gpu, family, surface, output);
        }
        *output = canPresent;
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Formats(VkPhysicalDevice gpu,
                                                  VkSurfaceKHR surface,
                                                  uint32_t* size,
                                                  VkSurfaceFormatKHR* output)
    {
        if (native)
        {
            return formats(gpu, surface, size, output);
        }
        *size = 1;
        if (output)
        {
            *output = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        }
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Modes(VkPhysicalDevice gpu,
                                                VkSurfaceKHR surface,
                                                uint32_t* size,
                                                VkPresentModeKHR* output)
    {
        if (native)
        {
            return modes(gpu, surface, size, output);
        }
        if (output)
        {
            std::copy(presentModes.begin(), presentModes.end(), output);
        }
        *size = static_cast<uint32_t>(presentModes.size());
        return VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice device,
                                                 const VkSwapchainCreateInfoKHR* info,
                                                 const VkAllocationCallbacks* allocator,
                                                 VkSwapchainKHR* output)
    {
        lastCreate = *info;
        ++createdCalls;
        if (createResult != VK_SUCCESS)
        {
            return createResult;
        }
        if (!native)
        {
            *output = (VkSwapchainKHR)uintptr_t(createdCalls);
            return VK_SUCCESS;
        }
        const VkResult result = create(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            uint32_t actual = 0;
            EXPECT_EQ(images(device, *output, &actual, nullptr), VK_SUCCESS);
            nativeCounts[*output]  = actual;
            exposedCounts[*output] = std::max(count, actual);
        }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice device,
                                              VkSwapchainKHR swapchain,
                                              const VkAllocationCallbacks* allocator)
    {
        ++destroyedCalls;
        if (native)
        {
            destroy(device, swapchain, allocator);
        }
        nativeCounts.erase(swapchain);
        exposedCounts.erase(swapchain);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Images(VkDevice device,
                                                 VkSwapchainKHR swapchain,
                                                 uint32_t* size,
                                                 VkImage* output)
    {
        const uint32_t exposed = native ? exposedCounts.at(swapchain) : count;
        if (!output)
        {
            *size = incomplete ? 8 : exposed;
            return VK_SUCCESS;
        }
        const uint32_t written = std::min(*size, exposed);
        if (native)
        {
            uint32_t actual = nativeCounts.at(swapchain);
            HeapVector<VkImage> real(actual);
            EXPECT_EQ(images(device, swapchain, &actual, real.data()), VK_SUCCESS);
            for (uint32_t i = 0; i < written; ++i)
            {
                output[i] = real[i < exposed - actual ? 0 : i - (exposed - actual)];
            }
        }
        else
        {
            for (uint32_t i = 0; i < written; ++i)
            {
                output[i] = (VkImage)uintptr_t(i + 1);
            }
        }
        *size      = written;
        incomplete = false;
        return written < exposed ? VK_INCOMPLETE : VK_SUCCESS;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Acquire(VkDevice device,
                                                  VkSwapchainKHR swapchain,
                                                  uint64_t timeout,
                                                  VkSemaphore semaphore,
                                                  VkFence fence,
                                                  uint32_t* output)
    {
        ++acquiredCalls;
        lastAcquire = semaphore;
        EXPECT_NE(timeout, UINT64_MAX);
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR)
        {
            return acquireResult;
        }
        if (!native)
        {
            *output = acquireIndex;
            // Synthetic WSI still completes real acquire fences; no completion
            // query is mocked into claiming an unsignaled fence has completed.
            EXPECT_EQ(
                submit(GVulkanRHI->GetDevice()->GetGfxQueue()->GetVkHandle(), 0, nullptr, fence),
                VK_SUCCESS);
            return acquireResult;
        }
        const VkResult result = acquire(device, swapchain, timeout, semaphore, fence, output);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        {
            *output += exposedCounts.at(swapchain) - nativeCounts.at(swapchain);
            return acquireResult == VK_SUBOPTIMAL_KHR ? acquireResult : result;
        }
        return result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Present(VkQueue queue, const VkPresentInfoKHR* info)
    {
        ++presentedCalls;
        const auto* fenceInfo = static_cast<const VkSwapchainPresentFenceInfoEXT*>(info->pNext);
        if (fenceInfo)
        {
            EXPECT_EQ(fenceInfo->sType, VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT);
            ++presentFenceCalls;
        }
        if (info->waitSemaphoreCount)
        {
            lastPresent = info->pWaitSemaphores[0];
            presentationSemaphores.insert(lastPresent);
        }
        if (!native)
        {
            if (fenceInfo &&
                (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR ||
                 presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
                 presentResult == VK_ERROR_SURFACE_LOST_KHR))
            {
                if (holdPresentation)
                {
                    heldPresentFence = fenceInfo->pFences[0];
                }
                else
                {
                    EXPECT_EQ(submit(queue, 0, nullptr, fenceInfo->pFences[0]), VK_SUCCESS);
                }
            }
            return presentResult;
        }
        VkPresentInfoKHR mapped = *info;
        const uint32_t index    = info->pImageIndices[0] -
            (exposedCounts.at(info->pSwapchains[0]) - nativeCounts.at(info->pSwapchains[0]));
        mapped.pImageIndices  = &index;
        const VkResult result = present(queue, &mapped);
        return presentResult == VK_SUCCESS ? result : presentResult;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue queue,
                                                 uint32_t size,
                                                 const VkSubmitInfo* info,
                                                 VkFence fence)
    {
        if (rejectSubmissions)
        {
            --rejectSubmissions;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return submit(queue, size, info, fence);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device,
                                                          const VkSemaphoreCreateInfo* info,
                                                          const VkAllocationCallbacks* allocator,
                                                          VkSemaphore* output)
    {
        if (semaphoreFailureCountdown && --semaphoreFailureCountdown == 0)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const VkResult result = createSemaphore(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            liveSemaphores.push_back(*output);
        }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice device,
                                                       VkSemaphore semaphore,
                                                       const VkAllocationCallbacks* allocator)
    {
        destroySemaphore(device, semaphore, allocator);
        presentationSemaphores.erase(semaphore);
        const auto it = std::find(liveSemaphores.begin(), liveSemaphores.end(), semaphore);
        if (it != liveSemaphores.end())
        {
            liveSemaphores.erase(it);
            ++semaphoreDestructions;
        }
    }
    static VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device,
                                                      const VkFenceCreateInfo* info,
                                                      const VkAllocationCallbacks* allocator,
                                                      VkFence* output)
    {
        if (fenceCreationFailures && --fenceCreationFailures == 0)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const VkResult result = createFence(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            liveFences.push_back(*output);
        }
        return result;
    }
    static VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice device,
                                                   VkFence fence,
                                                   const VkAllocationCallbacks* allocator)
    {
        destroyFence(device, fence, allocator);
        const auto it = std::find(liveFences.begin(), liveFences.end(), fence);
        if (it != liveFences.end())
        {
            liveFences.erase(it);
        }
    }
    static VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice device,
                                                        uint32_t count,
                                                        const VkFence* fences,
                                                        VkBool32 all,
                                                        uint64_t timeout)
    {
        if (heldPresentFence &&
            std::find(fences, fences + count, heldPresentFence) != fences + count)
        {
            EXPECT_EQ(semaphoreDestructions, 0u);
            EXPECT_EQ(destroyedCalls, 0u);
            if (rejectPresentWait)
            {
                rejectPresentWait = false;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            EXPECT_EQ(vkGetFenceStatus(device, heldPresentFence), VK_NOT_READY);
            EXPECT_EQ(submit(GVulkanRHI->GetDevice()->GetGfxQueue()->GetVkHandle(), 0, nullptr,
                             heldPresentFence),
                      VK_SUCCESS);
            heldPresentFence = VK_NULL_HANDLE;
        }
        return waitForFences(device, count, fences, all, timeout);
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL
    Validation(VkDebugUtilsMessageSeverityFlagBitsEXT,
               VkDebugUtilsMessageTypeFlagsEXT,
               const VkDebugUtilsMessengerCallbackDataEXT* data,
               void*)
    {
        ADD_FAILURE() << data->pMessage;
        return VK_FALSE;
    }
};

class VulkanSwapchainIntegrationTest : public testing::Test
{
protected:
    std::unique_ptr<test::VulkanSession> session;
    std::unique_ptr<platform::GlfwWindowImpl> window;
    std::unique_ptr<VulkanSwapchain> swapchain;
    RHIViewport* viewport{};
    RHICommandList* commands{};
    FVulkanCommandListContext* context{};
    VkDebugUtilsMessengerEXT messenger{};
    HeapVector<std::function<void()>> restore;

    template <typename T> void Hook(T& slot, T replacement, T& previous)
    {
        previous = slot;
        restore.push_back([&slot, old = slot] { slot = old; });
        slot = replacement;
    }
    void SetUp() override
    {
        session = std::make_unique<test::VulkanSession>();
        ASSERT_TRUE(glfwInit());
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        platform::WindowConfig config;
        config.width = config.height = 64;
        config.resizable             = true;
        window                       = std::make_unique<platform::GlfwWindowImpl>(config);
        WSIDriver::Reset();
        Hook(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, WSIDriver::Capabilities,
             WSIDriver::capabilities);
        Hook(vkGetPhysicalDeviceSurfaceSupportKHR, WSIDriver::Support, WSIDriver::support);
        Hook(vkGetPhysicalDeviceSurfaceFormatsKHR, WSIDriver::Formats, WSIDriver::formats);
        Hook(vkGetPhysicalDeviceSurfacePresentModesKHR, WSIDriver::Modes, WSIDriver::modes);
        Hook(vkCreateSwapchainKHR, WSIDriver::Create, WSIDriver::create);
        Hook(vkDestroySwapchainKHR, WSIDriver::Destroy, WSIDriver::destroy);
        Hook(vkGetSwapchainImagesKHR, WSIDriver::Images, WSIDriver::images);
        Hook(vkAcquireNextImageKHR, WSIDriver::Acquire, WSIDriver::acquire);
        Hook(vkQueuePresentKHR, WSIDriver::Present, WSIDriver::present);
        Hook(vkQueueSubmit, WSIDriver::Submit, WSIDriver::submit);
        Hook(vkCreateSemaphore, WSIDriver::CreateSemaphore, WSIDriver::createSemaphore);
        Hook(vkDestroySemaphore, WSIDriver::DestroySemaphore, WSIDriver::destroySemaphore);
        Hook(vkCreateFence, WSIDriver::CreateFence, WSIDriver::createFence);
        Hook(vkDestroyFence, WSIDriver::DestroyFence, WSIDriver::destroyFence);
        Hook(vkWaitForFences, WSIDriver::WaitForFences, WSIDriver::waitForFences);
        VkDebugUtilsMessengerCreateInfoEXT info{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        info.pfnUserCallback = WSIDriver::Validation;
        ASSERT_EQ(
            vkCreateDebugUtilsMessengerEXT(session->rhi.GetInstance(), &info, nullptr, &messenger),
            VK_SUCCESS);
    }
    void TearDown() override
    {
        if (!session)
        {
            return;
        }
        session->rhi.WaitDeviceIdle();
        ZEN_DELETE(commands);
        if (viewport)
        {
            session->rhi.DestroyViewport(viewport);
        }
        if (swapchain)
        {
            swapchain->Destroy(nullptr);
        }
        swapchain.reset();
        for (size_t i = restore.size(); i > 0; --i)
        {
            restore[i - 1]();
        }
        vkDestroyDebugUtilsMessengerEXT(session->rhi.GetInstance(), messenger, nullptr);
        window.reset();
        session.reset();
        WSIDriver::presentModes = {};
        WSIDriver::liveSemaphores = {};
        WSIDriver::liveFences     = {};
    }
    void Create(bool vsync = false)
    {
        swapchain = std::make_unique<VulkanSwapchain>(window.get(), 64, 64, vsync, nullptr);
    }
    void CreateNativeViewport()
    {
        WSIDriver::native = true;
        viewport          = session->rhi.CreateViewport(window.get(), 64, 64, false);
        context           = static_cast<FVulkanCommandListContext*>(
            session->rhi.GetCommandContext(RHICommandContextType::eGraphics));
        commands = RHICommandList::Create(context);
    }
    RHISubmissionResult RenderAndSubmit()
    {
        RHIRenderingLayout layout{};
        layout.SetRenderArea(0, 0, viewport->GetWidth(), viewport->GetHeight());
        layout.AddColorRenderTarget(viewport->GetSwapchainFormat(), viewport->GetColorBackBuffer(),
                                    RHIRenderTargetLoadOp::eClear, RHIRenderTargetStoreOp::eStore);
        context->RHIBeginRendering(&layout);
        context->RHIEndRendering();
        viewport->PrepareForPresent(commands);
        HeapVector<RHIPlatformCommandList*> platform;
        RHICommandList* lists[] = {commands};
        session->rhi.FinalizeCommandLists({lists, 1}, platform);
        session->rhi.SubmitPlatformCommandLists(platform);
        return session->rhi.FlushAllGPUCommands();
    }
};

TEST_F(VulkanSwapchainIntegrationTest, CompleteEnumerationKeepsImagesBeyondEight)
{
    WSIDriver::incomplete = true;
    Create();
    ASSERT_EQ(swapchain->GetNumSwapchainImages(), 12u);
    EXPECT_EQ(swapchain->GetSwapchainImages()[11], (VkImage)uintptr_t(12));
    VulkanSemaphore* semaphore{};
    EXPECT_EQ(swapchain->AcquireNextImage(&semaphore), 11);
    EXPECT_NE(semaphore, nullptr);
    EXPECT_EQ(WSIDriver::acquiredCalls, 1u);
}

TEST_F(VulkanSwapchainIntegrationTest, NegotiatesFixedExtentUnlimitedCountTransformAndUsage)
{
    Create();
    EXPECT_EQ(WSIDriver::lastCreate.imageExtent.width, 96u);
    EXPECT_EQ(WSIDriver::lastCreate.imageExtent.height, 80u);
    EXPECT_EQ(swapchain->GetExtent().width, 96u);
    EXPECT_EQ(WSIDriver::lastCreate.minImageCount,
              std::max(2u, GRHIFrameState.GetNumFramesInFlight()));
    EXPECT_EQ(WSIDriver::lastCreate.preTransform, VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR);
    EXPECT_EQ(WSIDriver::lastCreate.compositeAlpha, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR);
    EXPECT_EQ(WSIDriver::lastCreate.imageUsage, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    EXPECT_EQ(WSIDriver::lastCreate.presentMode, VK_PRESENT_MODE_FIFO_KHR);
}

TEST_F(VulkanSwapchainIntegrationTest, VariableExtentAndFiniteImageLimitAreClamped)
{
    WSIDriver::caps.currentExtent  = {UINT32_MAX, UINT32_MAX};
    WSIDriver::caps.minImageExtent = {80, 90};
    WSIDriver::caps.maxImageCount  = 2;
    Create();
    EXPECT_EQ(swapchain->GetExtent().width, 80u);
    EXPECT_EQ(swapchain->GetExtent().height, 90u);
    EXPECT_EQ(WSIDriver::lastCreate.minImageCount, 2u);
}

TEST_F(VulkanSwapchainIntegrationTest, VSyncChoicesUseOnlyAdvertisedModes)
{
    WSIDriver::presentModes = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                               VK_PRESENT_MODE_IMMEDIATE_KHR};
    Create(false);
    EXPECT_EQ(WSIDriver::lastCreate.presentMode, VK_PRESENT_MODE_IMMEDIATE_KHR);
    swapchain->Destroy(nullptr);
    Create(true);
    EXPECT_EQ(WSIDriver::lastCreate.presentMode, VK_PRESENT_MODE_MAILBOX_KHR);
}

TEST_F(VulkanSwapchainIntegrationTest, UnsupportedPresentationAndTransferUsageFailBeforeCreation)
{
    WSIDriver::canPresent = false;
    EXPECT_THROW(Create(), std::runtime_error);
    WSIDriver::canPresent               = true;
    WSIDriver::caps.supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    EXPECT_THROW(Create(), std::runtime_error);
    EXPECT_EQ(WSIDriver::createdCalls, 0u);
}

TEST_F(VulkanSwapchainIntegrationTest, ZeroExtentDefersSwapchainCreation)
{
    WSIDriver::caps.currentExtent = {0, 0};
    Create();
    EXPECT_EQ(WSIDriver::createdCalls, 0u);
    EXPECT_EQ(swapchain->GetNumSwapchainImages(), 0u);
    VulkanSemaphore* semaphore{};
    EXPECT_EQ(swapchain->AcquireNextImage(&semaphore), -1);
    EXPECT_EQ(WSIDriver::acquiredCalls, 0u);
}

TEST_F(VulkanSwapchainIntegrationTest, AcquireFailuresDoNotPublishAnImageOrSemaphore)
{
    Create();
    for (VkResult result :
         {VK_TIMEOUT, VK_NOT_READY, VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR,
          VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST})
    {
        WSIDriver::acquireResult   = result;
        VulkanSemaphore* semaphore = reinterpret_cast<VulkanSemaphore*>(uintptr_t(1));
        if (result == VK_ERROR_DEVICE_LOST || result == VK_ERROR_OUT_OF_HOST_MEMORY ||
            result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
        {
            EXPECT_THROW(swapchain->AcquireNextImage(&semaphore), std::runtime_error);
        }
        else
        {
            EXPECT_EQ(swapchain->AcquireNextImage(&semaphore), -1);
        }
        EXPECT_EQ(semaphore, nullptr);
        EXPECT_EQ(swapchain->GetLastResult(), result);
    }
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
    EXPECT_EQ(WSIDriver::acquiredCalls, 7u);
}

TEST_F(VulkanSwapchainIntegrationTest, InvalidSuccessfulIndexFailsWithoutReacquiring)
{
    Create();
    WSIDriver::acquireIndex = 12;
    VulkanSemaphore* semaphore{};
    EXPECT_THROW(swapchain->AcquireNextImage(&semaphore), std::runtime_error);
    EXPECT_EQ(WSIDriver::acquiredCalls, 1u);
    EXPECT_EQ(semaphore, nullptr);
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
}

TEST_F(VulkanSwapchainIntegrationTest, SuboptimalAcquisitionSurvivesSuccessfulPresent)
{
    Create();
    WSIDriver::acquireResult = VK_SUBOPTIMAL_KHR;
    VulkanSemaphore* semaphore{};
    EXPECT_EQ(swapchain->AcquireNextImage(&semaphore), 11);
    EXPECT_TRUE(swapchain->Present(nullptr));
    EXPECT_TRUE(swapchain->NeedsRecreation());
    EXPECT_EQ(swapchain->GetLastResult(), VK_SUBOPTIMAL_KHR);
}

TEST_F(VulkanSwapchainIntegrationTest, PresentErrorsDistinguishRecreationFromFatalFailure)
{
    Create();
    for (VkResult result :
         {VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR, VK_ERROR_DEVICE_LOST})
    {
        VulkanSemaphore* semaphore{};
        ASSERT_EQ(swapchain->AcquireNextImage(&semaphore), 11);
        WSIDriver::presentResult = result;
        if (result == VK_ERROR_DEVICE_LOST)
        {
            EXPECT_THROW(swapchain->Present(nullptr), std::runtime_error);
        }
        else
        {
            EXPECT_FALSE(swapchain->Present(nullptr));
            EXPECT_TRUE(swapchain->NeedsRecreation());
        }
    }
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
}

TEST_F(VulkanSwapchainIntegrationTest,
       NativeResizeRebuildsSemaphoresForGrowingAndShrinkingImageCounts)
{
    WSIDriver::count = 0;
    CreateNativeViewport();
    for (uint32_t count : {0u, 12u, 0u})
    {
        WSIDriver::count = count;
        glfwSetWindowSize(window->GetHandle(), 80 + count, 72 + count);
        glfwPollEvents();
        viewport->Resize(80 + count, 72 + count);
        EXPECT_EQ(viewport->GetWidth(), WSIDriver::lastCreate.imageExtent.width);
        EXPECT_EQ(viewport->GetHeight(), WSIDriver::lastCreate.imageExtent.height);
        const auto previous = WSIDriver::presentationSemaphores;
        for (int frame = 0; frame < 8; ++frame)
        {
            ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
            ASSERT_TRUE(viewport->Present());
            EXPECT_EQ(previous.count(WSIDriver::lastPresent), 0u);
        }
    }
}

TEST_F(VulkanSwapchainIntegrationTest, NativeRejectedSubmissionRetriesSameAcquisition)
{
    CreateNativeViewport();
    WSIDriver::rejectSubmissions = 1;
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eRejected);
    const auto acquired = WSIDriver::lastAcquire;
    EXPECT_FALSE(viewport->Present());
    EXPECT_EQ(WSIDriver::presentedCalls, 0u);
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
    EXPECT_EQ(WSIDriver::acquiredCalls, 1u);
    EXPECT_EQ(WSIDriver::lastAcquire, acquired);
}

class VulkanPresentationSubmissionTest : public VulkanSwapchainIntegrationTest,
                                         public testing::WithParamInterface<bool>
{
protected:
    void SetUp() override
    {
        VulkanSwapchainIntegrationTest::SetUp();
        if (GetParam())
        {
            ASSERT_TRUE(session->rhi.GetDevice()->SupportsTimelineSemaphore());
        }
        else
        {
            session->rhi.GetDevice()->GetExtensionFlags().hasTimelineSemaphore = 0;
        }
        WSIDriver::count = 0;
        CreateNativeViewport();
    }

    void FinalizeRecording(HeapVector<RHIPlatformCommandList*>& platform)
    {
        RHICommandList* lists[] = {commands};
        session->rhi.FinalizeCommandLists({lists, 1}, platform);
    }

    RHISubmissionResult SubmitRecording()
    {
        HeapVector<RHIPlatformCommandList*> platform;
        FinalizeRecording(platform);
        session->rhi.SubmitPlatformCommandLists(platform);
        return session->rhi.FlushAllGPUCommands();
    }

    bool ExpectPresentationSkipped()
    {
        // A regression must fail the test without queueing an unsignaled binary wait.
        static uint32_t attemptedPresent;
        attemptedPresent = 0;
        test::ScopedVulkanCall<PFN_vkQueuePresentKHR> preventWait(
            vkQueuePresentKHR, +[](VkQueue, const VkPresentInfoKHR*) -> VkResult {
                ++attemptedPresent;
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            });
        bool presented = false;
        EXPECT_NO_THROW(presented = viewport->Present());
        EXPECT_FALSE(presented);
        EXPECT_EQ(attemptedPresent, 0u);
        return !presented && attemptedPresent == 0;
    }
};

TEST_P(VulkanPresentationSubmissionTest, RejectedCopyAfterSuccessfulFramesRetainsAcquisition)
{
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    ASSERT_TRUE(viewport->Present());
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        session->rhi.WaitDeviceIdle();
        const uint64_t previousSerial = context->GetLastSubmittedSerial();
        ASSERT_GT(previousSerial, 0u);
        const uint32_t presentedCalls = WSIDriver::presentedCalls;
        WSIDriver::rejectSubmissions  = 1;
        ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eRejected);
        EXPECT_EQ(context->GetLastSubmittedSerial(), previousSerial);
        const VkSemaphore acquired   = WSIDriver::lastAcquire;
        const uint32_t acquiredCalls = WSIDriver::acquiredCalls;
        ASSERT_TRUE(ExpectPresentationSkipped());

        // A later accepted submission on the same context still does not signal the copy.
        context->GetCommandBuffer();
        ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
        EXPECT_GT(context->GetLastSubmittedSerial(), previousSerial);
        ASSERT_TRUE(ExpectPresentationSkipped());
        EXPECT_EQ(WSIDriver::presentedCalls, presentedCalls);

        ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
        ASSERT_TRUE(viewport->Present());
        EXPECT_EQ(WSIDriver::acquiredCalls, acquiredCalls);
        EXPECT_EQ(WSIDriver::lastAcquire, acquired);
        EXPECT_EQ(WSIDriver::presentedCalls, presentedCalls + 1);
    }
}

TEST_P(VulkanPresentationSubmissionTest, PreparedCopyIgnoresEarlierSubmissionOnReusedContext)
{
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    ASSERT_TRUE(viewport->Present());
    session->rhi.WaitDeviceIdle();
    const uint64_t previousSerial = context->GetLastSubmittedSerial();

    context->GetCommandBuffer();
    HeapVector<RHIPlatformCommandList*> earlier;
    FinalizeRecording(earlier);
    viewport->PrepareForPresent(commands);
    // Preparing a new copy must not erase the context's earlier lifetime wait serial.
    EXPECT_EQ(context->GetLastSubmittedSerial(), previousSerial);
    HeapVector<RHIPlatformCommandList*> copy;
    FinalizeRecording(copy);

    session->rhi.SubmitPlatformCommandLists(earlier);
    ASSERT_EQ(session->rhi.FlushAllGPUCommands(), RHISubmissionResult::eSuccess);
    EXPECT_GT(context->GetLastSubmittedSerial(), previousSerial);
    ASSERT_TRUE(ExpectPresentationSkipped());
    session->rhi.SubmitPlatformCommandLists(copy);
    ASSERT_EQ(session->rhi.FlushAllGPUCommands(), RHISubmissionResult::eSuccess);
    ASSERT_TRUE(viewport->Present());
}

TEST_P(VulkanPresentationSubmissionTest, AcceptedCopySurvivesLaterRejectionAndContextDestruction)
{
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    ASSERT_GT(context->GetLastSubmittedSerial(), 0u);
    session->rhi.WaitDeviceIdle();
    context->RHIWaitUntilCompleted();
    EXPECT_EQ(context->GetLastSubmittedSerial(), 0u);
    // Completion recycles the copy's workload before the viewport reads its acceptance.
    context->GetCommandBuffer();
    WSIDriver::rejectSubmissions = 1;
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eRejected);
    EXPECT_EQ(context->GetLastSubmittedSerial(), 0u);
    ZEN_DELETE(commands);
    commands = nullptr;
    context  = nullptr;
    ASSERT_TRUE(viewport->Present());
    EXPECT_EQ(WSIDriver::presentedCalls, 1u);
}

TEST_P(VulkanPresentationSubmissionTest, SignalAcceptanceFollowsMergedWorkAndSurvivesPoolReuse)
{
    VulkanQueue* pQueue              = context->GetQueue();
    VulkanSemaphoreManager* pManager = session->rhi.GetDevice()->GetSemaphoreManager();
    VulkanSemaphore* first           = pManager->GetOrCreateSemaphore();
    VulkanSemaphore* second          = pManager->GetOrCreateSemaphore();
    const uint64_t firstGeneration   = first->GetSignalGeneration();
    const uint64_t secondGeneration  = second->GetSignalGeneration();
    const uint64_t beforeSubmission  = pQueue->GetLastSubmittedSerial();
    HeapVector<RHIPlatformCommandList*> platform;
    // The first workload has no signals; merging transfers the second's two signals to it.
    context->GetCommandBuffer();
    FinalizeRecording(platform);
    context->GetCommandBuffer();
    context->AddSignalSemaphore(first);
    context->AddSignalSemaphore(second);
    FinalizeRecording(platform);
    EXPECT_EQ(first->GetSignalSubmissionSerial(pQueue, firstGeneration), 0u);
    EXPECT_EQ(second->GetSignalSubmissionSerial(pQueue, secondGeneration), 0u);
    session->rhi.SubmitPlatformCommandLists(platform);
    ASSERT_EQ(session->rhi.FlushAllGPUCommands(), RHISubmissionResult::eSuccess);
    const uint64_t firstSerial  = first->GetSignalSubmissionSerial(pQueue, firstGeneration);
    const uint64_t secondSerial = second->GetSignalSubmissionSerial(pQueue, secondGeneration);
    EXPECT_GT(firstSerial, beforeSubmission);
    EXPECT_EQ(secondSerial, firstSerial);
    EXPECT_EQ(secondSerial, context->GetLastSubmittedSerial());
    EXPECT_EQ(pQueue->GetLastSubmittedSerial() - beforeSubmission, GetParam() ? 1u : 2u);
    EXPECT_EQ(first->GetSignalGeneration(), firstGeneration + 1);
    EXPECT_EQ(second->GetSignalGeneration(), secondGeneration + 1);
    context->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, first);
    context->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, second);
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
    session->rhi.WaitDeviceIdle();

    VulkanSemaphore* rejected         = pManager->GetOrCreateSemaphore();
    const uint64_t rejectedGeneration = rejected->GetSignalGeneration();
    context->GetCommandBuffer();
    context->AddSignalSemaphore(rejected);
    WSIDriver::rejectSubmissions = 1;
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eRejected);
    EXPECT_EQ(rejected->GetSignalSubmissionSerial(pQueue, rejectedGeneration), 0u);
    EXPECT_EQ(rejected->GetSignalGeneration(), rejectedGeneration);
    VulkanSemaphore* replacement = pManager->GetOrCreateSemaphore();
    for (uint32_t reuse = 0; reuse < 4; ++reuse)
    {
        const uint64_t generation = replacement->GetSignalGeneration();
        context->GetCommandBuffer();
        context->AddSignalSemaphore(replacement);
        EXPECT_EQ(replacement->GetSignalSubmissionSerial(pQueue, generation), 0u);
        ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
        const uint64_t serial = replacement->GetSignalSubmissionSerial(pQueue, generation);
        EXPECT_GT(serial, secondSerial);
        EXPECT_EQ(replacement->GetSignalGeneration(), generation + 1);
        context->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, replacement);
        ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
        session->rhi.WaitDeviceIdle();
        EXPECT_EQ(replacement->GetSignalSubmissionSerial(pQueue, generation), serial);
        EXPECT_EQ(first->GetSignalSubmissionSerial(pQueue, firstGeneration), firstSerial);
        EXPECT_EQ(second->GetSignalSubmissionSerial(pQueue, secondGeneration), secondSerial);
        EXPECT_EQ(rejected->GetSignalSubmissionSerial(pQueue, rejectedGeneration), 0u);
        EXPECT_EQ(rejected->GetSignalGeneration(), rejectedGeneration);
    }
}

TEST_P(VulkanPresentationSubmissionTest, RecycledSemaphoreDoesNotInheritSignalAcceptance)
{
    VulkanQueue* pQueue              = context->GetQueue();
    VulkanSemaphoreManager* pManager = session->rhi.GetDevice()->GetSemaphoreManager();
    VulkanSemaphore* semaphore       = pManager->GetOrCreateSemaphore();
    const uint64_t initialGeneration = semaphore->GetSignalGeneration();
    context->AddSignalSemaphore(semaphore);
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
    const uint64_t firstSerial = semaphore->GetSignalSubmissionSerial(pQueue, initialGeneration);
    const uint64_t acceptedGeneration = semaphore->GetSignalGeneration();
    ASSERT_GT(firstSerial, 0u);
    context->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, semaphore);
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
    session->rhi.WaitDeviceIdle();
    VulkanSemaphore* original = semaphore;
    pManager->ReleaseSemaphore(semaphore);
    EXPECT_EQ(semaphore, nullptr);
    semaphore = pManager->GetOrCreateSemaphore();
    ASSERT_EQ(semaphore, original);
    EXPECT_EQ(semaphore->GetSignalGeneration(), acceptedGeneration);
    EXPECT_EQ(semaphore->GetSignalSubmissionSerial(pQueue, initialGeneration), 0u);
    context->AddSignalSemaphore(semaphore);
    EXPECT_EQ(semaphore->GetSignalSubmissionSerial(pQueue, acceptedGeneration), 0u);
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
    EXPECT_EQ(semaphore->GetSignalGeneration(), acceptedGeneration + 1);
    EXPECT_GT(semaphore->GetSignalSubmissionSerial(pQueue, acceptedGeneration), firstSerial);
    context->AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, semaphore);
    ASSERT_EQ(SubmitRecording(), RHISubmissionResult::eSuccess);
    session->rhi.WaitDeviceIdle();
    pManager->ReleaseSemaphore(semaphore);
}

TEST_P(VulkanPresentationSubmissionTest, SignalAcceptanceRequiresTheSubmittingQueue)
{
    VulkanDevice* pDevice      = session->rhi.GetDevice();
    VulkanSemaphore* semaphore = pDevice->GetSemaphoreManager()->GetOrCreateSemaphore();
    const uint64_t generation  = semaphore->GetSignalGeneration();
    FVulkanCommandListContext compute(RHICommandContextType::eAsyncCompute, pDevice);
    compute.AddSignalSemaphore(semaphore);
    ASSERT_EQ(compute.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);
    EXPECT_GT(semaphore->GetSignalSubmissionSerial(compute.GetQueue(), generation), 0u);
    EXPECT_EQ(semaphore->GetSignalSubmissionSerial(nullptr, generation), 0u);
    if (pDevice->GetGfxQueue() != compute.GetQueue())
    {
        EXPECT_EQ(semaphore->GetSignalSubmissionSerial(pDevice->GetGfxQueue(), generation), 0u);
    }
    compute.AddWaitSemaphore(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, semaphore);
    ASSERT_EQ(compute.SubmitRecordedWorkloads(), RHISubmissionResult::eSuccess);
    session->rhi.WaitDeviceIdle();
    pDevice->GetSemaphoreManager()->ReleaseSemaphore(semaphore);
}

INSTANTIATE_TEST_SUITE_P(TimelineAndFence, VulkanPresentationSubmissionTest, testing::Bool());

TEST_F(VulkanSwapchainIntegrationTest,
       NativeAcquireTimeoutKeepsSwapchainAndMinimizeSuspendsAcquisition)
{
    CreateNativeViewport();
    const uint32_t creations = WSIDriver::createdCalls;
    WSIDriver::acquireResult = VK_TIMEOUT;
    viewport->PrepareForPresent(commands);
    EXPECT_FALSE(viewport->Present());
    EXPECT_EQ(WSIDriver::createdCalls, creations);
    viewport->Resize(0, 0);
    WSIDriver::acquireResult = VK_SUCCESS;
    viewport->PrepareForPresent(commands);
    EXPECT_FALSE(viewport->Present());
    EXPECT_EQ(WSIDriver::acquiredCalls, 1u);
    viewport->Resize(64, 64);
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
}

TEST_F(VulkanSwapchainIntegrationTest, NativeSurfaceLossReplacesSurfaceAndOutOfDateReusesIt)
{
    CreateNativeViewport();
    for (VkResult result : {VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR})
    {
        WSIDriver::acquireResult = result;
        viewport->PrepareForPresent(commands);
        EXPECT_FALSE(viewport->Present());
        if (result == VK_ERROR_SURFACE_LOST_KHR)
        {
            EXPECT_EQ(WSIDriver::lastCreate.oldSwapchain, VK_NULL_HANDLE);
        }
        else
        {
            EXPECT_NE(WSIDriver::lastCreate.oldSwapchain, VK_NULL_HANDLE);
        }
        WSIDriver::acquireResult = VK_SUCCESS;
        ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
        EXPECT_TRUE(viewport->Present());
    }
    EXPECT_EQ(WSIDriver::createdCalls, 3u);
}

TEST_F(VulkanSwapchainIntegrationTest, NativePresentSuboptimalRecreatesAfterConsumingSubmission)
{
    CreateNativeViewport();
    WSIDriver::presentResult = VK_SUBOPTIMAL_KHR;
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
    EXPECT_EQ(WSIDriver::createdCalls, 2u);
    WSIDriver::presentResult = VK_SUCCESS;
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
}

TEST_F(VulkanSwapchainIntegrationTest,
       NativePresentOutOfDateAndSurfaceLossRecreateTheCorrectObjects)
{
    CreateNativeViewport();
    for (VkResult result : {VK_ERROR_OUT_OF_DATE_KHR, VK_ERROR_SURFACE_LOST_KHR})
    {
        WSIDriver::presentResult = result;
        ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
        EXPECT_FALSE(viewport->Present());
        if (result == VK_ERROR_SURFACE_LOST_KHR)
        {
            EXPECT_EQ(WSIDriver::lastCreate.oldSwapchain, VK_NULL_HANDLE);
        }
        else
        {
            EXPECT_NE(WSIDriver::lastCreate.oldSwapchain, VK_NULL_HANDLE);
        }
    }
    WSIDriver::presentResult = VK_SUCCESS;
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
}

TEST_F(VulkanSwapchainIntegrationTest, FailedViewportCreationCleansUpAndAllowsRetry)
{
    WSIDriver::native       = true;
    WSIDriver::createResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    EXPECT_THROW(session->rhi.CreateViewport(window.get(), 64, 64, false), std::runtime_error);
    EXPECT_FALSE(session->rhi.AreSubmissionsBlocked());
    WSIDriver::createResult = VK_SUCCESS;
    CreateNativeViewport();
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
}

TEST_F(VulkanSwapchainIntegrationTest, NativeMinimizeAfterRejectedSubmissionPreservesAcquisition)
{
    CreateNativeViewport();
    WSIDriver::rejectSubmissions = 1;
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eRejected);
    const uint32_t destroyedBeforeResize = WSIDriver::semaphoreDestructions;
    viewport->Resize(0, 0);
    viewport->PrepareForPresent(commands);
    EXPECT_FALSE(viewport->Present());
    EXPECT_EQ(WSIDriver::acquiredCalls, 1u);
    viewport->Resize(64, 64);
    ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
    EXPECT_TRUE(viewport->Present());
    // Vulkan may reuse a destroyed native handle for the replacement semaphore.
    EXPECT_GT(WSIDriver::semaphoreDestructions, destroyedBeforeResize);
}

TEST_F(VulkanSwapchainIntegrationTest, NativeDeviceLossBlocksFurtherPresentationAndRecreation)
{
    CreateNativeViewport();
    WSIDriver::acquireResult = VK_ERROR_DEVICE_LOST;
    EXPECT_THROW(viewport->PrepareForPresent(commands), std::runtime_error);
    EXPECT_TRUE(session->rhi.AreSubmissionsBlocked());
    EXPECT_THROW(viewport->Present(), std::runtime_error);
    EXPECT_THROW(viewport->Resize(128, 128), std::runtime_error);
    EXPECT_EQ(WSIDriver::createdCalls, 1u);
    EXPECT_EQ(WSIDriver::presentedCalls, 0u);
}

TEST_F(VulkanSwapchainIntegrationTest, FailedAcquireFenceCreationReleasesPartialSwapchain)
{
    for (uint32_t failureAt : {1u, 2u, 5u})
    {
        WSIDriver::fenceCreationFailures = failureAt;
        EXPECT_THROW(Create(), std::runtime_error);
        EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
        EXPECT_TRUE(WSIDriver::liveFences.empty());
    }
    EXPECT_EQ(WSIDriver::destroyedCalls, 3u);
    EXPECT_NO_THROW(Create());
}

TEST_F(VulkanSwapchainIntegrationTest, PendingPresentationFencePreventsEarlyDestruction)
{
    ASSERT_TRUE(session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1);
    WSIDriver::holdPresentation = true;
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    ASSERT_NE(WSIDriver::heldPresentFence, VK_NULL_HANDLE);
    swapchain->Destroy(nullptr);
    EXPECT_EQ(WSIDriver::heldPresentFence, VK_NULL_HANDLE);
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
}

TEST_F(VulkanSwapchainIntegrationTest, FailedPresentationFenceWaitRetainsResourcesForRetry)
{
    ASSERT_TRUE(session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1);
    WSIDriver::holdPresentation = WSIDriver::rejectPresentWait = true;
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    const size_t semaphores = WSIDriver::liveSemaphores.size();
    const size_t fences     = WSIDriver::liveFences.size();
    EXPECT_THROW(swapchain->Destroy(nullptr), std::runtime_error);
    EXPECT_EQ(WSIDriver::liveSemaphores.size(), semaphores);
    EXPECT_EQ(WSIDriver::liveFences.size(), fences);
    EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
    EXPECT_NO_THROW(swapchain->Destroy(nullptr));
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
}

TEST_F(VulkanSwapchainIntegrationTest, RejectedPresentationDoesNotWaitOnUnsubmittedFence)
{
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    WSIDriver::presentResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    EXPECT_THROW(swapchain->Present(nullptr), std::runtime_error);
    EXPECT_NO_THROW(swapchain->Destroy(nullptr));
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
}

TEST_F(VulkanSwapchainIntegrationTest, FallbackDefersConsecutiveRetirementsUntilReacquisitionProof)
{
    session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1 = 0;
    Create();
    constexpr uint32_t generations = 12;
    for (uint32_t generation = 0; generation < generations; ++generation)
    {
        VulkanSemaphore* semaphore{};
        ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
        ASSERT_TRUE(swapchain->Present(nullptr));
        VulkanSwapchainRecreateInfo recreate;
        swapchain->Destroy(&recreate);
        EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
        EXPECT_EQ(WSIDriver::liveSemaphores.size(), generation + 1u);
        swapchain = std::make_unique<VulkanSwapchain>(window.get(), 64, 64, false, &recreate);
        EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
    }
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    session->rhi.WaitDeviceIdle();
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    // Merely returning from acquire does not prove its asynchronous signal completed.
    EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
    ASSERT_TRUE(swapchain->Present(nullptr));
    session->rhi.WaitDeviceIdle();
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    EXPECT_EQ(WSIDriver::destroyedCalls, generations);
    EXPECT_EQ(WSIDriver::liveSemaphores.size(), 2u * swapchain->GetNumSwapchainImages());
    EXPECT_EQ(WSIDriver::presentFenceCalls, 0u);
}

TEST_F(VulkanSwapchainIntegrationTest, FallbackCreationFailureCleansRetiredPredecessors)
{
    session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1 = 0;
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    VulkanSwapchainRecreateInfo recreate;
    swapchain->Destroy(&recreate);
    WSIDriver::createResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    EXPECT_THROW(VulkanSwapchain(window.get(), 64, 64, false, &recreate), std::runtime_error);
    EXPECT_EQ(WSIDriver::destroyedCalls, 1u);
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
    EXPECT_EQ(recreate.swapchain, VK_NULL_HANDLE);
    EXPECT_EQ(recreate.surface, VK_NULL_HANDLE);
    EXPECT_TRUE(recreate.retiredSwapchains.empty());
}


TEST_F(VulkanSwapchainIntegrationTest, FailedSemaphoreCreationReleasesPartialSwapchain)
{
    for (uint32_t failureAt : {1u, 2u, 5u})
    {
        WSIDriver::semaphoreFailureCountdown = failureAt;
        EXPECT_THROW(Create(), std::runtime_error);
        EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
        EXPECT_TRUE(WSIDriver::liveFences.empty());
    }
    EXPECT_EQ(WSIDriver::destroyedCalls, 3u);
    EXPECT_NO_THROW(Create());
}

TEST_F(VulkanSwapchainIntegrationTest, FallbackZeroExtentPreservesOldSwapchainLinkForRestoration)
{
    session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1 = 0;
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    const VkSwapchainKHR oldSwapchain = swapchain->GetVkHandle();
    VulkanSwapchainRecreateInfo recreate;
    swapchain->Destroy(&recreate);
    WSIDriver::caps.currentExtent = {0, 0};
    swapchain = std::make_unique<VulkanSwapchain>(window.get(), 64, 64, false, &recreate);
    EXPECT_EQ(swapchain->GetNumSwapchainImages(), 0u);
    EXPECT_EQ(swapchain->GetVkHandle(), oldSwapchain);
    EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
    swapchain->Destroy(&recreate);
    WSIDriver::caps.currentExtent = {64, 64};
    swapchain = std::make_unique<VulkanSwapchain>(window.get(), 64, 64, false, &recreate);
    EXPECT_EQ(WSIDriver::lastCreate.oldSwapchain, oldSwapchain);
    EXPECT_EQ(WSIDriver::destroyedCalls, 0u);
    swapchain->Destroy(nullptr);
    EXPECT_EQ(WSIDriver::destroyedCalls, 2u);
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
}

TEST_F(VulkanSwapchainIntegrationTest, FallbackZeroExtentTeardownDestroysSharedOldHandleOnce)
{
    session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1 = 0;
    Create();
    VulkanSemaphore* semaphore{};
    ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
    ASSERT_TRUE(swapchain->Present(nullptr));
    VulkanSwapchainRecreateInfo recreate;
    swapchain->Destroy(&recreate);
    WSIDriver::caps.currentExtent = {0, 0};
    swapchain = std::make_unique<VulkanSwapchain>(window.get(), 64, 64, false, &recreate);
    swapchain->Destroy(nullptr);
    EXPECT_EQ(WSIDriver::destroyedCalls, 1u);
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
}

class VulkanSwapchainRetirementIntegrationTest :
    public VulkanSwapchainIntegrationTest,
    public testing::WithParamInterface<bool>
{
protected:
    void SetUp() override
    {
        VulkanSwapchainIntegrationTest::SetUp();
        if (GetParam())
        {
            ASSERT_TRUE(session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1);
        }
        else
        {
            // Exercise the unextended algorithm even on a maintenance-capable device.
            session->rhi.GetDevice()->GetExtensionFlags().hasSwapchainMaintenance1 = 0;
        }
        WSIDriver::count = 0;
    }
};

TEST_P(VulkanSwapchainRetirementIntegrationTest,
       NativeAbandonedAcquisitionsReleaseSemaphoresAndFences)
{
    WSIDriver::native = true;
    for (uint32_t iteration = 0; iteration < 32; ++iteration)
    {
        Create();
        VulkanSemaphore* semaphore{};
        ASSERT_GE(swapchain->AcquireNextImage(&semaphore), 0);
        // No graphics wait consumes this semaphore's acquisition signal.
        swapchain->Destroy(nullptr);
        EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
        EXPECT_TRUE(WSIDriver::liveFences.empty());
    }
}

TEST_P(VulkanSwapchainRetirementIntegrationTest, NativeResizeChurnReturnsToStableLiveCounts)
{
    CreateNativeViewport();
    const uint32_t images           = WSIDriver::exposedCounts.begin()->second;
    const size_t expectedSemaphores = 2u * images;
    const size_t expectedFences     = (GetParam() ? 2u : 1u) * images;
    for (uint32_t iteration = 0; iteration < 100; ++iteration)
    {
        viewport->Resize(64 + (iteration % 2) * 16, 64);
        for (uint32_t frame = 0; frame < 8; ++frame)
        {
            ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
            ASSERT_TRUE(viewport->Present());
        }
        session->rhi.WaitDeviceIdle();
        ASSERT_EQ(RenderAndSubmit(), RHISubmissionResult::eSuccess);
        ASSERT_TRUE(viewport->Present());
        EXPECT_EQ(WSIDriver::liveSemaphores.size(), expectedSemaphores) << iteration;
        EXPECT_EQ(WSIDriver::liveFences.size(), expectedFences) << iteration;
        EXPECT_EQ(WSIDriver::createdCalls - WSIDriver::destroyedCalls, 1u) << iteration;
    }
    session->rhi.WaitDeviceIdle();
    session->rhi.DestroyViewport(viewport);
    viewport = nullptr;
    EXPECT_TRUE(WSIDriver::liveSemaphores.empty());
    EXPECT_TRUE(WSIDriver::liveFences.empty());
    EXPECT_EQ(WSIDriver::createdCalls, WSIDriver::destroyedCalls);
    EXPECT_EQ(WSIDriver::presentFenceCalls != 0, GetParam());
}

INSTANTIATE_TEST_SUITE_P(PresentationCompletion,
                         VulkanSwapchainRetirementIntegrationTest,
                         testing::Bool());
} // namespace
