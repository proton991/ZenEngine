#pragma once
#include <vector>
#include <vulkan/vulkan.hpp>
#include "Platform/NativeWindow.h"

namespace zen::vulkan {
class Context;
class SwapChain {
public:
  struct Desc {
    uint32_t width;
    uint32_t height;
    uint32_t imageCount;
    bool enableVsync;
  };
  SwapChain(const Context& context, vk::SurfaceKHR surface, Desc desc);
  ~SwapChain();

private:
  void Setup(vk::PhysicalDevice gpu, vk::Device device);
  vk::Extent2D ChooseExtent(const vk::Extent2D& requiredExtent, const vk::Extent2D& minExtent,
                            const vk::Extent2D& maxExtent);
  vk::SurfaceFormatKHR ChooseFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats);
  vk::PresentModeKHR ChoosePresentMode(const std::vector<vk::PresentModeKHR>& availablePresentModes,
                                       bool enableVsync);

  const Context& m_context;
  Desc m_desc;
  vk::SwapchainKHR m_swapchain;
  vk::SurfaceKHR m_surface;
  vk::SurfaceFormatKHR m_surfaceFormat;
  vk::PresentModeKHR m_presentMode;
  vk::Extent2D m_extent;

  std::vector<vk::Image> m_images;
  std::vector<vk::UniqueImageView> m_imageViews;
};
}  // namespace zen::vulkan