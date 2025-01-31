#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
struct GraphicsPipeline
{
    rhi::FramebufferHandle framebuffer;
    rhi::RenderPassHandle renderPass;
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
};
} // namespace zen::rc