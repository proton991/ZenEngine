#pragma once
#include "RHIDefs.h"
#include "Common/BitField.h"
#include <string>
#include <vector>

#define MAX_COLOR_ATTACHMENT_COUNT 8
#define MAX_SUBPASS_COUNT          8

#define ROUND_UP_ALIGNMENT(m_number, m_alignment) \
    ((((m_number) + ((m_alignment)-1)) / (m_alignment)) * (m_alignment))

namespace zen::rhi
{}
namespace zen::rhi
{
template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

enum class DataFormat : uint32_t
{
    eUndefined          = 0,   // = VK_FORMAT_UNDEFINED
    eR16UInt            = 74,  // = VK_FORMAT_R16_UINT
    eR16SInt            = 75,  // = VK_FORMAT_R16_SINT
    eR16SFloat          = 76,  // = VK_FORMAT_R16_SFLOAT
    eR16G16UInt         = 81,  // = VK_FORMAT_R16G16_UINT
    eR16G16SInt         = 82,  // = VK_FORMAT_R16G16_SINT
    eR16G16SFloat       = 83,  // = VK_FORMAT_R16G16_SFLOAT
    eR16G16B16UInt      = 88,  // = VK_FORMAT_R16G16B16_UINT
    eR16G16B16SInt      = 89,  // = VK_FORMAT_R16G16B16_SINT
    eR16G16B16SFloat    = 90,  // = VK_FORMAT_R16G16B16_SFLOAT
    eR16G16B16A16UInt   = 95,  // = VK_FORMAT_R16G16B16A16_UINT
    eR16G16B16A16SInt   = 96,  // = VK_FORMAT_R16G16B16A16_SINT
    eR16G16B16A16SFloat = 97,  // = VK_FORMAT_R16G16B16A16_SFLOAT
    eR32UInt            = 98,  // = VK_FORMAT_R32_UINT
    eR32SInt            = 99,  // = VK_FORMAT_R32_SINT
    eR32SFloat          = 100, // = VK_FORMAT_R32_SFLOAT
    eR32G32UInt         = 101, // = VK_FORMAT_R32G32_UINT
    eR32G32SInt         = 102, // = VK_FORMAT_R32G32_SINT
    eR32G32SFloat       = 103, // = VK_FORMAT_R32G32_SFLOAT
    eR32G32B32UInt      = 104, // = VK_FORMAT_R32G32B32_UINT
    eR32G32B32SInt      = 105, // = VK_FORMAT_R32G32B32_SINT
    eR32G32B32SFloat    = 106, // = VK_FORMAT_R32G32B32_SFLOAT
    eR32G32B32A32UInt   = 107, // = VK_FORMAT_R32G32B32A32_UINT
    eR32G32B32A32SInt   = 108, // = VK_FORMAT_R32G32B32A32_SINT
    eR32G32B32A32SFloat = 109, // = VK_FORMAT_R32G32B32A32_SFLOAT
    eR64UInt            = 110, // = VK_FORMAT_R64_UINT
    eR64SInt            = 111, // = VK_FORMAT_R64_SINT
    eR64SFloat          = 112, // = VK_FORMAT_R64_SFLOAT
    eR64G64UInt         = 113, // = VK_FORMAT_R64G64_UINT
    eR64G64SInt         = 114, // = VK_FORMAT_R64G64_SINT
    eR64G64SFloat       = 115, // = VK_FORMAT_R64G64_SFLOAT
    eR64G64B64UInt      = 116, // = VK_FORMAT_R64G64B64_UINT
    eR64G64B64SInt      = 117, // = VK_FORMAT_R64G64B64_SINT
    eR64G64B64SFloat    = 118, // = VK_FORMAT_R64G64B64_SFLOAT
    eR64G64B64A64UInt   = 119, // = VK_FORMAT_R64G64B64A64_UINT
    eR64G64B64A64SInt   = 120, // = VK_FORMAT_R64G64B64A64_SINT
    eR64G64B64A64SFloat = 121, // = VK_FORMAT_R64G64B64A64_SFLOAT
};

inline uint32_t GetTextureFormatPixelSize(DataFormat format)
{
    switch (format)
    {
        case DataFormat::eR16UInt:
        case DataFormat::eR16SInt:
        case DataFormat::eR16SFloat: return 2;

        case DataFormat::eR16G16UInt:
        case DataFormat::eR16G16SInt:
        case DataFormat::eR16G16SFloat:
        case DataFormat::eR32UInt:
        case DataFormat::eR32SInt:
        case DataFormat::eR32SFloat: return 4;

        case DataFormat::eR16G16B16UInt:
        case DataFormat::eR16G16B16SInt:
        case DataFormat::eR16G16B16SFloat: return 6;

        case DataFormat::eR16G16B16A16UInt:
        case DataFormat::eR16G16B16A16SInt:
        case DataFormat::eR16G16B16A16SFloat:
        case DataFormat::eR32G32UInt:
        case DataFormat::eR32G32SInt:
        case DataFormat::eR32G32SFloat:
        case DataFormat::eR64UInt:
        case DataFormat::eR64SInt:
        case DataFormat::eR64SFloat: return 8;

        case DataFormat::eR32G32B32UInt:
        case DataFormat::eR32G32B32SInt:
        case DataFormat::eR32G32B32SFloat: return 12;

        case DataFormat::eR32G32B32A32UInt:
        case DataFormat::eR32G32B32A32SInt:
        case DataFormat::eR32G32B32A32SFloat:
        case DataFormat::eR64G64UInt:
        case DataFormat::eR64G64SInt:
        case DataFormat::eR64G64SFloat: return 16;

        case DataFormat::eR64G64B64UInt:
        case DataFormat::eR64G64B64SInt:
        case DataFormat::eR64G64B64SFloat: return 24;

        case DataFormat::eR64G64B64A64UInt:
        case DataFormat::eR64G64B64A64SInt:
        case DataFormat::eR64G64B64A64SFloat: return 32;

        default: return 0x7fffffff;
    }
}

/*******************/
/**** Shaders ****/
/*******************/
enum class ShaderLanguage : uint32_t
{
    eGLSL = 0,
    eMax  = 1
};
enum class ShaderStage : uint32_t
{
    eVertex                = 0,
    eFragment              = 1,
    eTesselationConrol     = 2,
    eTesselationEvaluation = 3,
    eCompute               = 4,
    eMax                   = 5
};

enum class ShaderStageFlagBits : uint32_t
{
    eVertex                = 1 << 0,
    eFragment              = 1 << 1,
    eTesselationConrol     = 1 << 2,
    eTesselationEvaluation = 1 << 3,
    eCompute               = 1 << 4,
    eMax                   = 1 << 5
};

static std::string ShaderStageToString(ShaderStage stage)
{
    static const char* SHADER_STAGE_NAMES[ToUnderlying(ShaderStage::eMax)] = {
        "Vertex", "Fragment", "TesselationControl", "TesselationEvaluation", "Compute",
    };
    return SHADER_STAGE_NAMES[ToUnderlying(stage)];
}

static std::string ShaderStageFlagToString(BitField<ShaderStageFlagBits> stageFlags)
{
    std::string str;
    if (stageFlags.HasFlag(ShaderStageFlagBits::eVertex)) { str += "Vertex "; }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eFragment)) { str += "Fragment "; }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationConrol))
    {
        str += "TesselationControl ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eTesselationEvaluation))
    {
        str += "TesselationEvaluation ";
    }
    if (stageFlags.HasFlag(ShaderStageFlagBits::eCompute)) { str += "Compute "; }
    str.pop_back();
    return str;
}

