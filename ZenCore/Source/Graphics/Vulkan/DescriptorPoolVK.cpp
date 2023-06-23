#include "Graphics/Vulkan/DescriptorPoolVK.h"
#include "Common/Error.h"
#include "Common/Logging.h"

namespace zen::vulkan {
vk::DescriptorPool DescriptorPool::CreatePool(const char* debugName) {
  auto poolCI = vk::DescriptorPoolCreateInfo().setPoolSizes(m_poolSizes).setMaxSets(m_maxSets);
  auto pool   = m_device.GetHandle().createDescriptorPool(poolCI);
  m_device.SetDebugObjName(pool, debugName);
  return pool;
}

DescriptorPool::~DescriptorPool() {
  for (auto& pool : m_pools) {
    m_device.GetHandle().destroyDescriptorPool(pool);
  }
}

void DescriptorPool::Reset() {
  for (auto& pool : m_pools) {
    m_device.GetHandle().resetDescriptorPool(pool);
  }
}

vk::DescriptorSet DescriptorPool::Allocate(vk::DescriptorSetLayout setLayout,
                                           const char* debugName) {
  std::lock_guard<std::mutex> lock(m_mutex);
  vk::DescriptorSet handle{VK_NULL_HANDLE};
  for (auto it = m_pools.begin(); it != m_pools.end(); ++it) {
    auto allocInfo = vk::DescriptorSetAllocateInfo()
                         .setDescriptorPool(*it)
                         .setSetLayouts(setLayout)
                         .setDescriptorSetCount(1);

    auto result = m_device.GetHandle().allocateDescriptorSets(&allocInfo, &handle);
    if (result != vk::Result::eSuccess) {
      return nullptr;
    }
    if (handle) {
      if (it != m_pools.begin()) {
        std::swap(*it, m_pools.front());
      }
      return handle;
    }
  }
  // Failed to allocate descriptor from existing pools -> create a new one
  LOGI("Creating new descriptor pool");
  m_pools.emplace_front(CreatePool("DescriptorPool"));
  auto& newPool  = m_pools.front();
  auto allocInfo = vk::DescriptorSetAllocateInfo()
                       .setDescriptorPool(newPool)
                       .setSetLayouts(setLayout)
                       .setDescriptorSetCount(1);

  VK_CHECK(m_device.GetHandle().allocateDescriptorSets(&allocInfo, &handle),
           "allocateDescriptorSets");
  VK_ASSERT(handle);
  return handle;
}
}  // namespace zen::vulkan