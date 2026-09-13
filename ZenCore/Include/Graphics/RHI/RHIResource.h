#pragma once
#include "RHICommon.h"
#include "Templates/NameID.h"
#include "Utils/RefCountPtr.h"
#include "Utils/Helpers.h"
#include "Utils/Errors.h"
#include "Templates/HashMap.h"

namespace zen
{
class RHICommandList;

enum class RHIResourceType : uint32_t
{
    eNone          = 0,
    eViewport      = 1,
    eBuffer        = 2,
    eTexture       = 3,
    eTextureView   = 4,
    eSampler       = 5,
    eShader        = 6,
    ePipeline      = 7,
    eDescriptorSet = 8,
    eMax           = 9
};

// Raw RHI callers own resource lifetime: retain resources until every recorded use
// is discarded or completes on the GPU. ReleaseReference/Destroy* do not wait.
class RHIResource
{
public:
    RHIResource() = default;

    virtual ~RHIResource()
    {
        VERIFY_EXPR(m_counter.GetValue() == 0);
    }

    explicit RHIResource(RHIResourceType resourceType) : m_resourceType(resourceType)
    {
        AddReference();
    }

    explicit RHIResource(RHIResourceType resourceType, NameID tag) :
        m_resourceType(resourceType), m_resourceTag(tag)
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

    NameID GetResourceTag() const
    {
        return m_resourceTag;
    }

    uint64_t GetStableId() const
    {
        return m_stableId;
    }

    uint32_t GetGenerationId() const
    {
        return m_generationId;
    }

    void BumpGeneration()
    {
        ++m_generationId;
    }

    RHIResourceType GetResourceType() const
    {
        return m_resourceType;
    }

protected:
    virtual void Init() = 0;

    virtual void Destroy() = 0;

    NameID m_resourceTag;

private:
    static uint64_t GenerateStableId()
    {
        static std::atomic<uint64_t> sNextStableId{1};
        return sNextStableId.fetch_add(1, std::memory_order_relaxed);
    }

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
    RHIResourceType m_resourceType{RHIResourceType::eMax};
    uint64_t m_stableId{GenerateStableId()};
    uint32_t m_generationId{0}; /// For aliasing
};

class RHIShaderGroupSource : public RefCounted
{
public:
    RHIShaderGroupSource() = default;

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
    RHIShaderLanguage m_shaderLanguage{RHIShaderLanguage::eGLSL};
    std::string m_source[ToUnderlying(RHIShaderStage::eMax)];
};

class RHIShaderGroupSPIRV : public RefCounted
{
public:
    RHIShaderGroupSPIRV() = default;

    ~RHIShaderGroupSPIRV() {}

    void SetStageFlags(int64_t flags)
    {
        m_stageFlags = flags;
    }

    void SetStageSPIRV(RHIShaderStage stage, HeapVector<uint8_t>&& source)
    {
        VERIFY_EXPR(stage < RHIShaderStage::eMax);
        m_stageCount = HasShaderStage(stage) ? m_stageCount : m_stageCount++;
        m_stageFlags.SetFlag(static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(stage)));
        m_spirv[static_cast<uint32_t>(stage)] = std::move(source);
    }

    const HeapVector<uint8_t>& GetStageSPIRV(RHIShaderStage stage) const
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

    uint32_t GetStageCount() const
    {
        return m_stageCount;
    }

    uint32_t GetHash32() const
    {
        uint32_t hash = 0;

        // Hash SPIR-V bytecode for each stage
        for (uint32_t i = 0; i < ToUnderlying(RHIShaderStage::eMax); i++)
        {
            if (!HasShaderStage(static_cast<RHIShaderStage>(i)))
            {
                continue;
            }

            const HeapVector<uint8_t>& code = m_spirv[i];

            uint32_t spirvHash = 0;

            for (uint8_t b : code)
            {
                spirvHash ^= b;
                spirvHash *= 0x01000193u;
            }

            util::HashCombine32(hash, spirvHash);
        }

        return hash;
    }

private:
    // shader flags
    BitField<RHIShaderStageFlagBits> m_stageFlags;
    uint32_t m_stageCount{0};

    // shader language
    RHIShaderLanguage m_shaderLanguage{RHIShaderLanguage::eGLSL};

