#pragma once

#include "Common/Helpers.h"
#include "Graphics/Vulkan/DescriptorSetLayoutVK.h"
#include "Graphics/Vulkan/ShaderModuleVK.h"

using namespace zen;
namespace std {
template <>
struct hash<vulkan::DescriptorSetLayout> {
  std::size_t operator()(const vulkan::DescriptorSetLayout& descriptorSetLayout) const {
    std::size_t result = 0;
    HashCombine(result, descriptorSetLayout.GetCHandle());
    return result;
  }
};

template <>
struct hash<vulkan::ShaderResource> {
  std::size_t operator()(const vulkan::ShaderResource& shaderResource) const {
    std::size_t result = 0;

    if (shaderResource.type == vulkan::ShaderResourceType::Input ||
        shaderResource.type == vulkan::ShaderResourceType::Output ||
        shaderResource.type == vulkan::ShaderResourceType::PushConstant ||
        shaderResource.type == vulkan::ShaderResourceType::SpecializationConstant) {
      return result;
    }

    HashCombine(result, shaderResource.set);
    HashCombine(result, shaderResource.binding);
    HashCombine(result, static_cast<std::underlying_type<vulkan::ShaderResourceType>::type>(
                            shaderResource.type));
    HashCombine(result, shaderResource.mode);

    return result;
  }
};

template <>
struct hash<vulkan::ShaderModule> {
  std::size_t operator()(const vulkan::ShaderModule& shaderModule) const {
    std::size_t result = 0;

    HashCombine(result, shaderModule.GetId());

    return result;
  }
};
}  // namespace std
