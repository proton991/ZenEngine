#pragma once
#include "Graphics/RHI/DynamicRHI.h"

namespace zen::rc
{
class ShaderProgram;

struct SimpleVertex
{
    Vec4 position;
};

struct ComputeIndirectCommand
{
    uint32_t x;
    uint32_t y;
    uint32_t z;
};

struct DrawIndexedIndirectCommand
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int vertexOffset;
    uint32_t firstInstance;
};

enum class PassResourceType
{
    eBuffer  = 0,
    eTexture = 1,
    eMax     = 2
};

struct PassResourceTracker
{
    std::string name;
    rhi::TextureHandle textureHandle;
    rhi::BufferHandle bufferHandle;
    PassResourceType resourceType{PassResourceType::eMax};
    rhi::AccessMode accessMode{rhi::AccessMode::eNone};
    BitField<rhi::AccessFlagBits> accessFlags;
    rhi::BufferUsage bufferUsage{rhi::BufferUsage::eNone};
    rhi::TextureUsage textureUsage{rhi::TextureUsage::eNone};
    rhi::TextureSubResourceRange textureSubResRange;
};

struct GraphicsPass
{
    rhi::FramebufferHandle framebuffer;
    rhi::RenderPassHandle renderPass;
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
    ShaderProgram* shaderProgram;
    rhi::RenderPassLayout renderPassLayout;
    // setIndex as vector index, bindingIndex as inner map key
    // resource trackers are used by rc::RenderGraph for resolving pass node dependencies
    std::vector<HashMap<uint32_t, PassResourceTracker>> resourceTrackers;
};

struct ComputePass
{
    rhi::PipelineHandle pipeline;
    std::vector<rhi::DescriptorSetHandle> descriptorSets;
    ShaderProgram* shaderProgram;
    // setIndex as vector index, bindingIndex as inner map key
    // resource trackers are used by rc::RenderGraph for resolving pass node dependencies
    std::vector<HashMap<uint32_t, PassResourceTracker>> resourceTrackers;
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