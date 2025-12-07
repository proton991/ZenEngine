#pragma once
#include <atomic>
#include "Utils/RefCountPtr.h"
#include "RHICommon.h"
#include "Utils/Errors.h"

namespace zen::rhi
{
enum class ResourceType : uint32_t
{
    eNone          = 0,
    eViewport      = 1,
    eBuffer        = 2,
    eTexture       = 3,
    eSampler       = 4,
    eShader        = 5,
    ePipeline      = 6,
    eDescriptorSet = 7,
    eMax           = 8
};

class RHIResource
{
public:
    RHIResource() = default;

    virtual ~RHIResource()
    {
        VERIFY_EXPR(m_counter.GetValue() == 0);
    }

    explicit RHIResource(ResourceType resourceType) : m_resourceType(resourceType)
    {
        AddReference();
    }

    uint32_t AddReference()
    {
        uint32_t newValue = m_counter.AddRef();
        return newValue;
    }

    uint32_t ReleaseReference()
    {
        uint32_t newValue = m_counter.Release();
        if (newValue == 0)
        {
            Destroy();
        }
        return newValue;
    }

    uint32_t GetRefCount() const
    {
        return m_counter.GetValue();
    }

    const std::string& GetResourceTag() const
    {
        return m_resourceTag;
    }

protected:
    virtual void Init() = 0;

    virtual void Destroy() = 0;

    std::string m_resourceTag;

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

class ShaderGroupSource : public RefCounted
{
public:
    ShaderGroupSource() = default;

    void SetStageSource(RHIShaderStage stage, const std::string& source)
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        m_source[ToUnderlying(stage)] = source;
    }

    const std::string& GetStageSource(RHIShaderStage stage) const
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        return m_source[ToUnderlying(stage)];
    }

private:
    ShaderLanguage m_shaderLanguage{ShaderLanguage::eGLSL};
    std::string m_source[ToUnderlying(RHIShaderStage::eMax)];
};

class ShaderGroupSPIRV : public RefCounted
{
public:
    ShaderGroupSPIRV() = default;

    void SetStageFlags(int64_t flags)
    {
        m_stageFlags = flags;
    }

    void SetStageSPIRV(RHIShaderStage stage, const std::vector<uint8_t>& source)
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        m_stageFlags.SetFlag(static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(stage)));
        m_spirv[static_cast<uint32_t>(stage)] = source;
    }

    std::vector<uint8_t> GetStageSPIRV(RHIShaderStage stage) const
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        return m_spirv[ToUnderlying(stage)];
    }

    void SetStageCompileError(RHIShaderStage stage, const std::string& error)
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        m_compileErrors[ToUnderlying(stage)] = error;
    }

    const std::string& GetStageCompileError(RHIShaderStage stage) const
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        return m_compileErrors[ToUnderlying(stage)];
    }

    bool HasShaderStage(RHIShaderStage stage) const
    {
        return m_stageFlags.HasFlag(static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(stage)));
    }

private:
    // shader flags
    BitField<RHIShaderStageFlagBits> m_stageFlags;
    // shader language
    ShaderLanguage m_shaderLanguage{ShaderLanguage::eGLSL};
    // spirv code
    std::vector<uint8_t> m_spirv[ToUnderlying(RHIShaderStage::eMax)];
    // compile errors
    std::string m_compileErrors[ToUnderlying(RHIShaderStage::eMax)];
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

class RHITexture;

class RHIViewport : public RHIResource
{
public:
    // static RHIViewport* Create(void* pWindow, uint32_t width, uint32_t height, bool enableVSync);

    // RHIViewport() : RHIResource(ResourceType::eViewport)
    // {
    //     AddReference();
    // }

    virtual ~RHIViewport() = default;

    virtual uint32_t GetWidth() const = 0;

    virtual uint32_t GetHeight() const = 0;

    virtual void WaitForFrameCompletion() = 0;

    virtual void IssueFrameEvent() = 0;

    virtual DataFormat GetSwapchainFormat() = 0;

    virtual DataFormat GetDepthStencilFormat() = 0;

    virtual RHITexture* GetColorBackBuffer() = 0;

    virtual TextureSubResourceRange GetColorBackBufferRange() = 0;

    virtual RHITexture* GetDepthStencilBackBuffer() = 0;

    virtual TextureSubResourceRange GetDepthStencilBackBufferRange() = 0;

    virtual FramebufferHandle GetCompatibleFramebuffer(RenderPassHandle renderPassHandle,
                                                       const FramebufferInfo* fbInfo) = 0;

