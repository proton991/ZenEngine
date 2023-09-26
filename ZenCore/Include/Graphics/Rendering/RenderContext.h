#pragma once
#include "RenderFrame.h"
#include "Graphics/Val/Swapchain.h"
#include "Common/UniquePtr.h"
#include "Platform/GlfwWindow.h"


namespace zen
{
class RenderContext
{
public:
    RenderContext(val::Device& device, platform::GlfwWindowImpl* window);

    void Init();

private:
    val::Device& m_valDevice;

    // VkSurfaceKHR is created and managed by RenderContext
    VkSurfaceKHR m_surface{VK_NULL_HANDLE};
    // val::Swapchain is created and managed by RenderContext
    UniquePtr<val::Swapchain> m_swapchain;
    // store frames
    std::vector<RenderFrame> m_frames;
};
} // namespace zen