static ShaderStageFlagBits ShaderStageToFlagBits(ShaderStage stage)
{
    return static_cast<ShaderStageFlagBits>(1 << ToUnderlying(stage));
}

enum class ShaderSpecializationConstantType : uint32_t
{
    eBool  = 0,
    eInt   = 1,
    eFloat = 2,
    eMax   = 3
};

struct ShaderSpecializationConstant
{
    union
    {
        uint32_t intValue = 0;
        float floatValue;
        bool boolValue;
    };
    ShaderSpecializationConstantType type{ShaderSpecializationConstantType::eMax};
    uint32_t constantId{0};
    BitField<ShaderStageFlagBits> stages;
};

enum class ShaderResourceType : uint32_t
{
    // For sampling only (sampler GLSL type).
    eSampler = 0,
    // Only texture, (textureXX GLSL type).
    eTexture = 1,
    // For sampling only, but includes a texture, (samplerXX GLSL type), first a sampler then a texture.
    eSamplerWithTexture = 2,
    // Storage image (imageXX GLSL type), for compute mostly.
    eImage = 3,
    // Buffer texture (or TBO, textureBuffer type).
    eTextureBuffer = 4,
    // Buffer texture with a sampler(or TBO, samplerBuffer type).
    eSamplerWithTextureBuffer = 5,
    // Texel buffer, (imageBuffer type), for compute mostly.
    eImageBuffer = 6,
    // Regular uniform buffer (or UBO).
    eUniformBuffer = 7,
    // Storage buffer ("buffer" qualifier) like UBO, but supports storage, for compute mostly.
    eStorageBuffer = 8,
    // Used for sub-pass read/write, for mobile mostly.
    eInputAttachment = 9,
    eMax             = 10
};

