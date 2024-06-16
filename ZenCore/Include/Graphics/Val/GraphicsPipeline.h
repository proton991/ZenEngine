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
    GraphicsPipeline(const Device& device,
                     const PipelineLayout& pipelineLayout,
                     PipelineState& pipelineState,
                     VkPipelineCache pipelineCache = VK_NULL_HANDLE);

    GraphicsPipeline(GraphicsPipeline&& other) noexcept;

    ~GraphicsPipeline();

private:
};

} // namespace zen::val