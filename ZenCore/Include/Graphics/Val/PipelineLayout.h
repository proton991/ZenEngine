#pragma once
#include <vulkan/vulkan.h>
#include "Graphics/Val/Shader.h"
#include "DeviceObject.h"
namespace zen::val
{
class DescriptorSetLayout;

class PipelineLayout : public DeviceObject<VkPipelineLayout, VK_OBJECT_TYPE_PIPELINE_LAYOUT>
{
public:
    PipelineLayout(Device& device, const std::vector<ShaderModule*>& shaderModules);

    ~PipelineLayout();

    auto& GetShaderModules() const { return m_shaderModules; }

private:
    std::vector<ShaderResource> GetResources(ShaderResourceType type, VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL);
    // The shader modules that this pipeline layout uses
    std::vector<ShaderModule*> m_shaderModules;
    // The shader resources
    std::unordered_map<std::string, ShaderResource> m_shaderResources;
    // A map of each set and the resources it owns used by the pipeline layout
    std::unordered_map<uint32_t, std::vector<ShaderResource>> m_perSetResource;

    std::vector<DescriptorSetLayout> m_dsLayouts;
};

class PipelineLayoutCache
{
};
} // namespace zen::val