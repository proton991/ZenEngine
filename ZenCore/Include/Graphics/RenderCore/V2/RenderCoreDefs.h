#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
class ShaderProgram;
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
    ShaderProgram* shaderProgram;
    rhi::RenderPassLayout renderPassLayout;
};

enum class GfxPassShaderMode : uint32_t
{
    ePreCompiled = 0,
    eRuntime     = 1,
    eMax         = 2
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