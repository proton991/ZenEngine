#pragma once
#include "Graphics/Vulkan/DeviceResource.h"

namespace zen::vulkan {
class RenderPass;
class Framebuffer : public DeviceResource<vk::Framebuffer> {
public:
  Framebuffer(const Device& device, const RenderPass& renderPass,
              const std::vector<vk::ImageView> attachments, vk::Extent2D extent,
              uint32_t layers = 1);
  ~Framebuffer();

private:
  vk::Extent2D m_extent{};
};
}  // namespace zen::vulkan