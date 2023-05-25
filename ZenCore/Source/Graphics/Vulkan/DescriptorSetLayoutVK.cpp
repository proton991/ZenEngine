#include "Graphics/Vulkan/DescriptorSetLayoutVK.h"
#include "Common/Logging.h"
#include "Graphics/Vulkan/ShaderModuleVK.h"

namespace zen::vulkan {
inline vk::DescriptorType FindDescriptorType(ShaderResourceType resourceType, bool dynamic) {
  switch (resourceType) {
    case ShaderResourceType::InputAttachment:
      return vk::DescriptorType::eInputAttachment;
      break;
    case ShaderResourceType::Image:
      return vk::DescriptorType::eSampledImage;
      break;
    case ShaderResourceType::ImageSampler:
      return vk::DescriptorType::eCombinedImageSampler;
      break;
    case ShaderResourceType::ImageStorage:
      return vk::DescriptorType::eStorageImage;
      break;
    case ShaderResourceType::Sampler:
      return vk::DescriptorType::eSampler;
      break;
    case ShaderResourceType::BufferUniform:
      if (dynamic) {
        return vk::DescriptorType::eUniformBufferDynamic;
      } else {
        return vk::DescriptorType::eUniformBuffer;
      }
      break;
    case ShaderResourceType::BufferStorage:
      if (dynamic) {
        return vk::DescriptorType::eStorageBufferDynamic;
      } else {
        return vk::DescriptorType::eStorageBuffer;
      }
      break;
    default:
      throw std::runtime_error("No conversion possible for the shader resource type.");
      break;
  }
}

bool DescriptorSetLayout::ValidBindingFlags() const {
  if (m_bindingFlags.empty()) {
    return true;
  }
  if (m_bindings.size() != m_bindingFlags.size()) {
    LOGE("Binding count has to be equal to flag count.");
    return false;
  }
  return true;
}

DescriptorSetLayout::DescriptorSetLayout(const Device& device, uint32_t setIndex,
                                         const std::vector<ShaderModule*>& shaderModule,
                                         const std::vector<ShaderResource>& shaderResources)
    : DeviceResource<vk::DescriptorSetLayout>(device, nullptr), m_setIndex(setIndex) {
  for (const auto& resource : shaderResources) {
    // Skip shader resources without a binding point
    if (resource.type == ShaderResourceType::Input ||         //
        resource.type == ShaderResourceType::Output ||        //
        resource.type == ShaderResourceType::PushConstant ||  //
        resource.type == ShaderResourceType::SpecializationConstant) {
      continue;
    }
    if (resource.mode == ShaderResourceMode::UpdateAfterBind) {
      m_bindingFlags.push_back(vk::DescriptorBindingFlagBits::eUpdateAfterBind);
    } else {
      m_bindingFlags.push_back(static_cast<vk::DescriptorBindingFlags>(0));
    }
    // Convert from ShaderResourceType to vk::DescriptorType.
    auto descriptorType =
        FindDescriptorType(resource.type, resource.mode == ShaderResourceMode::Dynamic);
    auto layoutBinding = vk::DescriptorSetLayoutBinding()
                             .setBinding(resource.binding)
                             .setDescriptorCount(resource.arraySize)
                             .setDescriptorType(descriptorType)
                             .setStageFlags(resource.stages);
    m_bindings.push_back(layoutBinding);
    m_bindingTable.emplace(resource.binding, layoutBinding);
    m_bindingFlagTable.emplace(resource.binding, m_bindingFlags.back());
    m_nameToBinding.emplace(resource.name, resource.binding);
  }
  auto createFlags = static_cast<vk::DescriptorSetLayoutCreateFlags>(0);

  if (std::find_if(shaderResources.begin(), shaderResources.end(),
                   [](const ShaderResource& resource) {
                     return resource.mode == ShaderResourceMode::UpdateAfterBind;
                   }) != shaderResources.end()) {
    if (std::find_if(shaderResources.begin(), shaderResources.end(),
                     [](const ShaderResource& resource) {
                       return resource.mode == ShaderResourceMode::Dynamic;
                     }) != shaderResources.end()) {
      throw std::runtime_error(
          "Cannot create descriptor set layout, dynamic resources are not allowed if at least one "
          "resource is update-after-bind.");
    }
    createFlags = vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool;
  }
  if (!ValidBindingFlags()) {
    throw std::runtime_error(
        "Invalid binding and binding flags, couldn't create descriptor set layout.");
  }
  auto descriptorSetLayoutCI =
      vk::DescriptorSetLayoutCreateInfo().setBindings(m_bindings).setFlags(createFlags);
  SetHandle(GetDeviceHandle().createDescriptorSetLayout(descriptorSetLayoutCI));
}
}  // namespace zen::vulkan