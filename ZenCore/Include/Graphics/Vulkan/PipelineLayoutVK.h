#pragma once
#include <unordered_map>
#include "DeviceResource.h"

namespace zen::vulkan {
class ShaderModule;
class DescriptorSetLayout;
enum class ShaderResourceType;
struct ShaderResource;
class PipelineLayout : public DeviceResource<vk::PipelineLayout> {
public:
  ZEN_MOVE_CONSTRUCTOR_ONLY(PipelineLayout)
  PipelineLayout(const Device& device, const std::vector<ShaderModule*>& shaderModules);
  PipelineLayout(PipelineLayout&& other);
  ~PipelineLayout();

  [[nodiscard]] auto GetShaderModules() const { return m_shaderModules; }

private:
  std::vector<ShaderResource> GetResources(
      ShaderResourceType type, vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eAll) const;
  std::vector<ShaderModule*> m_shaderModules;
  std::unordered_map<std::string, ShaderResource> m_shaderResourceTable;
  std::unordered_map<uint32_t, std::vector<ShaderResource>> m_perSetResources;
  std::vector<DescriptorSetLayout*> m_descriptorSetLayouts;
};

}  // namespace zen::vulkan