struct ShaderResourceDescriptor
{
    std::string name;
    ShaderResourceType type{ShaderResourceType::eMax};
    BitField<ShaderStageFlagBits> stageFlags;
    bool writable{false};
    // Size of arrays (in total elements), or ubos (in bytes * total elements).
    uint32_t arraySize{1};
    uint32_t blockSize{0};
    uint32_t set{0};
    uint32_t binding{0};
};

struct VertexInputAttribute
{
    std::string name;
    uint32_t location{0};
    uint32_t binding{0};
    uint32_t offset{0};
    DataFormat format{0};
};

struct ShaderPushConstants
{
    std::string name;
    uint32_t size{0};
    BitField<ShaderStageFlagBits> stageFlags;
};

// .vert .frag .compute together
struct ShaderGroupInfo
{
    // TODO: store spirv code using some cache strategy, no-copy
    std::vector<std::vector<uint8_t>> sprivCode;
    ShaderPushConstants pushConstants{};
    // vertex input attribute
    std::vector<VertexInputAttribute> vertexInputAttributes;
    // vertex binding stride
    uint32_t vertexBindingStride{0};
    // per set shader resources
    std::vector<std::vector<ShaderResourceDescriptor>> SRDs;
    // specialization constants
    std::vector<ShaderSpecializationConstant> specializationConstants;
    // shader stages
    std::vector<ShaderStage> shaderStages;
};

/*****************************/
/**** Gfx Pipeline States ****/
/*****************************/
enum class DrawPrimitiveType : uint32_t
{
    ePointList                  = 0,
    eLineList                   = 1,
    eLineStrip                  = 2,
    eTriangleList               = 3,
    eTriangleStrip              = 4,
    eTriangleFan                = 5,
    eLineListWithAdjacency      = 6,
    eLineStripWithAdjacency     = 7,
    eTriangleListWithAdjacency  = 8,
    eTriangleStripWithAdjacency = 9,
    ePatchList                  = 10,
    eMax                        = 11
};

enum class PolygonCullMode : uint32_t
{
    eDisabled     = 0,
    eFront        = 1,
    eBack         = 2,
    eFrontAndBack = 3,
    eMax          = 4
};

enum class PolygonFrontFace : uint32_t
{
    eClockWise        = 0,
    eCounterClockWise = 1,
    eMax              = 2,
};

enum class SampleCount : uint32_t
{
    e1   = 0,
    e2   = 1,
    e4   = 2,
    e8   = 3,
    e16  = 4,
    e32  = 5,
    e64  = 6,
    eMax = 7
};

enum class CompareOperator : uint32_t
{
    eNever          = 0,
    eLess           = 1,
    eEqual          = 2,
    eLessOrEqual    = 3,
    eGreater        = 4,
    eNotEqual       = 5,
    eGreaterOrEqual = 6,
    eAlways         = 7,
    eMax            = 8
};

