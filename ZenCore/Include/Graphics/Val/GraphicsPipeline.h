#pragma once
#include <vulkan/vulkan.h>
#include <string>

namespace zen::val
{
class PipelineState;
class PipelineLayout;
class Device;

class GraphicsPipeline
{
public:
    GraphicsPipeline(Device& device, const PipelineLayout& pipelineLayout, const PipelineState& pipelineState, const std::string& debugName, VkPipelineCache pipelineCache = VK_NULL_HANDLE);

    ~GraphicsPipeline();

    VkPipeline GetHandle() const { return m_handle; }

private:
    Device&    m_device;
    VkPipeline m_handle{VK_NULL_HANDLE};
};

} // namespace zen::val