#pragma once
#include "Graphics/RHI/DynamicRHI.h"

#define ADD_SHADER_BINDING_SINGLE(bindings_, index_, type_, ...)                                   \
    {                                                                                              \
        rhi::ShaderResourceBinding binding{};                                                      \
        binding.binding = index_;                                                                  \
        binding.type    = type_;                                                                   \
        {                                                                                          \
            std::initializer_list<rhi::RHIResource*> resources = {__VA_ARGS__};                    \
            binding.resources.insert(binding.resources.end(), resources.begin(), resources.end()); \
        }                                                                                          \
        bindings_.emplace_back(std::move(binding));                                                \
    }

#define ADD_SHADER_BINDING_TEXTURE_ARRAY(bindings_, index_, type_, sampler_, textures_) \
    {                                                                                   \
        rhi::ShaderResourceBinding binding{};                                           \
        binding.binding = index_;                                                       \
        binding.type    = type_;                                                        \
        for (rhi::RHITexture * texture : (textures_))                                   \
        {                                                                               \
            binding.resources.push_back(sampler_);                                      \
            binding.resources.push_back(texture);                                       \
        }                                                                               \
        bindings_.emplace_back(std::move(binding));                                     \
    }

namespace zen::rc
{
class ShaderProgram;

enum class TextureDimension : uint32_t
{
    e1D   = 0,
    e2D   = 1,
    e3D   = 2,
    eCube = 3,
    eMax  = 4
};

struct TextureUsageHint
{
    bool copyUsage : 1;
};

struct TextureFormat
{
    DataFormat format{DataFormat::eUndefined};
    TextureDimension dimension{TextureDimension::e2D};
    SampleCount sampleCount{SampleCount::e1};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{0};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
    bool mutableFormat{false};
};

struct TextureProxyFormat
{
    DataFormat format{DataFormat::eUndefined};
    TextureDimension dimension{TextureDimension::e1D};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
};

struct TextureSlice
{
    uint32_t baseMipLevel{0};
    uint32_t levelCount{1};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{1};
};

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
    std::vector<rhi::RHITexture*> textures;
    // rhi::TextureHandle textureHandle;
    rhi::RHIBuffer* buffer;
    PassResourceType resourceType{PassResourceType::eMax};
    rhi::AccessMode accessMode{rhi::AccessMode::eNone};
    BitField<rhi::AccessFlagBits> accessFlags;
    rhi::BufferUsage bufferUsage{rhi::BufferUsage::eNone};
    rhi::TextureUsage textureUsage{rhi::TextureUsage::eNone};
    // rhi::TextureSubResourceRange textureSubResRange;
};

struct GraphicsPass
{
    rhi::FramebufferHandle framebuffer;
    rhi::RenderPassHandle renderPass;
    rhi::RHIPipeline* pipeline;
    std::vector<rhi::RHIDescriptorSet*> descriptorSets;
    ShaderProgram* shaderProgram;
    rhi::RenderPassLayout renderPassLayout;
    // setIndex as vector index, bindingIndex as inner map key
    // resource trackers are used by rc::RenderGraph for resolving pass node dependencies
    std::vector<HashMap<uint32_t, PassResourceTracker>> resourceTrackers;
};

struct ComputePass
{
    rhi::RHIPipeline* pipeline;
    std::vector<rhi::RHIDescriptorSet*> descriptorSets;
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
    rhi::RHITexture* skybox;
    rhi::RHITexture* irradiance;
    rhi::RHITexture* prefiltered;
    rhi::RHITexture* lutBRDF;
    rhi::RHISampler* irradianceSampler;
    rhi::RHISampler* prefilteredSampler;
    rhi::RHISampler* lutBRDFSampler;
    std::string tag;
};
} // namespace zen::rc