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
    Framebuffer(Device& device, RenderPass& renderPass, const std::vector<VkImageView>& attachments, VkExtent3D extent3D);
    ~Framebuffer();

private:
    VkExtent3D m_extent;
};

} // namespace zen::val