#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include "DeviceObject.h"

namespace zen::val
{
class PipelineState;
class PipelineLayout;

class GraphicsPipeline : public DeviceObject<VkPipeline, VK_OBJECT_TYPE_PIPELINE>
{
public:
    GraphicsPipeline(Device& device, const PipelineLayout& pipelineLayout, const PipelineState& pipelineState, const std::string& debugName, VkPipelineCache pipelineCache = VK_NULL_HANDLE);

    ~GraphicsPipeline();

private:
};

} // namespace zen::val