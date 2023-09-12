#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace zen::val
{
class Device;
class RenderPass;

class Framebuffer
{
public:
    Framebuffer(Device& device, RenderPass& renderPass, const std::vector<VkImageView>& attachments, VkExtent3D extent3D);

    ~Framebuffer();

    VkFramebuffer GetHandle() const { return m_handle; }

private:
    Device&       m_device;
    VkFramebuffer m_handle{VK_NULL_HANDLE};
    VkExtent3D    m_extent;
};

} // namespace zen::val