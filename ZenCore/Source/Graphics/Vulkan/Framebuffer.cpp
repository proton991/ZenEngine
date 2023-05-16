#include "Graphics/Vulkan/Framebuffer.h"
#include "Graphics/Vulkan/RenderPassVK.h"

namespace zen::vulkan {
Framebuffer::Framebuffer(const Device& device, const RenderPass& renderPass,
                         const std::vector<vk::ImageView> attachments, vk::Extent2D extent,
                         uint32_t layers /*= 1*/)
    : DeviceResource(device, nullptr), m_extent(extent) {
  auto framebufferCI = vk::FramebufferCreateInfo()
                           .setRenderPass(renderPass.GetHandle())
                           .setAttachments(attachments)
                           .setWidth(extent.width)
                           .setHeight(extent.height)
                           .setLayers(layers);

  SetHandle(GetDeviceHandle().createFramebuffer(framebufferCI));
}

Framebuffer::~Framebuffer() {
  if (GetHandle()) {
    GetDeviceHandle().destroyFramebuffer(GetHandle());
  }
}

}  // namespace zen::vulkan