    // spirv code
    HeapVector<uint8_t> m_spirv[ToUnderlying(RHIShaderStage::eMax)];

    // compile errors
    std::string m_compileErrors[ToUnderlying(RHIShaderStage::eMax)];
};

using RHIShaderGroupSourcePtr = RefCountPtr<RHIShaderGroupSource>;
using RHIShaderGroupSPIRVPtr  = RefCountPtr<RHIShaderGroupSPIRV>;

/*****************************/
/********** Sampler **********/
/*****************************/
struct RHISamplerInfo
{
    RHISamplerFilter magFilter{RHISamplerFilter::eNearest};
    RHISamplerFilter minFilter{RHISamplerFilter::eNearest};
    RHISamplerFilter mipFilter{RHISamplerFilter::eNearest};
    RHISamplerRepeatMode repeatU{RHISamplerRepeatMode::eClampToEdge};
    RHISamplerRepeatMode repeatV{RHISamplerRepeatMode::eClampToEdge};
    RHISamplerRepeatMode repeatW{RHISamplerRepeatMode::eClampToEdge};
    float lodBias{0.0f};
    bool useAnisotropy{false};
    float maxAnisotropy{1.0f};
    bool enableCompare{false};
    RHIDepthCompareOperator compareOp{RHIDepthCompareOperator::eAlways};
    float minLod{0.0f};
    float maxLod{1e20}; // Something very large should do.
    RHISamplerBorderColor borderColor{RHISamplerBorderColor::eFloatOpaqueBlack};
    bool unnormalizedUVW{false};
};

/*****************************/
/********* Textures **********/
/*****************************/
class RHITexture;

class RHIViewport : public RHIResource
{
public:
    // static RHIViewport* Create(void* pWindow, uint32_t width, uint32_t height, bool enableVSync);

    // RHIViewport() : RHIResource(RHIResourceType::eViewport)
    // {
    //     AddReference();
    // }

    virtual ~RHIViewport() = default;

    virtual void PrepareForPresent(RHICommandList* pCmdList) = 0;

    virtual bool Present() = 0;

    virtual uint32_t GetWidth() const = 0;

    virtual uint32_t GetHeight() const = 0;

    virtual DataFormat GetSwapchainFormat() = 0;

    virtual DataFormat GetDepthStencilFormat() = 0;

    virtual RHITexture* GetColorBackBuffer() = 0;

    virtual RHITextureSubResourceRange GetColorBackBufferRange() = 0;

    virtual RHITexture* GetDepthStencilBackBuffer() = 0;

    virtual RHITextureSubResourceRange GetDepthStencilBackBufferRange() = 0;

    virtual void Resize(uint32_t width, uint32_t height) = 0;

protected:
    RHIViewport(void* pWindow, uint32_t width, uint32_t height, bool enableVSync) :
        RHIResource(RHIResourceType::eViewport),
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
    BitField<RHIBufferUsageFlagBits> usageFlags{0};
    RHIBufferAllocateType allocateType{RHIBufferAllocateType::eNone};
    NameID tag;
};

class RHIBuffer : public RHIResource
{
public:
    // static RHIBuffer* Create(const RHIBufferCreateInfo& createInfo);

    ~RHIBuffer() {}

    virtual uint8_t* Map() = 0;

    virtual void Unmap() = 0;

    // Creates an immutable view of the logical buffer. Repeating its format is a
    // no-op; changing a successfully created view's format is rejected.
    virtual void SetTexelFormat(DataFormat format) = 0;

    BitField<RHIBufferUsageFlagBits> GetUsageFlags() const
    {
        return m_usageFlags;
    }

    uint32_t GetRequiredSize() const
    {
        return m_requiredSize;
    }

protected:
    explicit RHIBuffer(const RHIBufferCreateInfo& createInfo) :
        RHIResource(RHIResourceType::eBuffer),
        m_requiredSize(createInfo.size),
        m_usageFlags(createInfo.usageFlags),
        m_allocateType(createInfo.allocateType)
    {
        m_resourceTag = createInfo.tag;
    }

    uint32_t m_requiredSize{0};
    BitField<RHIBufferUsageFlagBits> m_usageFlags;
    RHIBufferAllocateType m_allocateType{RHIBufferAllocateType::eNone};
    // BufferHandle m_handle;
};

