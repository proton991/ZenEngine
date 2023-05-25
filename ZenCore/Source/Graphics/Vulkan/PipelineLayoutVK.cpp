#include "Graphics/Vulkan/PipelineLayoutVK.h"
#include "Graphics/Utils/ResourceCache.h"
#include "Graphics/Vulkan/DescriptorSetLayoutVK.h"
#include "Graphics/Vulkan/ShaderModuleVK.h"

namespace zen::vulkan {
PipelineLayout::PipelineLayout(const Device& device,
                               const std::vector<ShaderModule*>& shaderModules)
    : DeviceResource<vk::PipelineLayout>(device, nullptr), m_shaderModules(shaderModules) {
  for (const auto& shaderModule : m_shaderModules) {
    for (const auto& shaderResource : shaderModule->GetResources()) {
      if (m_perSetResources.find(shaderResource.set) == m_perSetResources.end()) {
        m_perSetResources.emplace(shaderResource.set, std::vector<ShaderResource>{shaderResource});
      } else {
        m_perSetResources[shaderResource.set].emplace_back(shaderResource);
      }
      std::string key;
      if (shaderResource.type == ShaderResourceType::Input ||
          shaderResource.type == ShaderResourceType::Output) {
        key += to_string(shaderResource.stages) + "_" + key;
      } else {
        key = shaderResource.name;
      }
      auto it = m_shaderResourceTable.find(key);
      if (it == m_shaderResourceTable.end()) {
        m_shaderResourceTable.emplace(key, shaderResource);
      } else {
        it->second.stages |= shaderResource.stages;
      }
    }
  }
  std::vector<vk::DescriptorSetLayout> descriptorSetLayoutHandles;
  for (const auto& [set, resource] : m_perSetResources) {
    m_descriptorSetLayouts.emplace_back(
        &GetDevice().GetResourceCache()->RequestDescriptorSetLayout(set, shaderModules, resource));
    descriptorSetLayoutHandles.push_back(m_descriptorSetLayouts.back()->GetHandle());
  }
  // Collect all the push constant shader resources
  std::vector<vk::PushConstantRange> pushConstantRanges;
  for (auto& pushConstantResource : GetResources(ShaderResourceType::PushConstant)) {
    pushConstantRanges.emplace_back(pushConstantResource.stages, pushConstantResource.offset,
                                    pushConstantResource.size);
  }

  auto pipelineLayoutCI = vk::PipelineLayoutCreateInfo()
                              .setSetLayouts(descriptorSetLayoutHandles)
                              .setPushConstantRanges(pushConstantRanges);
  SetHandle(GetDeviceHandle().createPipelineLayout(pipelineLayoutCI));
}

std::vector<ShaderResource> PipelineLayout::GetResources(ShaderResourceType type,
                                                         vk::ShaderStageFlagBits stage) const {
  std::vector<ShaderResource> foundResources;

  for (auto& it : m_shaderResourceTable) {
    auto& shaderResource = it.second;

    if (shaderResource.type == type || type == ShaderResourceType::All) {
      if (shaderResource.stages == stage || stage == vk::ShaderStageFlagBits::eAll) {
        foundResources.push_back(shaderResource);
      }
    }
  }
  return foundResources;
}
PipelineLayout::PipelineLayout(PipelineLayout&& other)
    : DeviceResource<vk::PipelineLayout>(std::move(other)) {}

PipelineLayout::~PipelineLayout() {}

}  // namespace zen::vulkan