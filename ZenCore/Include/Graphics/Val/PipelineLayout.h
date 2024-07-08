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
    PipelineLayout(const Device& device, const std::vector<ShaderModule*>& shaderModules);

    PipelineLayout(PipelineLayout&& other) noexcept;

    ~PipelineLayout();

    auto& GetShaderModules() const
    {
        return m_shaderModules;
    }

    auto& GetDescriptorSetLayouts() const
    {
        return m_dsLayouts;
    }

    std::vector<ShaderResource> GetResources(
        ShaderResourceType type,
        VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL) const;

private:
    // The shader modules that this pipeline layout uses
    std::vector<ShaderModule*> m_shaderModules;
    // The shader resources
    HashMap<std::string, ShaderResource> m_shaderResources;
    // A map of each set and the resources it owns used by the pipeline layout
    HashMap<uint32_t, std::vector<ShaderResource>> m_perSetResource;

    std::vector<DescriptorSetLayout> m_dsLayouts;
};

class PipelineLayoutCache
{};
} // namespace zen::val