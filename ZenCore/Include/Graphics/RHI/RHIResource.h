#pragma once
#include <atomic>
#include "Common/RefCountPtr.h"
#include "RHICommon.h"
#include "Common/Errors.h"
#include "Common/BitField.h"

namespace zen::rhi
{
enum class ResourceType
{
    eNone,
    eViewport,
    eMax
};

class RHIResource
{
public:
    RHIResource() = default;

    virtual ~RHIResource()
    {
        VERIFY_EXPR(m_counter.GetValue() == 0);
    }

    explicit RHIResource(ResourceType resourceType) : m_resourceType(resourceType) {}

    uint32_t AddRef()
    {
        uint32_t newValue = m_counter.AddRef();
        return newValue;
    }

    uint32_t Release()
    {
        uint32_t newValue = m_counter.Release();
        if (newValue == 0)
        {
            delete this;
        }
        return newValue;
    }

    uint32_t GetRefCount() const
    {
        return m_counter.GetValue();
    }

private:
    class AtomicCounter
    {
    public:
        uint32_t AddRef()
        {
            uint32_t oldValue = m_count.fetch_add(1, std::memory_order_acquire);
            return oldValue + 1;
        }

        uint32_t Release()
        {
            uint32_t oldValue = m_count.fetch_sub(1, std::memory_order_release);
            return oldValue - 1;
        }

        uint32_t GetValue()
        {
            return m_count.load(std::memory_order_relaxed);
        }

    private:
        std::atomic_uint m_count{0};
    };

    mutable AtomicCounter m_counter;
    ResourceType m_resourceType{ResourceType::eMax};
};

class ShaderGroupSource : public RHIResource
{
public:
    ShaderGroupSource() = default;

    void SetStageSource(ShaderStage stage, const std::string& source)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_source[ToUnderlying(stage)] = source;
    }

    const std::string& GetStageSource(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_source[ToUnderlying(stage)];
    }

private:
    ShaderLanguage m_shaderLanguage{ShaderLanguage::eGLSL};
    std::string m_source[ToUnderlying(ShaderStage::eMax)];
};

class ShaderGroupSPIRV : public RHIResource
{
public:
    ShaderGroupSPIRV() = default;

    void SetStageSPIRV(ShaderStage stage, const std::vector<uint8_t>& source)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_stageFlags.SetFlag(static_cast<ShaderStageFlagBits>(1 << ToUnderlying(stage)));
        m_spirv[static_cast<uint32_t>(stage)] = source;
    }

    const std::vector<uint8_t>& GetStageSPIRV(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_spirv[ToUnderlying(stage)];
    }

    void SetStageCompileError(ShaderStage stage, const std::string& error)
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        m_compileErrors[ToUnderlying(stage)] = error;
    }

    const std::string& GetStageCompileError(ShaderStage stage) const
    {
        VERIFY_EXPR(stage < ShaderStage::eMax);
        return m_compileErrors[ToUnderlying(stage)];
    }

    bool HasShaderStage(ShaderStage stage) const
    {
        return m_stageFlags.HasFlag(static_cast<ShaderStageFlagBits>(1 << ToUnderlying(stage)));
    }

private:
    // shader flags
    BitField<ShaderStageFlagBits> m_stageFlags;
    // shader language
    ShaderLanguage m_shaderLanguage{ShaderLanguage::eGLSL};
    // spirv code
    std::vector<uint8_t> m_spirv[ToUnderlying(ShaderStage::eMax)];
    // compile errors
    std::string m_compileErrors[ToUnderlying(ShaderStage::eMax)];
};

using ShaderGroupSourcePtr = RefCountPtr<ShaderGroupSource>;
using ShaderGroupSPIRVPtr  = RefCountPtr<ShaderGroupSPIRV>;