    virtual FramebufferHandle GetCompatibleFramebufferForBackBuffer(
        RenderPassHandle renderPassHandle) = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;

protected:
    RHIViewport(void* pWindow, uint32_t width, uint32_t height, bool enableVSync) :
        RHIResource(ResourceType::eViewport),
        m_pWindow(pWindow),
        m_width(width),
        m_height(height),
        m_enableVSync(enableVSync)
    {}

    void* m_pWindow{nullptr};
    uint32_t m_width{0};
    uint32_t m_height{0};
    bool m_enableVSync{true};
};

struct RHIBufferCreateInfo
{
    uint32_t size{0};
    BitField<BufferUsageFlagBits> usageFlags{0};
    BufferAllocateType allocateType{BufferAllocateType::eNone};
    std::string tag;
};

class RHIBuffer : public RHIResource
{
public:
    // static RHIBuffer* Create(const RHIBufferCreateInfo& createInfo);

    ~RHIBuffer() {}

    virtual uint8_t* Map() = 0;

    virtual void Unmap() = 0;

    virtual void SetTexelFormat(DataFormat format) = 0;

    uint32_t GetRequiredSize() const
    {
        return m_requiredSize;
    }

protected:
    explicit RHIBuffer(const RHIBufferCreateInfo& createInfo) :
        RHIResource(ResourceType::eBuffer),
        m_requiredSize(createInfo.size),
        m_usageFlags(createInfo.usageFlags),
        m_allocateType(createInfo.allocateType)
    {
        m_resourceTag = createInfo.tag;
    }

    uint32_t m_requiredSize{0};
    BitField<BufferUsageFlagBits> m_usageFlags;
    BufferAllocateType m_allocateType{BufferAllocateType::eNone};
    // BufferHandle m_handle;
};

struct RHITextureCreateInfo
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
    std::string tag;
};

struct RHITextureProxyCreateInfo
{
    DataFormat format{DataFormat::eUndefined};
    TextureType type{TextureType::e1D};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};
    std::string tag;
};

inline uint32_t CalculateTextureSize(const RHITextureCreateInfo& info)
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

class RHITexture : public RHIResource
{
public:
    // static RHITexture* Create(const RHITextureCreateInfo& createInfo);

    RHITexture* CreateProxy(const RHITextureProxyCreateInfo& proxyInfo);

    DataFormat GetFormat() const
    {
        return m_baseInfo.format;
    }

    const RHITextureCreateInfo& GetBaseInfo() const
    {
        return m_baseInfo;
    }

    const TextureSubResourceRange& GetSubResourceRange() const
    {
        return m_subResourceRange;
    }

    uint32_t GetWidth() const
    {
        return m_baseInfo.width;
    }

    uint32_t GetHeight() const
    {
        return m_baseInfo.height;
    }

    uint32_t GetDepth() const
    {
        return m_baseInfo.depth;
    }

    // todo: impl Hash function
    uint32_t GetHash32() const
    {
        return 0;
    }

protected:
    explicit RHITexture(const RHITextureCreateInfo& createInfo) :
        RHIResource(ResourceType::eTexture), m_baseInfo(createInfo), m_isProxy(false)
    {
        m_resourceTag = createInfo.tag;
        InitSubresourceRange();
    }

    RHITexture(const RHITexture* pBaseTexture, const RHITextureProxyCreateInfo& proxyInfo) :
        RHIResource(ResourceType::eTexture),
        m_pBaseTexture(pBaseTexture),
        m_proxyInfo(proxyInfo),
        m_isProxy(true)
    {
        m_resourceTag = proxyInfo.tag;
        InitSubresourceRange();
    }

    void InitSubresourceRange()
    {
        if (FormatIsDepthOnly(m_baseInfo.format))
        {
            m_subResourceRange = TextureSubResourceRange::Depth();
        }
        else if (FormatIsStencilOnly(m_baseInfo.format))
        {
            m_subResourceRange = TextureSubResourceRange::Stencil();
        }
        else if (FormatIsDepthStencil(m_baseInfo.format))
        {
            m_subResourceRange = TextureSubResourceRange::DepthStencil();
        }
        else
        {
            m_subResourceRange = TextureSubResourceRange::Color();
        }
        m_subResourceRange.layerCount = m_baseInfo.arrayLayers;
        m_subResourceRange.levelCount = m_baseInfo.mipmaps;
    }

    const RHITexture* m_pBaseTexture{nullptr};

    RHITextureCreateInfo m_baseInfo{};

    RHITextureProxyCreateInfo m_proxyInfo{};

    bool m_isProxy{};

    TextureSubResourceRange m_subResourceRange{};
};

struct RHISamplerCreateInfo
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