enum class LogicOperation : uint32_t
{
    eClear        = 0,
    eAnd          = 1,
    eAndReverse   = 2,
    eCopy         = 3,
    eAndInverted  = 4,
    eNoOp         = 5,
    eXor          = 6,
    eOr           = 7,
    eNor          = 8,
    eEquivalent   = 9,
    eInvert       = 10,
    eOrReverse    = 11,
    eCopyInverted = 12,
    eOrInverted   = 13,
    eNand         = 14,
    eSet          = 15,
    eMax          = 16
};

struct GfxPipelineRasterizationState
{
    bool enableDepthClamp{false};
    bool discardPrimitives{false};
    bool wireframe{false};
    PolygonCullMode cullMode{PolygonCullMode::eDisabled};
    PolygonFrontFace frontFace{PolygonFrontFace::eClockWise};
    bool enableDepthBias{false};
    float depthBiasConstantFactor{0.0f};
    float depthBiasClamp{0.0f};
    float depthBiasSlopeFactor{0.0f};
    float lineWidth{1.0f};
};

struct GfxPipelineMultiSampleState
{
    SampleCount sampleCount{SampleCount::e1};
    bool enableSampleShading{false};
    float minSampleShading{0.0f};
    bool enableAlphaToCoverage{false};
    bool enbaleAlphaToOne{false};
    std::vector<uint32_t> sampleMasks;
};

enum class StencilOperation : uint32_t
{
    eKeep              = 0,
    eZero              = 1,
    eReplace           = 2,
    eIncrementAndClamp = 3,
    eDecrementAndClamp = 4,
    eInvert            = 5,
    eIncrementAndWrap  = 6,
    eDecrementAndWrap  = 7,
    eMax               = 8
};

struct StencilOperationState
{
    StencilOperation fail{StencilOperation::eReplace};
    StencilOperation pass{StencilOperation::eReplace};
    StencilOperation depthFail{StencilOperation::eReplace};
    CompareOperator compare{CompareOperator::eAlways};
    uint32_t compareMask{0};
    uint32_t writeMask{0};
    uint32_t reference{0};
};


struct GfxPipelineDepthStencilState
{
    bool enableDepthTest{false};
    bool enableDepthWrite{false};
    CompareOperator depthCompareOp{CompareOperator::eNever};
    bool enableDepthBoundsTest{false};
    bool enableStencilTest{false};
    StencilOperationState frontOp{};
    StencilOperationState backOp{};
    float minDepthBounds{0.0f};
    float maxDepthBounds{1.0f};
};

enum class BlendFactor : uint32_t
{
    eZero                  = 0,
    eOne                   = 1,
    eSrcColor              = 2,
    eOneMinusSrcColor      = 3,
    eDstColor              = 4,
    eOneMinusDstColor      = 5,
    eSrcAlpha              = 6,
    eOneMinusSrcAlpha      = 7,
    eDstAlpha              = 8,
    eOneMinusDstAlpha      = 9,
    eConstantColor         = 10,
    eOneMinusConstantColor = 11,
    eConstantAlpha         = 12,
    eOneMinusConstantAlpha = 13,
    eSrcAlphaSatuate       = 14,
    eSrc1Color             = 15,
    eOneMinusSrc1Color     = 16,
    eSrc1Alpha             = 17,
    eOneMinusSrc1Alpha     = 18,
    eMax                   = 19
};

enum class BlendOperation : uint32_t
{
    eAdd              = 0,
    eSubstract        = 1,
    eReverseSubstract = 2,
    eMinimum          = 3,
    eMaximum          = 4,
    eMax              = 5
};

struct GfxPipelineColorBlendState
{
    bool enableLogicOp{false};
    LogicOperation logicOp{LogicOperation::eClear};

    struct Attachment
    {
        bool enableBlend{false};
        BlendFactor srcColorBlendFactor{BlendFactor::eZero};
        BlendFactor dstColorBlendFactor{BlendFactor::eZero};
        BlendOperation colorBlendOp{BlendOperation::eAdd};
        BlendFactor srcAlphaBlendFactor{BlendFactor::eZero};
        BlendFactor dstAlphaBlendFactor{BlendFactor::eZero};
        BlendOperation alphaBlendOp{BlendOperation::eAdd};
        bool writeR{true};
        bool writeG{true};
        bool writeB{true};
        bool writeA{true};
    };

