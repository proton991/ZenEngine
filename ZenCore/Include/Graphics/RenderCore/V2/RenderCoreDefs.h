#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
struct GraphicsPass
{
    rhi::FramebufferHandle framebuffer;
    rhi::RenderPassHandle renderPass;
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
    std::vector<rhi::TextureHandle> rtHandles;
    uint32_t framebufferWidth;
    uint32_t framebufferHeight;
};
} // namespace zen::rc