class RHISampler : public RHIResource
{
public:
    // static RHISampler* Create(const RHISamplerCreateInfo& createInfo);
    ~RHISampler() {}

protected:
    explicit RHISampler(const RHISamplerCreateInfo& createInfo) :
        RHIResource(ResourceType::eSampler), m_baseInfo(createInfo)
    {}

    RHISamplerCreateInfo m_baseInfo{};
};

class RHIShader;

class RHIDescriptorSet : public RHIResource
{
public:
    ~RHIDescriptorSet() override = default;

    virtual void Update(const std::vector<ShaderResourceBinding>& resourceBindings) = 0;

protected:
    RHIDescriptorSet(const RHIShader* pShader, uint32_t setIndex) :
        RHIResource(ResourceType::eDescriptorSet), m_pShader(pShader), m_setIndex(setIndex)
    {}

    const RHIShader* m_pShader;
    uint32_t m_setIndex{0};
};

struct RHIShaderCreateInfo
{
    std::string spirvFileName[ToUnderlying(RHIShaderStage::eMax)];
    BitField<RHIShaderStageFlagBits> stageFlags;
    HashMap<uint32_t, int> specializationConstants;
    std::string name;
};

// Includes all stages used
class RHIShader : public RHIResource
{
public:
    ~RHIShader() override = default;

    virtual void GetShaderResourceDescriptorTable(ShaderResourceDescriptorTable& outSRDs)
    {
        outSRDs = m_SRDTable;
    }

    virtual RHIDescriptorSet* CreateDescriptorSet(uint32_t setIndex) = 0;

protected:
    explicit RHIShader(const RHIShaderCreateInfo& createInfo) :
        RHIResource(ResourceType::eShader),
        m_shaderGroupSPIRV(MakeRefCountPtr<ShaderGroupSPIRV>()),
        m_shaderStageFlags(createInfo.stageFlags),
        m_specializationConstants(createInfo.specializationConstants),
        m_name(createInfo.name)
    {

        std::ranges::copy(createInfo.spirvFileName, std::begin(m_spirvFileName));

        m_shaderGroupSPIRV->SetStageFlags(m_shaderStageFlags);
    }

    ShaderGroupSPIRVPtr m_shaderGroupSPIRV{};
    std::string m_spirvFileName[ToUnderlying(RHIShaderStage::eMax)];
    BitField<RHIShaderStageFlagBits> m_shaderStageFlags;
    HashMap<uint32_t, int> m_specializationConstants;
    ShaderResourceDescriptorTable m_SRDTable;
    std::string m_name;
};

struct RHIGfxPipelineCreateInfo
{
    RHIShader* shader;
    GfxPipelineStates states;
    RenderPassLayout renderPassLayout;
    RenderPassHandle renderPassHandle;
    uint32_t subpassIdx;
};

struct RHIComputePipelineCreateInfo
{
    RHIShader* shader;
};

enum class RHIPipelineType : uint32_t
{
    eNone     = 0,
    eGraphics = 1,
    eCompute  = 2,
    eMax      = 3
};

class RHIPipeline : public RHIResource
{
public:
    ~RHIPipeline() override = default;

protected:
    RHIPipeline(const RHIGfxPipelineCreateInfo& createInfo) :
        RHIResource(ResourceType::ePipeline),
        m_type(RHIPipelineType::eGraphics),
        m_shader(createInfo.shader),
        m_gfxStates(createInfo.states),
        m_renderPassLayout(createInfo.renderPassLayout),
        m_subpassIdx(createInfo.subpassIdx)
    {}

    RHIPipeline(const RHIComputePipelineCreateInfo& createInfo) :
        RHIResource(ResourceType::ePipeline),
        m_type(RHIPipelineType::eCompute),
        m_shader(createInfo.shader)
    {}

    RHIPipelineType m_type{RHIPipelineType::eNone};

    RHIShader* m_shader{nullptr};

    // for graphics pipeline
    GfxPipelineStates m_gfxStates;
    RenderPassLayout m_renderPassLayout;
    RenderPassHandle m_renderPassHandle{0LLU};
    uint32_t m_subpassIdx;
};

class RHIResourceFactory
{
public:
    virtual ~RHIResourceFactory() = default;

    virtual RHIBuffer* CreateBuffer(const RHIBufferCreateInfo& createInfo) = 0;

    virtual RHITexture* CreateTexture(const RHITextureCreateInfo& createInfo) = 0;

    virtual RHISampler* CreateSampler(const RHISamplerCreateInfo& createInfo) = 0;

    virtual RHIShader* CreateShader(const RHIShaderCreateInfo& createInfo) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIComputePipelineCreateInfo& createInfo) = 0;

    virtual RHIPipeline* CreatePipeline(const RHIGfxPipelineCreateInfo& createInfo) = 0;
};
} // namespace zen::rhi