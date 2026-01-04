#pragma once
#include "Graphics/RHI/DynamicRHI.h"

#define ADD_SHADER_BINDING_SINGLE(bindings_, index_, type_, ...)                                   \
    {                                                                                              \
        RHIShaderResourceBinding binding{};                                                        \
        binding.binding = index_;                                                                  \
        binding.type    = type_;                                                                   \
        {                                                                                          \
            std::initializer_list<RHIResource*> resources = {__VA_ARGS__};                         \
            binding.resources.insert(binding.resources.end(), resources.begin(), resources.end()); \
        }                                                                                          \
        bindings_.emplace_back(std::move(binding));                                                \
    }

#define ADD_SHADER_BINDING_TEXTURE_ARRAY(bindings_, index_, type_, sampler_, textures_) \
    {                                                                                   \
        RHIShaderResourceBinding binding{};                                             \
        binding.binding = index_;                                                       \
        binding.type    = type_;                                                        \
        for (RHITexture * texture : (textures_))                                        \
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
    std::vector<RHITexture*> textures;
    // TextureHandle textureHandle;
    RHIBuffer* buffer;
    PassResourceType resourceType{PassResourceType::eMax};
    RHIAccessMode accessMode{RHIAccessMode::eNone};
    BitField<RHIAccessFlagBits> accessFlags;
    RHIBufferUsage bufferUsage{RHIBufferUsage::eNone};
    RHITextureUsage textureUsage{RHITextureUsage::eNone};
    // RHITextureSubResourceRange textureSubResRange;
};

struct GraphicsPass
{
    // FramebufferHandle framebuffer;
    // RenderPassHandle renderPass;
    RHIPipeline* pipeline;
    RHIDescriptorSet* descriptorSets[MAX_NUM_DESCRIPTOR_SETS];
    uint32_t numDescriptorSets{0};
    ShaderProgram* shaderProgram;
    // RHIRenderPassLayout renderPassLayout;
    RHIRenderingLayout* pRenderingLayout{nullptr};
    // setIndex as vector index, bindingIndex as inner map key
    // resource trackers are used by rc::RenderGraph for resolving pass node dependencies
    HashMap<uint32_t, PassResourceTracker> resourceTrackers[MAX_NUM_DESCRIPTOR_SETS];
};

struct ComputePass
{
    RHIPipeline* pipeline;
    RHIDescriptorSet* descriptorSets[MAX_NUM_DESCRIPTOR_SETS];
    uint32_t numDescriptorSets{0};
    ShaderProgram* shaderProgram;
    // setIndex as vector index, bindingIndex as inner map key
    // resource trackers are used by rc::RenderGraph for resolving pass node dependencies
    HashMap<uint32_t, PassResourceTracker> resourceTrackers[MAX_NUM_DESCRIPTOR_SETS];
};

enum class GfxPassShaderMode : uint32_t
{
    eNone        = 0,
    ePreCompiled = 1,
    eRuntime     = 2,
    eMax         = 3
};

struct EnvTexture
{
    RHITexture* skybox;
    RHITexture* irradiance;
    RHITexture* prefiltered;
    RHITexture* lutBRDF;
    RHISampler* irradianceSampler;
    RHISampler* prefilteredSampler;
    RHISampler* lutBRDFSampler;
    std::string tag;
};
} // namespace zen::rc