/*****************************/
/********** Sampler **********/
/*****************************/
struct SamplerInfo
{
    SamplerFilter magFilter{SamplerFilter::eNearest};
    SamplerFilter minFilter{SamplerFilter::eNearest};
    SamplerFilter mipFilter{SamplerFilter::eNearest};
    SamplerRepeatMode repeatU{SamplerRepeatMode::eClampToEdge};
    SamplerRepeatMode repeatV{SamplerRepeatMode::eClampToEdge};
    SamplerRepeatMode repeatW{SamplerRepeatMode::eClampToEdge};
    float lodBias{0.0f};
    bool useAnisotropy{false};
    float maxAnisotropy{1.0f};
    bool enableCompare{false};
    CompareOperator compareOp{CompareOperator::eAlways};
    float minLod{0.0f};
    float maxLod{1e20}; // Something very large should do.
    SamplerBorderColor borderColor{SamplerBorderColor::eFloatOpaqueBlack};
    bool unnormalizedUVW{false};
};

/*****************************/
/********* Textures **********/
/*****************************/
struct TextureInfo
{
    DataFormat format{DataFormat::eUndefined};
    SampleCount samples{SampleCount::e1};
    BitField<TextureUsageFlagBits> usageFlags;
    TextureType type{TextureType::e1D};
    uint32_t width{1};
    uint32_t height{1};
    uint32_t depth{1};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
    // memory flags
    bool cpuReadable{false};
    bool mutableFormat{false};
    std::string name;
};

struct TextureProxyInfo
{
    DataFormat format{DataFormat::eUndefined};
    TextureType type{TextureType::e1D};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
    std::string name;
};

#define INIT_TEXTURE_INFO(info, type_, format_, width_, height_, depth_, mipmaps_, arrayLayers_, \
                          samples_, name_, ...)                                                  \
    rhi::TextureInfo info{};                                                                     \
    info.type        = type_;                                                                    \
    info.format      = format_;                                                                  \
    info.width       = width_;                                                                   \
    info.height      = height_;                                                                  \
    info.depth       = depth_;                                                                   \
    info.mipmaps     = mipmaps_;                                                                 \
    info.arrayLayers = arrayLayers_;                                                             \
    info.samples     = SampleCount::e1;                                                          \
    info.name        = name_;                                                                    \
    info.usageFlags.SetFlags(__VA_ARGS__);

inline uint32_t CalculateTextureSize(const TextureInfo& info)
{
    // TODO: Support compressed texture format
    uint32_t pixelSize = GetTextureFormatPixelSize(info.format);

    uint32_t w = info.width;
    uint32_t h = info.height;
    uint32_t d = info.depth;

    uint32_t size = 0;
    for (uint32_t i = 0; i < info.mipmaps; i++)
    {
        uint32_t numPixels = w * h * d;
        size += numPixels * pixelSize;
        w >>= 1;
        h >>= 1;
        d >>= 1;
    }
    return size;
}

inline uint32_t CalculateTextureMipLevels(uint32_t dim)
{
    return static_cast<uint32_t>(floor(log2(dim)) + 1);
}


inline uint32_t CalculateTextureMipLevels(uint32_t width, uint32_t height)
{
    return static_cast<uint32_t>(floor(log2(std::max(width, height))) + 1);
}

inline uint32_t CalculateTextureMipLevels(uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t maxDim = std::max(std::max(width, height), depth);
    return static_cast<uint32_t>(floor(log2(maxDim)) + 1);
}

class RHIViewport : public RHIResource
{
public:
    RHIViewport() : RHIResource(ResourceType::eViewport) {}

    virtual ~RHIViewport() = default;

    virtual uint32_t GetWidth() const = 0;

    virtual uint32_t GetHeight() const = 0;

    virtual void WaitForFrameCompletion() = 0;

    virtual void IssueFrameEvent() = 0;

    virtual DataFormat GetSwapchainFormat() = 0;

    virtual DataFormat GetDepthStencilFormat() = 0;

    virtual TextureHandle GetColorBackBuffer() = 0;

    virtual TextureHandle GetDepthStencilBackBuffer() = 0;

    virtual FramebufferHandle GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
                                                       const FramebufferInfo* fbInfo) = 0;

    virtual FramebufferHandle GetCompatibleFramebufferForBackBuffer(
        RenderPassHandle renderPassHandle) = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;
};
} // namespace zen::rhi