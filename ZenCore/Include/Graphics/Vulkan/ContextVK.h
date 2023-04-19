#pragma once
#include <vector>
#include <vulkan/vulkan.hpp>

namespace zen::vulkan {
enum QueueIndices {
  QUEUE_INDEX_GRAPHICS,
  QUEUE_INDEX_COMPUTE,
  QUEUE_INDEX_TRANSFER,
  QUEUE_INDEX_VIDEO_DECODE,
  QUEUE_INDEX_COUNT
};
struct DeviceQueueInfo {
  DeviceQueueInfo() {
    for (auto i = 0; i < QUEUE_INDEX_COUNT; i++) {
      familyIndices[i] = VK_QUEUE_FAMILY_IGNORED;
      indices[i]       = uint32_t(-1);
    }
  }
  uint32_t familyIndices[QUEUE_INDEX_COUNT];
  uint32_t indices[QUEUE_INDEX_COUNT];
  std::vector<uint32_t> queueOffsets;
  std::vector<std::vector<float>> queuePriorites;
};
class Context {
public:
  Context() = default;
  void SetupInstance(const char** extensions, uint32_t extensionsCount, const char** layers,
                     uint32_t layersCount);
  void SetupDevice(const char** extensions, uint32_t extensionsCount, vk::SurfaceKHR surface);

  auto GetInstance() const { return m_instance.get(); }
  auto GetGPU() const { return m_gpu; }
  auto GetDevice() const { return m_device.get(); }
  auto GetQueueFamliyIndex(QueueIndices index) const { return m_queueInfo.familyIndices[index]; }

private:
  static vk::UniqueInstance CreateInstance(const std::vector<const char*>& extensions,
                                           const std::vector<const char*>& layers);
  static vk::UniqueHandle<vk::DebugUtilsMessengerEXT, vk::DispatchLoaderDynamic>
  CreateDebugUtilsMessenger(vk::Instance instance,
                            PFN_vkDebugUtilsMessengerCallbackEXT debugCallback,
                            vk::DispatchLoaderDynamic& loader);
  static vk::PhysicalDevice SelectPhysicalDevice(vk::Instance instance);
  static DeviceQueueInfo GetDeviceQueueInfo(vk::PhysicalDevice gpu, vk::SurfaceKHR surface);
  static vk::UniqueDevice CreateDevice(vk::PhysicalDevice gpu, const DeviceQueueInfo& queueInfo,
                                       const std::vector<const char*>& deviceExtensions);

  vk::UniqueInstance m_instance;
  vk::DispatchLoaderDynamic m_loader;
  vk::UniqueHandle<vk::DebugUtilsMessengerEXT, vk::DispatchLoaderDynamic> m_debugUtilsMessenger;
  vk::PhysicalDevice m_gpu;
  DeviceQueueInfo m_queueInfo{};
  vk::UniqueDevice m_device;
  // Device Queues
  vk::Queue m_graphicsQueue;
  vk::Queue m_presentQueue;
  vk::Queue m_transferQueue;
};
}  // namespace zen::vulkan