    static GfxPipelineColorBlendState create_disabled(int count = 1)
    {
        GfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++) { bs.attachments.emplace_back(); }
        return bs;
    }

    static GfxPipelineColorBlendState createBlend(int count = 1)
    {
        GfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            Attachment ba;
            ba.enableBlend         = true;
            ba.srcColorBlendFactor = BlendFactor::eSrcAlpha;
            ba.dstColorBlendFactor = BlendFactor::eOneMinusSrcAlpha;
            ba.srcAlphaBlendFactor = BlendFactor::eSrcAlpha;
            ba.dstAlphaBlendFactor = BlendFactor::eOneMinusSrcAlpha;

            bs.attachments.emplace_back(ba);
        }
        return bs;
    }

    std::vector<Attachment> attachments; // One per render target texture.
    std::vector<float> blendConstants;
};

enum class DynamicState : uint32_t
{
    eViewPort = 0,
    eScissor  = 1,
    eMax      = 2
};

struct GfxPipelineStates
{
    DrawPrimitiveType primitiveType;
    GfxPipelineRasterizationState rasterizationState;
    GfxPipelineMultiSampleState multiSampleState;
    GfxPipelineDepthStencilState depthStencilState;
    GfxPipelineColorBlendState colorBlendState;
    std::vector<DynamicState> dynamicStates;
};
/*****************************/
/********* Buffers ***********/
/*****************************/
enum class BufferUsageFlagBits
{
    eTransferSrcBuffer = 1 << 0,
    eTransferDstBuffer = 1 << 1,
    eTextureBuffer     = 1 << 2,
    eImageBuffer       = 1 << 3,
    eUniformBuffer     = 1 << 4,
    eStorageBuffer     = 1 << 5,
    eIndexBuffer       = 1 << 6,
    eVertexBuffer      = 1 << 7,
    eIndirectBuffer    = 1 << 8,
    eMax               = 0x7FFFFFFF
};

enum class BufferAllocateType
{
    eCPU = 0,
    eGPU = 1,
    eMax = 2
};

/*****************************/
/********* Textures **********/
/*****************************/
enum class TextureUsageFlagBits : uint32_t
{
    eTransferSrc            = 1 << 0,
    eTransferDst            = 1 << 1,
    eSampled                = 1 << 2,
    eStorage                = 1 << 3,
    eColorAttachment        = 1 << 4,
    eDepthStencilAttachment = 1 << 5,
    eTransientAttachment    = 1 << 6,
    eInputAttachment        = 1 << 7,
    eMax                    = 0x7FFFFFFF
};

enum class TextureUsage : uint32_t
{
    eTransferSrc            = 0,
    eTransferDst            = 1,
    eSampled                = 2,
    eStorage                = 3,
    eColorAttachment        = 4,
    eDepthStencilAttachment = 5,
    eTransientAttachment    = 6,
    eInputAttachment        = 7,
    eMax                    = 8
};

enum class TextureLayout : uint32_t
{
    eUndefined,
    eGeneral,
    eColorTarget,
    eDepthStencilTarget,
    eShaderReadOnly,
    eTransferSrc,
    eTransferDst,
    eMax
};

enum class TextureType : uint32_t
{
    e1D  = 0,
    e2D  = 1,
    e3D  = 2,
    eMax = 3
};

inline TextureLayout TextureUsageToLayout(TextureUsage usage)
{
    switch (usage)
    {
        case TextureUsage::eTransferSrc: return TextureLayout::eTransferSrc;
        case TextureUsage::eTransferDst: return TextureLayout::eTransferDst;
        case TextureUsage::eSampled:
        case TextureUsage::eInputAttachment: return TextureLayout::eTransferSrc;
        case TextureUsage::eStorage: return TextureLayout::eGeneral;
        case TextureUsage::eColorAttachment: return TextureLayout::eColorTarget;
        case TextureUsage::eDepthStencilAttachment: return TextureLayout::eDepthStencilTarget;
        default: return TextureLayout::eUndefined;
    }
}

