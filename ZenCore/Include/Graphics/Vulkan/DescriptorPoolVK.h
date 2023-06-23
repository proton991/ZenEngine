#pragma once
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include "Graphics/Vulkan/DeviceVK.h"
namespace zen::vulkan {
class Device;
class DescriptorPool {
public:
  DescriptorPool(const vulkan::Device& device, std::string poolName,
                 std::vector<vk::DescriptorPoolSize> poolSizes, uint32_t maxSets)
      : m_device(device),
        m_poolName(std::move(poolName)),
        m_poolSizes(std::move(poolSizes)),
        m_maxSets(maxSets) {}

  ~DescriptorPool();

  void Reset();

  vk::DescriptorSet Allocate(vk::DescriptorSetLayout setLayout, const char* debugName);

private:
  vk::DescriptorPool CreatePool(const char* debugName);
  const vulkan::Device& m_device;
  const std::string m_poolName;
  const std::vector<vk::DescriptorPoolSize> m_poolSizes;
  const uint32_t m_maxSets;
  std::mutex m_mutex;
  std::deque<vk::DescriptorPool> m_pools;
};
}  // namespace zen::vulkan