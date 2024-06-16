#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "DeviceObject.h"

namespace zen::val
{
class RenderPass;

class Framebuffer : public DeviceObject<VkFramebuffer, VK_OBJECT_TYPE_FRAMEBUFFER>
{
public:
    Framebuffer(const Device& device,
                VkRenderPass renderPassHandle,
                const std::vector<VkImageView>& attachments,
                VkExtent3D extent3D);
    ~Framebuffer();

    Framebuffer(Framebuffer&& other) noexcept;

    VkRect2D GetRenderArea() const
    {
        VkRect2D rect2D{};
        rect2D.offset = {0, 0};
        rect2D.extent = {m_extent.width, m_extent.height};
        return rect2D;
    }

private:
    VkExtent3D m_extent;
};

} // namespace zen::val