#pragma once
#include "Common/Base.h"
#include "DeviceResource.h"

namespace zen::vulkan {
class PipelineState;
class PipelineLayout;
class Pipeline : public DeviceResource<vk::Pipeline> {
public:
  explicit Pipeline(const Device& device);
};

class GraphicsPipeline : public Pipeline {
public:
  GraphicsPipeline(const Device& device, vk::PipelineCache pipelineCache,
                   const PipelineState& pipelineState, const PipelineLayout* pipelineLayout);
};
}  // namespace zen::vulkan