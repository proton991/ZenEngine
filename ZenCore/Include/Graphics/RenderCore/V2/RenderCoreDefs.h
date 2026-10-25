#pragma once
#include "Graphics/RHI/DynamicRHI.h"

#define ADD_SHADER_BINDING_SINGLE(bindings_, index_, type_, ...)           \
    {                                                                      \
        RHIShaderResourceBinding binding{};                                \
        binding.binding = index_;                                          \
        binding.type    = type_;                                           \
        {                                                                  \
            std::initializer_list<RHIResource*> resources = {__VA_ARGS__}; \
            for (RHIResource* resource : resources)                        \
            {                                                              \
                binding.resources.push_back(resource);                     \
            }                                                              \
        }                                                                  \
        bindings_.emplace_back(std::move(binding));                        \
    }

#define ADD_SHADER_BINDING_TEXTURE_ARRAY(bindings_, index_, type_, sampler_, textures_) \
    {                                                                                   \
        RHIShaderResourceBinding binding{};                                             \
        binding.binding = index_;                                                       \
        binding.type    = type_;                                                        \
        for (RHITexture* texture : (textures_))                                         \
        {                                                                               \
            binding.resources.push_back(sampler_);                                      \
            binding.resources.push_back(texture);                                       \
        }                                                                               \
        bindings_.emplace_back(std::move(binding));                                     \
    }

namespace zen::rc
{
class ShaderProgram;

enum class RenderFrameResult : uint8_t
{
    eSubmitted, // Accepted inline, or handed off for threaded submission.
    eDeclined,  // No frame work accepted; retry after NextFrame.
    eFailed,    // Declaration, compilation or recording failed; rebuild the graph.
    eBlocked    // Submission failure requires device teardown.
};

// Application loops retry declined frames, but not indefinitely. Updates the caller's
// count of consecutive declines; false once rendering should stop.
inline bool TrackDeclinedFrames(RenderFrameResult result, uint32_t& consecutiveDeclines)
{
    constexpr uint32_t kMaxConsecutiveDeclines = 8;

    consecutiveDeclines                        = result == RenderFrameResult::eDeclined ? consecutiveDeclines + 1 : 0;

    if (consecutiveDeclines > kMaxConsecutiveDeclines)
    {
        LOGE("Rendering stopped after {} consecutive declined frames", consecutiveDeclines);
    }

    return consecutiveDeclines <= kMaxConsecutiveDeclines;
}

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
    DataFormat       format{DataFormat::eUndefined};
    TextureDimension dimension{TextureDimension::e2D};
    SampleCount      sampleCount{SampleCount::e1};
    uint32_t         width{0};
    uint32_t         height{0};
    uint32_t         depth{0};
    uint32_t         arrayLayers{1};
    uint32_t         mipmaps{1};
    bool             mutableFormat{false};
};

struct TextureViewFormat
{
    DataFormat       format{DataFormat::eUndefined};
    TextureDimension dimension{TextureDimension::e1D};
    uint32_t         arrayLayers{1};
    uint32_t         mipmaps{1};
    uint32_t         baseMipLevel{0};
    uint32_t         baseArrayLayer{0};
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
    int      vertexOffset;
    uint32_t firstInstance;
};

struct GraphicsPass
{
    // FramebufferHandle framebuffer;
    // RenderPassHandle renderPass;
    RHIPipeline* pPipeline;

    uint32_t       numDescriptorSets{0};
    ShaderProgram* pShaderProgram;

    RHIRenderingLayout* pRenderingLayout{nullptr};

    void Reset()
    {
        pPipeline         = nullptr;
        numDescriptorSets = 0;
        pShaderProgram    = nullptr;
        pRenderingLayout  = nullptr;
    }
};

struct ComputePass
{
    RHIPipeline* pPipeline;

    uint32_t       numDescriptorSets{0};
    ShaderProgram* pShaderProgram;
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
    bool        authoredCubemaps{false};
    RHITexture* pSkybox{nullptr};
    RHITexture* pIrradiance{nullptr};
    RHITexture* pPrefiltered{nullptr};
    RHITexture* pLutBRDF{nullptr};
    RHISampler* pIrradianceSampler{nullptr};
    RHISampler* pPrefilteredSampler{nullptr};
    RHISampler* pLutBRDFSampler{nullptr};
    NameID      tag;

    bool IsComplete() const
    {
        bool valid = pIrradianceSampler != nullptr && pPrefilteredSampler != nullptr && pLutBRDFSampler != nullptr;

        for (RHITexture* texture : {pSkybox, pIrradiance, pPrefiltered, pLutBRDF})
        {
            valid = valid && texture != nullptr && texture->GetDefaultView() != nullptr;
        }

        return valid;
    }
};
} // namespace zen::rc
