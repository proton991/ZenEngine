#include "Graphics/VulkanRHI/VulkanRHI.h"

using namespace zen;


int main(int argc, char** argv)
{
    VulkanRHI vkRHI;
    vkRHI.Init();
    platform::WindowConfig windowConfig;

    auto*           window = new platform::GlfwWindowImpl(windowConfig);
    MacOSWindowData data{window->GetHandle(), windowConfig.width, windowConfig.height};

    SurfaceHandle   surfaceHandle = VulkanMacOSPlatform::CreateSurface(vkRHI.GetInstance(), &data);
    SwapchainHandle swapchainHandle = vkRHI.CreateSwapchain(surfaceHandle, true);

    while (!window->ShouldClose()) { window->Update(); }

    vkRHI.DestroySwapchain(swapchainHandle);
    VulkanMacOSPlatform::DestroySurface(vkRHI.GetInstance(), surfaceHandle);
    vkRHI.Destroy();
}