struct RHITextureCreateInfo
{
    DataFormat format{DataFormat::eUndefined};
    SampleCount samples{SampleCount::e1};
    BitField<RHITextureUsageFlagBits> usageFlags;
    RHITextureType type{RHITextureType::e1D};
    uint32_t width{1};
    uint32_t height{1};
    uint32_t depth{1};
    uint32_t arrayLayers{1};
    uint32_t mipmaps{1};

    // memory flags
    bool cpuReadable{false};
    bool mutableFormat{false};
    NameID tag;
};

struct RHITextureViewCreateInfo
{
    DataFormat format{DataFormat::eUndefined};
    RHITextureType type{RHITextureType::e1D};
    uint32_t arrayLayers{1};
    uint32_t mipLevels{1};
    uint32_t baseMipLevel{0};
    uint32_t baseArrayLayer{0};
    // Empty selects the format's aspects; combined formats retain depth sampling by default.
    BitField<RHITextureAspectFlagBits> aspect;
    NameID tag;
};

class RHITextureView;

class RHITexture : public RHIResource
{
public:
    // Both accessors return borrowed views owned by this texture. Do not release
    // the texture-owned reference. An extra view reference does not retain its
    // base texture; callers retaining a view must also keep the texture alive.
    virtual RHITextureView* CreateView(const RHITextureViewCreateInfo& createInfo) = 0;

    RHITextureView* GetDefaultView() const
    {
        return m_pDefaultView;
    }

    DataFormat GetFormat() const
    {
        return m_baseInfo.format;
    }

    const RHITextureCreateInfo& GetBaseInfo() const
    {
        return m_baseInfo;
    }

    const RHITextureSubResourceRange& GetSubResourceRange() const;

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

    uint32_t GetArrayLayers() const
    {
        return m_baseInfo.arrayLayers;
    }

    uint32_t GetNumMipmaps() const
    {
        return m_baseInfo.mipmaps;
    }

    bool IsRenderTarget() const
    {
        return m_baseInfo.usageFlags.HasFlags(RHITextureUsageFlagBits::eColorAttachment,
                                              RHITextureUsageFlagBits::eDepthStencilAttachment);
    }

    // todo: impl Hash function
    uint32_t GetHash32() const
    {
        return 0;
    }

    static uint32_t CalculateTextureMipLevels(uint32_t dim)
    {
        return static_cast<uint32_t>(floor(log2(dim)) + 1);
    }

    static uint32_t CalculateTextureMipLevels(uint32_t width, uint32_t height)
    {
        return static_cast<uint32_t>(floor(log2(std::max(width, height))) + 1);
    }

    static uint32_t CalculateTextureMipLevels(uint32_t width, uint32_t height, uint32_t depth)
    {
        uint32_t maxDim = std::max(std::max(width, height), depth);
        return static_cast<uint32_t>(floor(log2(maxDim)) + 1);
    }

protected:
    explicit RHITexture(const RHITextureCreateInfo& createInfo) :
        RHIResource(RHIResourceType::eTexture), m_baseInfo(createInfo)
    {
        m_resourceTag = createInfo.tag;
    }

    void RegisterOwnedView(RHITextureView* view)
    {
        m_ownedViews.push_back(view);
    }

    void DestroyOwnedViews();

    const RHITexture* m_pBaseTexture{nullptr};

    RHITextureCreateInfo m_baseInfo{};

    RHITextureView* m_pDefaultView{nullptr};
    SmallVector<RHITextureView*, 4> m_ownedViews;
};

class RHITextureView : public RHIResource
{
public:
    DataFormat GetFormat() const
    {
        return m_viewInfo.format;
    }

    RHITexture* GetTexture() const
    {
        return m_pTexture;
    }

    const RHITextureSubResourceRange& GetSubResourceRange() const
    {
        return m_subResourceRange;
    }

    RHITextureType GetTextureType() const
    {
        return m_viewInfo.type;
    }

protected:
    RHITextureView(RHITexture* pTexture, RHITextureViewCreateInfo createInfo) :
        RHIResource(RHIResourceType::eTextureView, createInfo.tag),
        m_pTexture(pTexture),
        m_viewInfo(std::move(createInfo))
    {
        InitSubresourceRange();
    }

