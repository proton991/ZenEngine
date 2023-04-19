#pragma once
#include <vulkan/vulkan.hpp>
namespace zen::vulkan {
class WSIPlatform {
public:
  virtual ~WSIPlatform() = default;

  virtual vk::SurfaceKHR CreateSurface(vk::Instance instance, vk::PhysicalDevice gpu) const = 0;

  virtual std::vector<const char*> GetInstanceExtensions() = 0;
  virtual std::vector<const char*> GetDeviceExtensions() { return {"VK_KHR_swapchain"}; }
};
}  // namespace zen::vulkan