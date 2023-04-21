#pragma once
#include <vulkan/vulkan.hpp>
#include "Common/Base.h"

namespace zen::platform {

struct WindowConfig {
  std::string title{"ZenEngine"};
  bool resizable{false};
  uint32_t width{1280};
  uint32_t height{720};
};

class NativeWindow {
public:
  NativeWindow() = default;
  ZEN_NO_COPY_MOVE(NativeWindow)
  virtual ~NativeWindow() = default;

  virtual vk::SurfaceKHR CreateSurface(vk::Instance instance) const = 0;

  virtual std::vector<const char*> GetInstanceExtensions() = 0;
  virtual std::vector<const char*> GetDeviceExtensions() { return {"VK_KHR_swapchain"}; }
};
}  // namespace zen::platform