    RHITexture* m_pTexture{nullptr};

    RHITextureSubResourceRange m_subResourceRange;

    RHITextureViewCreateInfo m_viewInfo{};

private:
    void InitSubresourceRange()
    {
        m_subResourceRange.aspect         = m_viewInfo.aspect.IsEmpty() ?
            GetTextureFormatAspects(m_viewInfo.format) :
            m_viewInfo.aspect;
        m_subResourceRange.layerCount   = m_viewInfo.arrayLayers;
        m_subResourceRange.levelCount   = m_viewInfo.mipLevels;
        m_subResourceRange.baseMipLevel = m_viewInfo.baseMipLevel;
        m_subResourceRange.baseArrayLayer = m_viewInfo.baseArrayLayer;
    }
};

inline BitField<RHITextureAspectFlagBits> RHIRenderTarget::GetAspects() const
{
    if (pTextureView != nullptr)
    {
        return pTextureView->GetSubResourceRange().aspect;
    }
    return GetTextureFormatAspects(format);
}

inline void RHITexture::DestroyOwnedViews()
{
    for (RHITextureView* pView : m_ownedViews)
    {
        if (pView != nullptr)
        {
            pView->ReleaseReference();
        }
    }

    m_ownedViews.clear();
    m_pDefaultView = nullptr;
}

inline const RHITextureSubResourceRange& RHITexture::GetSubResourceRange() const
{
    return m_pDefaultView->GetSubResourceRange();
}

struct RHISamplerCreateInfo
{
    RHISamplerFilter magFilter{RHISamplerFilter::eNearest};
    RHISamplerFilter minFilter{RHISamplerFilter::eNearest};
    RHISamplerFilter mipFilter{RHISamplerFilter::eNearest};
    RHISamplerRepeatMode repeatU{RHISamplerRepeatMode::eClampToEdge};
    RHISamplerRepeatMode repeatV{RHISamplerRepeatMode::eClampToEdge};
    RHISamplerRepeatMode repeatW{RHISamplerRepeatMode::eClampToEdge};
    float lodBias{0.0f};
    bool useAnisotropy{false};
    float maxAnisotropy{1.0f};
    bool enableCompare{false};
    RHIDepthCompareOperator compareOp{RHIDepthCompareOperator::eAlways};
    float minLod{0.0f};
    float maxLod{1e20}; // Something very large should do.
    RHISamplerBorderColor borderColor{RHISamplerBorderColor::eFloatOpaqueBlack};
    bool unnormalizedUVW{false};
};

class RHISampler : public RHIResource
{
public:
    // static RHISampler* Create(const RHISamplerCreateInfo& createInfo);
    ~RHISampler() {}

protected:
    explicit RHISampler(const RHISamplerCreateInfo& createInfo) :
        RHIResource(RHIResourceType::eSampler), m_baseInfo(createInfo)
    {}

    RHISamplerCreateInfo m_baseInfo{};
};

class RHIShader;

struct RHIShaderCreateInfo
{
    std::string spirvFileName[ToUnderlying(RHIShaderStage::eMax)];
    BitField<RHIShaderStageFlagBits> stageFlags;
    HashMap<uint32_t, int> specializationConstants;
    NameID name;
};

// Includes all stages used
class RHIShader : public RHIResource
{
public:
    ~RHIShader() override = default;

    RHIShaderCreateInfo GetCreateInfo() const
    {
        RHIShaderCreateInfo info{};
        std::ranges::copy(m_spirvFileName, std::begin(info.spirvFileName));
        info.stageFlags              = m_shaderStageFlags;
        info.specializationConstants = m_specializationConstants;
        info.name                    = m_name;

        return info;
    }

    const RHIShaderResourceDescriptorTable* GetSRDTable() const
    {
        return &m_SRDTable;
    }

    uint32_t GetSRDCountByType(RHIShaderResourceType type) const
    {
        return m_SRDCount[ToUnderlying(type)];
    }

    const RHIShaderResourceDescriptor* GetSRDByLocation(uint32_t set, uint32_t binding)
    {
        const RHIShaderResourceDescriptor* pSRD = nullptr;

        if (set < m_SRDTable.size())
        {
            for (const RHIShaderResourceDescriptor& srd : m_SRDTable[set])
            {
                if (srd.binding == binding)
                {
                    pSRD = &srd;
                    break;
                }
            }
        }

        return pSRD;
    }

