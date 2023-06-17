#include "Graphics/Vulkan/SwapchainVK.h"
#include "Graphics/Vulkan/DeviceContext.h"
#include "Platform/NativeWindow.h"

namespace zen::vulkan {
SwapChain::SwapChain(const DeviceContext& context, vk::SurfaceKHR surface, Desc desc)
    : m_context(context), m_desc(std::move(desc)) {
  m_surface = surface;
  Setup(m_context.GetGPU(), m_context.GetLogicalDevice());
}

void SwapChain::Setup(vk::PhysicalDevice gpu, vk::Device device) {
  assert(m_surface);
  vk::SurfaceCapabilitiesKHR surfaceCaps       = gpu.getSurfaceCapabilitiesKHR(m_surface);
  std::vector<vk::SurfaceFormatKHR> formats    = gpu.getSurfaceFormatsKHR(m_surface);
  std::vector<vk::PresentModeKHR> presentModes = gpu.getSurfacePresentModesKHR(m_surface);

  m_surfaceFormat = ChooseFormat(formats);
  m_extent        = ChooseExtent({m_desc.width, m_desc.height}, surfaceCaps.minImageExtent,
                                 surfaceCaps.maxImageExtent);
  m_presentMode   = ChoosePresentMode(presentModes, m_desc.enableVsync);

  uint32_t minImageCount =
      std::clamp(m_desc.imageCount, surfaceCaps.minImageCount, surfaceCaps.maxImageCount);
  auto swapchainCreateInfo = vk::SwapchainCreateInfoKHR()
                                 .setSurface(m_surface)
                                 .setMinImageCount(minImageCount)
                                 .setImageFormat(m_surfaceFormat.format)
                                 .setImageColorSpace(m_surfaceFormat.colorSpace)
                                 .setImageExtent(m_extent)
                                 .setImageArrayLayers(1)
                                 .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
                                 .setPreTransform(surfaceCaps.currentTransform)
                                 .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
                                 .setPresentMode(m_presentMode)
                                 .setClipped(true)
                                 .setOldSwapchain(nullptr);
  // use same queue for graphics and present
  swapchainCreateInfo.setImageSharingMode(vk::SharingMode::eExclusive);
  m_swapchain = device.createSwapchainKHR(swapchainCreateInfo);
  // get swapchain images and image views
  m_images = device.getSwapchainImagesKHR(m_swapchain);
  m_imageViews.resize(m_images.size());
  assert(m_images.empty() == false);

  for (std::size_t i = 0; i < m_images.size(); i++) {
    auto componets =
        vk::ComponentMapping(vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                             vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity);
    auto subresourceRange = vk::ImageSubresourceRange()
                                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                                .setBaseMipLevel(0)
                                .setLevelCount(1)
                                .setBaseArrayLayer(0)
                                .setLayerCount(1);
    auto imageViewCI = vk::ImageViewCreateInfo()
                           .setImage(m_images[i])
                           .setViewType(vk::ImageViewType::e2D)
                           .setFormat(m_surfaceFormat.format)
                           .setComponents(componets)
                           .setSubresourceRange(subresourceRange);
    m_imageViews[i] = device.createImageViewUnique(imageViewCI);
  }
}

vk::Extent2D SwapChain::ChooseExtent(const vk::Extent2D& requiredExtent,
                                     const vk::Extent2D& minExtent, const vk::Extent2D& maxExtent) {
  vk::Extent2D extent{};
  extent.width  = std::clamp(requiredExtent.width, minExtent.width, maxExtent.width);
  extent.height = std::clamp(requiredExtent.height, minExtent.height, maxExtent.height);
  return extent;
}

vk::SurfaceFormatKHR SwapChain::ChooseFormat(
    const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
  static const std::vector<vk::SurfaceFormatKHR> bestFormats{
      {vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
      {vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}};
  vk::SurfaceFormatKHR chosenFormat{};
  for (const auto available : availableFormats) {
    auto format = std::find_if(bestFormats.begin(), bestFormats.end(),
                               [&](const vk::SurfaceFormatKHR candidate) {
                                 return available.format == candidate.format &&
                                        available.colorSpace == candidate.colorSpace;
                               });

    if (format != bestFormats.end()) {
      chosenFormat = *format;
    }
  }
  return chosenFormat;
}

vk::PresentModeKHR SwapChain::ChoosePresentMode(
    const std::vector<vk::PresentModeKHR>& availablePresentModes, bool enableVsync) {
  static const std::vector<vk::PresentModeKHR> presentModePriorities{
      vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eFifoRelaxed, vk::PresentModeKHR::eFifo};
  if (!enableVsync) {
    for (const auto requested : presentModePriorities) {
      const auto iter =
          std::find(availablePresentModes.begin(), availablePresentModes.end(), requested);
      if (iter != availablePresentModes.end()) {
        return *iter;
      }
    }
  }
  return vk::PresentModeKHR::eFifo;
}

SwapChain::~SwapChain() {
  if (m_swapchain) {
    m_context.GetLogicalDevice().destroySwapchainKHR(m_swapchain);
  }
  if (m_surface) {
    m_context.GetInstance().destroySurfaceKHR(m_surface);
  }
}

}  // namespace zen::vulkan