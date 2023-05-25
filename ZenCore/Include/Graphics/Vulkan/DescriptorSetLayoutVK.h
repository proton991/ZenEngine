#pragma once
#include <unordered_map>
#include "DeviceResource.h"
namespace zen::vulkan {
class ShaderModule;
class ShaderResource;
class DescriptorSetLayout : public DeviceResource<vk::DescriptorSetLayout> {
public:
  DescriptorSetLayout(const Device& device, uint32_t setIndex,
                      const std::vector<ShaderModule*>& shaderModule,
                      const std::vector<ShaderResource>& shaderResources);

private:
  bool ValidBindingFlags() const;
  uint32_t m_setIndex;
  std::vector<vk::DescriptorSetLayoutBinding> m_bindings;

  std::vector<vk::DescriptorBindingFlagsEXT> m_bindingFlags;

  std::unordered_map<uint32_t, vk::DescriptorSetLayoutBinding> m_bindingTable;

  std::unordered_map<uint32_t, vk::DescriptorBindingFlagsEXT> m_bindingFlagTable;

  std::unordered_map<std::string, uint32_t> m_nameToBinding;
};

}  // namespace zen::vulkan