    const RHIShaderResourceDescriptor* GetSRDByName(NameID glslName)
    {
        return m_namedSRDLut.contains(glslName) ? m_namedSRDLut[glslName] : nullptr;
    }

    uint32_t GetHash32() const
    {
        uint32_t hash = 0;

        // Shader stage flags
        util::HashCombine32(hash, m_shaderStageFlags);

        // Hash the SPIR-V src
        util::HashCombine32(hash, m_shaderGroupSPIRV->GetHash32());

        // Hash specialization constants
        for (const HashMap<uint32_t, int>::value_type& constant : m_specializationConstants)
        {
            uint32_t kv =
                (constant.first << 16) ^ (static_cast<uint32_t>(constant.second) & 0xFFFFu);
            util::HashCombine32(hash, kv);
        }

        // Hash shader name
        util::HashCombine32T(hash, m_name);

        return hash;
    }

protected:
    explicit RHIShader(const RHIShaderCreateInfo& createInfo) :
        RHIResource(RHIResourceType::eShader),
        m_shaderGroupSPIRV(MakeRefCountPtr<RHIShaderGroupSPIRV>()),
        m_shaderStageFlags(createInfo.stageFlags),
        m_specializationConstants(createInfo.specializationConstants),
        m_SRDCount(ToUnderlying(RHIShaderResourceType::eMax)),
        m_name(createInfo.name)
    {
        std::ranges::copy(createInfo.spirvFileName, std::begin(m_spirvFileName));

        m_shaderGroupSPIRV->SetStageFlags(m_shaderStageFlags);
    }

    RHIShaderGroupSPIRVPtr m_shaderGroupSPIRV{};
    std::string m_spirvFileName[ToUnderlying(RHIShaderStage::eMax)];
    BitField<RHIShaderStageFlagBits> m_shaderStageFlags;
    HashMap<uint32_t, int> m_specializationConstants;
    RHIShaderResourceDescriptorTable m_SRDTable;
    SmallVector<uint32_t, ToUnderlying(RHIShaderResourceType::eMax)> m_SRDCount;
    HashMap<NameID, const RHIShaderResourceDescriptor*> m_namedSRDLut;
    NameID m_name;
};

struct RHIRenderingLayout
{
    Rect2<int> renderArea;
    uint32_t numLayers{1};
    uint32_t numColorRenderTargets{0};
    bool hasDepthStencilRT{false};
    RHIRenderTarget colorRenderTargets[MAX_NUM_COLOR_ATTACHMENTS];
    RHIRenderTarget depthStencilRenderTarget;

    void Reset()
    {
        ClearRenderTargetInfo();
    }

    uint32_t GetTotalNumRenderTargets() const
    {
        return hasDepthStencilRT ? numColorRenderTargets + 1 : numColorRenderTargets;
    }

    void GetRHIRenderTargetClearValueData(RHIRenderTargetClearValue* pClearValues) const
    {
        uint32_t rtIdx = 0;

        for (; rtIdx < numColorRenderTargets; rtIdx++)
        {
            pClearValues[rtIdx] = colorRenderTargets[rtIdx].clearValue;
        }

        if (hasDepthStencilRT)
        {
            pClearValues[rtIdx] = depthStencilRenderTarget.clearValue;
        }
    }

    void GetRHITextureData(RHITexture** pTextures) const
    {
        uint32_t rtIdx = 0;

        for (; rtIdx < numColorRenderTargets; rtIdx++)
        {
            pTextures[rtIdx] = colorRenderTargets[rtIdx].pTexture;
        }

        if (hasDepthStencilRT)
        {
            pTextures[rtIdx] = depthStencilRenderTarget.pTexture;
        }
    }

    void SetRenderArea(int32_t offsetX, int32_t offsetY, uint32_t width, uint32_t height)
    {
        if (int64_t(offsetX) + width > INT32_MAX || int64_t(offsetY) + height > INT32_MAX)
        {
            LOG_ERROR_AND_THROW("Render area exceeds the RHI coordinate range");
        }
        renderArea.minX = offsetX;
        renderArea.maxX = static_cast<int32_t>(int64_t(offsetX) + width);
        renderArea.minY = offsetY;
        renderArea.maxY = static_cast<int32_t>(int64_t(offsetY) + height);
    }