/*****************************/
/** Renderpass and subpass ***/
/*****************************/
enum class RenderTargetLoadOp : uint32_t
{
    eLoad  = 0,
    eClear = 1,
    eNone  = 2,
    eMax   = 3
};

enum class RenderTargetStoreOp : uint32_t
{
    eStore = 0,
    eClear = 1,
    eNone  = 2,
    eMax   = 3
};

struct RenderTarget
{
    RenderTarget() = default;

    RenderTarget(DataFormat format_, TextureUsage usage_) : format(format_), usage(usage_) {}

    DataFormat format{DataFormat::eUndefined};
    TextureUsage usage{TextureUsage::eMax};
};

class RenderPassLayout
{
public:
    RenderPassLayout(uint32_t numColorRT, bool hasDepthStencilRT) :
        m_hasDepthStencilRT(hasDepthStencilRT)
    {
        if (numColorRT > MAX_COLOR_ATTACHMENT_COUNT) { numColorRT = MAX_COLOR_ATTACHMENT_COUNT; }
        m_colorRTs.resize(numColorRT);
    }

    void AddColorRenderTarget(DataFormat format, TextureUsage usage)
    {
        if (m_numColorRT < m_colorRTs.size())
        {
            RenderTarget renderTarget = {format, usage};
            m_colorRTs.emplace_back(renderTarget);
            m_numColorRT++;
        }
    }

    void SetDepthStencilRenderTarget(DataFormat format)
    {
        if (!m_hasDepthStencilRT)
        {
            m_depthStencilRT.format = format;
            m_depthStencilRT.usage  = TextureUsage::eDepthStencilAttachment;
            m_hasDepthStencilRT     = true;
        }
    }

    void SetColorTargetLoadStoreOp(RenderTargetLoadOp loadOp, RenderTargetStoreOp storeOp)
    {
        m_colorRToadOp   = loadOp;
        m_colorRTStoreOp = storeOp;
    }

    void SetDepthStencilTargetLoadStoreOp(RenderTargetLoadOp loadOp, RenderTargetStoreOp storeOp)
    {
        m_depthStencilRTLoadOp = loadOp;
        m_depthStenciRTStoreOp = storeOp;
    }

    void SetNumSamples(SampleCount sampleCount) { m_numSamples = sampleCount; }

    const auto GetNumColorRenderTargets() const { return m_numColorRT; }

    const auto GetNumSamples() const { return m_numSamples; }

    const auto HasDepthStencilRenderTarget() const { return m_hasDepthStencilRT; }

    const auto GetColorRenderTargetLoadOp() const { return m_colorRToadOp; }

    const auto GetDepthStencilRenderTargetLoadOp() const { return m_depthStencilRTLoadOp; }

    const auto GetColorRenderTargetStoreOp() const { return m_colorRTStoreOp; }

    const auto GetDepthStencilRenderTargetStoreOp() const { return m_depthStenciRTStoreOp; }

    const auto& GetColorRenderTargets() const { return m_colorRTs; }

    const auto& GetDepthStencilRenderTarget() const { return m_depthStencilRT; }

private:
    uint32_t m_numColorRT{0};
    std::vector<RenderTarget> m_colorRTs;
    RenderTarget m_depthStencilRT;
    SampleCount m_numSamples{SampleCount::e1};
    RenderTargetLoadOp m_colorRToadOp{RenderTargetLoadOp::eNone};
    RenderTargetStoreOp m_colorRTStoreOp{RenderTargetStoreOp::eNone};
    RenderTargetLoadOp m_depthStencilRTLoadOp{RenderTargetLoadOp::eNone};
    RenderTargetStoreOp m_depthStenciRTStoreOp{RenderTargetStoreOp::eNone};
    bool m_hasDepthStencilRT{false};
};

inline uint64_t GetRenderpassLayoutHash(const RenderPassLayout& layout) { return 0; }

struct RenderTargetInfo
{
    uint32_t numRenderTarget{0};
    TextureHandle* renderTargets{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
};
} // namespace zen::rhi