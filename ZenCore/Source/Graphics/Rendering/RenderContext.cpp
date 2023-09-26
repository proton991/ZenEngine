#include "Graphics/Rendering/RenderContext.h"

namespace zen
{
RenderContext::RenderContext(val::Device& device, platform::GlfwWindowImpl* window) :
    m_valDevice(device)
{
    m_surface   = window->CreateSurface(device.GetInstanceHandle());
    m_swapchain = MakeUnique<val::Swapchain>(m_valDevice, m_surface, window->GetExtent2D());
}

void RenderContext::Init()
{
    for (auto& imageHandle : m_swapchain->GetImages())
    {
        auto image = MakeUnique<val::Image>(m_valDevice, imageHandle, m_swapchain->GetExtent3D(), m_swapchain->GetFormat());
        auto frame = RenderFrame(m_valDevice, std::move(image));
        m_frames.push_back(std::move(frame));
    }
}
} // namespace zen