    void AddColorRenderTarget(DataFormat format,
                              RHITexture* pTexture,
                              RHIRenderTargetLoadOp loadOp,
                              RHIRenderTargetStoreOp storeOp,
                              RHIRenderTargetClearValue clearValue,
                              SampleCount numSamples)
    {
        if (numColorRenderTargets >= MAX_NUM_COLOR_ATTACHMENTS)
        {
            LOG_ERROR_AND_THROW("Too many color attachments");
        }
        RHIRenderTarget colorRT;
        colorRT.format     = format;
        colorRT.pTexture   = pTexture;
        colorRT.loadOp     = loadOp;
        colorRT.storeOp    = storeOp;
        colorRT.clearValue = clearValue;
        colorRT.numSamples = numSamples;

        IncludeAttachmentLayers(pTexture != nullptr ? pTexture->GetArrayLayers() : 1);
        colorRenderTargets[numColorRenderTargets++] = colorRT;
    }

    void AddColorRenderTarget(DataFormat format,
                              RHITexture* pTexture,
                              RHIRenderTargetLoadOp loadOp,
                              RHIRenderTargetStoreOp storeOp,
                              RHIRenderTargetClearValue clearValue = DEFAULT_COLOR_CLEAR_VALUE)
    {
        AddColorRenderTarget(format, pTexture, loadOp, storeOp, clearValue,
                             pTexture != nullptr ? pTexture->GetBaseInfo().samples :
                                                   SampleCount::e1);
    }

    void AddColorRenderTarget(RHITextureView* pView,
                              RHIRenderTargetLoadOp loadOp,
                              RHIRenderTargetStoreOp storeOp,
                              RHIRenderTargetClearValue clearValue = DEFAULT_COLOR_CLEAR_VALUE)
    {
        if (pView == nullptr)
        {
            LOG_ERROR_AND_THROW("Color attachment view is null");
        }
        const uint32_t previousLayers = numLayers;
        const bool firstAttachment    = GetTotalNumRenderTargets() == 0;
        AddColorRenderTarget(pView->GetFormat(), pView->GetTexture(), loadOp, storeOp, clearValue);
        colorRenderTargets[numColorRenderTargets - 1].pTextureView = pView;
        numLayers                                                  = firstAttachment ?
            pView->GetSubResourceRange().layerCount :
            std::min(previousLayers, pView->GetSubResourceRange().layerCount);
    }

    void AddDepthStencilRenderTarget(DataFormat format,
                                     RHITexture* pTexture,
                                     RHIRenderTargetLoadOp loadOp,
                                     RHIRenderTargetStoreOp storeOp,
                                     RHIRenderTargetClearValue clearValue = DEFAULT_DS_CLEAR_VALUE)
    {
        if (!hasDepthStencilRT)
        {
            depthStencilRenderTarget.format     = format;
            depthStencilRenderTarget.pTexture   = pTexture;
            depthStencilRenderTarget.numSamples =
                pTexture != nullptr ? pTexture->GetBaseInfo().samples : SampleCount::e1;
            depthStencilRenderTarget.loadOp     = loadOp;
            depthStencilRenderTarget.storeOp    = storeOp;
            depthStencilRenderTarget.clearValue = clearValue;
            IncludeAttachmentLayers(pTexture != nullptr ? pTexture->GetArrayLayers() : 1);
            hasDepthStencilRT                   = true;
        }
    }

    void AddDepthStencilRenderTarget(RHITextureView* pView,
                                     RHIRenderTargetLoadOp loadOp,
                                     RHIRenderTargetStoreOp storeOp,
                                     RHIRenderTargetClearValue clearValue = DEFAULT_DS_CLEAR_VALUE)
    {
        if (pView == nullptr)
        {
            LOG_ERROR_AND_THROW("Depth/stencil attachment view is null");
        }
        if (!hasDepthStencilRT)
        {
            const uint32_t previousLayers = numLayers;
            const bool firstAttachment    = GetTotalNumRenderTargets() == 0;
            AddDepthStencilRenderTarget(pView->GetFormat(), pView->GetTexture(), loadOp, storeOp,
                                        clearValue);
            depthStencilRenderTarget.pTextureView = pView;
            numLayers                             = firstAttachment ?
                pView->GetSubResourceRange().layerCount :
                std::min(previousLayers, pView->GetSubResourceRange().layerCount);
        }
    }

