#include "Graphics/Utils/ResourceCache.h"

namespace zen::vulkan {
DescriptorSetLayout& ResourceCache::RequestDescriptorSetLayout(
    uint32_t setIndex, const std::vector<ShaderModule*>& shaderModules,
    const std::vector<ShaderResource>& shaderResources) {
  return RequestResource(m_device, m_mutexTable.descriptorSetLayout,
                         m_resourceTable.descriptorSetLayouts, setIndex, shaderModules,
                         shaderResources);
}
}  // namespace zen::vulkan