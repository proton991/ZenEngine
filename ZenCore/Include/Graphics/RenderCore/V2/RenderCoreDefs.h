#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
struct PushConstantNode
{
    uint32_t nodeIndex;
    uint32_t materialIndex;
};

struct GraphicsPass
{
    rhi::FramebufferHandle framebuffer;
    rhi::RenderPassHandle renderPass;
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
};

struct EnvTexture
{
    rhi::TextureHandle skybox;
    rhi::TextureHandle irradiance;
    rhi::TextureHandle prefiltered;
    rhi::TextureHandle lutBRDF;
    rhi::SamplerHandle irradianceSampler;
    rhi::SamplerHandle prefilteredSampler;
    rhi::SamplerHandle lutBRDFSampler;
    std::string tag;
};
} // namespace zen::rc