    void ClearRenderTargetInfo()
    {
        std::ranges::fill(colorRenderTargets, RHIRenderTarget{});
        depthStencilRenderTarget = {};
        hasDepthStencilRT        = false;
        numColorRenderTargets    = 0;
        numLayers                = 1;
    }

    uint32_t GetHash32() const
    {
        uint32_t seed = 0;
        // Hash basic types
        util::HashCombine32(seed, numColorRenderTargets);
        util::HashCombine32(seed, hasDepthStencilRT);
        util::HashCombine32(seed, renderArea.minX);
        util::HashCombine32(seed, renderArea.minY);
        util::HashCombine32(seed, renderArea.maxX);
        util::HashCombine32(seed, renderArea.maxY);

        for (uint32_t i = 0; i < numColorRenderTargets; ++i)
        {
            const RHIRenderTarget& rt = colorRenderTargets[i];
            util::HashCombine32T(seed, rt.loadOp);
            util::HashCombine32T(seed, rt.storeOp);
            util::HashCombine32T(seed, rt.numSamples);
            util::HashCombine32T(seed, rt.format);
        }

        if (hasDepthStencilRT)
        {
            util::HashCombine32T(seed, depthStencilRenderTarget.loadOp);
            util::HashCombine32T(seed, depthStencilRenderTarget.storeOp);
            util::HashCombine32T(seed, depthStencilRenderTarget.format);
        }

        return seed;
    }

private:
    void IncludeAttachmentLayers(uint32_t layers)
    {
        numLayers = GetTotalNumRenderTargets() == 0 ? layers : std::min(numLayers, layers);
    }
};

struct RHIGeometryBuffer
{
    HeapVector<RHIBuffer*> vertexBuffers;
    RHIBuffer* pIndexBuffer{nullptr};
    DataFormat indexBufferFormat{DataFormat::eR32UInt};
    uint32_t indexBufferOffset{0};
};

struct RHIGfxPipelineCreateInfo
{
    RHIShader* pShader;
    RHIGfxPipelineStates states;

    // RHIRenderPassLayout renderPassLayout;
    const RHIRenderingLayout* pRenderingLayout;

    // RenderPassHandle renderPassHandle;
    uint32_t subpassIdx;
};

struct RHIComputePipelineCreateInfo
{
    RHIShader* pShader;
};

// enum class RHIPipelineType : uint32_t
// {
//     eNone     = 0,
//     eGraphics = 1,
//     eCompute  = 2,
//     eMax      = 3
// };

class RHIPipeline : public RHIResource
{
public:
    ~RHIPipeline() override
    {
        if (m_pShader != nullptr)
        {
            m_pShader->ReleaseReference();
        }
    }

    RHIShader* GetShader() const
    {
        return m_pShader;
    }

protected:
    RHIPipeline(const RHIGfxPipelineCreateInfo& createInfo) :
        RHIResource(RHIResourceType::ePipeline),
        m_type(RHIPipelineType::eGraphics),
        m_pShader(createInfo.pShader),
        m_gfxStates(createInfo.states),
        // m_renderPassLayout(createInfo.renderPassLayout),
        m_pRenderingLayout(createInfo.pRenderingLayout),
        m_subpassIdx(createInfo.subpassIdx)
    {
        m_pShader->AddReference();
    }

    RHIPipeline(const RHIComputePipelineCreateInfo& createInfo) :
        RHIResource(RHIResourceType::ePipeline),
        m_type(RHIPipelineType::eCompute),
        m_pShader(createInfo.pShader)
    {
        m_pShader->AddReference();
    }

    RHIPipelineType m_type{RHIPipelineType::eNone};

    RHIShader* m_pShader{nullptr};

    // for graphics pipeline
    RHIGfxPipelineStates m_gfxStates;

    // RHIRenderPassLayout m_renderPassLayout;
    const RHIRenderingLayout* m_pRenderingLayout;

    // RenderPassHandle m_renderPassHandle{0LLU};
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
} // namespace zen
