#pragma once
#include "Graphics/Common/Format.h"
#include "Graphics/Common/Color.h"
#include "Templates/BitField.h"
#include "Templates/HashMap.h"
#include "Templates/SmallVector.h"
#include "Math/Math.h"
#include <string>
#include <vector>

#define MAX_COLOR_ATTACHMENT_COUNT 8
#define MAX_SUBPASS_COUNT          8

#define MAX_NUM_DESCRIPTOR_SETS 8

#define ZEN_BUFFER_WHOLE_SIZE (~0ULL)

#define ALLOCA(m_size)                (assert((m_size) != 0), alloca(m_size))
#define ALLOCA_ARRAY(m_type, m_count) ((m_type*)ALLOCA(sizeof(m_type) * (m_count)))
#define ALLOCA_SINGLE(m_type)         ALLOCA_ARRAY(m_type, 1)

namespace zen
{
class RHIResource;
class RHIBuffer;
class RHITexture;

enum class RHIAPIType
{
    eVulkan = 0,
    eMax    = 1
};

struct RHIGPUInfo
{
    bool supportGeometryShader{false};
    size_t uniformBufferAlignment{0};
    size_t storageBufferAlignment{0};
};

template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

/*******************/
/**** Shaders ****/
/*******************/
enum class RHIShaderLanguage : uint32_t
{
    eGLSL = 0,
    eMax  = 1
};

enum class RHIShaderStage : uint32_t
{
    eVertex                = 0,
    eTesselationControl    = 1,
    eTesselationEvaluation = 2,
    eGeometry              = 3,
    eFragment              = 4,
    eCompute               = 5,
    eMax                   = 6
};

enum class RHIShaderStageFlagBits : uint32_t
{
    eVertex                = 1 << 0,
    eTesselationControl    = 1 << 1,
    eTesselationEvaluation = 1 << 2,
    eGeometry              = 1 << 3,
    eFragment              = 1 << 4,
    eCompute               = 1 << 5,
    eMax                   = 1 << 6
};



// enum class RHIShaderStage : uint32_t
// {
//     eVertex                = 0,
//     eTesselationControl    = 1,
//     eTesselationEvaluation = 2,
//     eGeometry              = 3,
//     eFragment              = 4,
//     eCompute               = 5,
//     eMax                   = 6
// };
//
// enum class RHIShaderStageFlagBits : uint32_t
// {
//     eVertex                = 1 << 0,
//     eTesselationControl    = 1 << 1,
//     eTesselationEvaluation = 1 << 2,
//     eGeometry              = 1 << 3,
//     eFragment              = 1 << 4,
//     eCompute               = 1 << 5,
//     eMax                   = 1 << 6
// };

static std::string RHIShaderStageToString(RHIShaderStage stage)
{
    static const char* SHADER_STAGE_NAMES[ToUnderlying(RHIShaderStage::eMax)] = {
        "Vertex", "Fragment", "TesselationControl", "TesselationEvaluation", "Geometry", "Compute",
    };
    return SHADER_STAGE_NAMES[ToUnderlying(stage)];
}

static std::string RHIShaderStageFlagToString(BitField<RHIShaderStageFlagBits> stageFlags)
{
    std::string str;
    if (stageFlags.HasFlag(RHIShaderStageFlagBits::eVertex))
    {
        str += "Vertex ";
    }
    if (stageFlags.HasFlag(RHIShaderStageFlagBits::eFragment))
    {
        str += "Fragment ";
    }
    if (stageFlags.HasFlag(RHIShaderStageFlagBits::eTesselationControl))
    {
        str += "TesselationControl ";
    }
    if (stageFlags.HasFlag(RHIShaderStageFlagBits::eTesselationEvaluation))
    {
        str += "TesselationEvaluation ";
    }
    if (stageFlags.HasFlag(RHIShaderStageFlagBits::eCompute))
    {
        str += "Compute ";
    }
    str.pop_back();
    return str;
}

static RHIShaderStageFlagBits RHIShaderStageToFlagBits(RHIShaderStage stage)
{
    return static_cast<RHIShaderStageFlagBits>(1 << ToUnderlying(stage));
}

enum class RHIShaderSpecializationConstantType : uint32_t
{
    eBool  = 0,
    eInt   = 1,
    eFloat = 2,
    eMax   = 3
};

struct RHIShaderSpecializationConstant
{
    union
    {
        uint32_t intValue = 0;
        float floatValue;
        bool boolValue;
    };

    RHIShaderSpecializationConstantType type{RHIShaderSpecializationConstantType::eMax};
    uint32_t constantId{0};
    BitField<RHIShaderStageFlagBits> stages;
};

enum class RHIShaderResourceType : uint32_t
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

struct RHIShaderResourceDescriptor
{
    std::string name;
    RHIShaderResourceType type{RHIShaderResourceType::eMax};
    BitField<RHIShaderStageFlagBits> stageFlags;
    bool writable{false}; // for storage image/buffer
    // Size of arrays (in total elements), or ubos (in bytes * total elements).
    uint32_t arraySize{1};
    uint32_t blockSize{0};
    uint32_t set{0};
    uint32_t binding{0};
};

struct RHIShaderResourceBinding
{
    RHIShaderResourceType type{RHIShaderResourceType::eMax};
    uint32_t binding{0};
    std::vector<RHIResource*> resources;
};

using RHIShaderResourceDescriptorTable =
    SmallVector<SmallVector<RHIShaderResourceDescriptor>, MAX_NUM_DESCRIPTOR_SETS>;
// .vert .frag .compute together
struct RHIShaderGroupInfo
{
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
        BitField<RHIShaderStageFlagBits> stageFlags;
    };

    HashMap<RHIShaderStage, std::vector<uint8_t>> sprivCode;
    ShaderPushConstants pushConstants{};
    // vertex input attribute
    std::vector<VertexInputAttribute> vertexInputAttributes;
    // vertex binding stride
    uint32_t vertexBindingStride{0};
    // per set shader resources
    // std::vector<std::vector<RHIShaderResourceDescriptor>> SRDs;
    RHIShaderResourceDescriptorTable SRDTable;
    // specialization constants
    std::vector<RHIShaderSpecializationConstant> specializationConstants;
    std::string name;
};

/*****************************/
/**** Gfx Pipeline States ****/
/*****************************/
enum class RHIDrawPrimitiveType : uint32_t
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

enum class RHIPolygonCullMode : uint32_t
{
    eDisabled     = 0,
    eFront        = 1,
    eBack         = 2,
    eFrontAndBack = 3,
    eMax          = 4
};

enum class RHIPolygonFrontFace : uint32_t
{
    eCounterClockWise = 0,
    eClockWise        = 1,
    eMax              = 2,
};

enum class RHIDepthCompareOperator : uint32_t
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

enum class RHIBlendLogicOp : uint32_t
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

struct RHIGfxPipelineRasterizationState
{
    bool enableDepthClamp{false};
    bool discardPrimitives{false};
    bool wireframe{false};
    RHIPolygonCullMode cullMode{RHIPolygonCullMode::eDisabled};
    RHIPolygonFrontFace frontFace{RHIPolygonFrontFace::eCounterClockWise};
    bool enableDepthBias{false};
    float depthBiasConstantFactor{0.0f};
    float depthBiasClamp{0.0f};
    float depthBiasSlopeFactor{0.0f};
    float lineWidth{1.0f};
};

struct RHIGfxPipelineMultiSampleState
{
    SampleCount sampleCount{SampleCount::e1};
    bool enableSampleShading{false};
    float minSampleShading{0.0f};
    bool enableAlphaToCoverage{false};
    bool enbaleAlphaToOne{false};
    std::vector<uint32_t> sampleMasks;
};

enum class RHIStencilOp : uint32_t
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

struct RHIStencilOpState
{
    RHIStencilOp fail{RHIStencilOp::eReplace};
    RHIStencilOp pass{RHIStencilOp::eReplace};
    RHIStencilOp depthFail{RHIStencilOp::eReplace};
    RHIDepthCompareOperator compare{RHIDepthCompareOperator::eAlways};
    uint32_t compareMask{0};
    uint32_t writeMask{0};
    uint32_t reference{0};
};


struct RHIGfxPipelineDepthStencilState
{
    bool enableDepthTest{false};
    bool enableDepthWrite{false};
    RHIDepthCompareOperator depthCompareOp{RHIDepthCompareOperator::eNever};
    bool enableDepthBoundsTest{false};
    bool enableStencilTest{false};
    RHIStencilOpState frontOp{};
    RHIStencilOpState backOp{};
    float minDepthBounds{0.0f};
    float maxDepthBounds{1.0f};

    static RHIGfxPipelineDepthStencilState Create(bool enableDepthTest,
                                                  bool enableDepthWrite,
                                                  RHIDepthCompareOperator depthCompareOp)
    {
        RHIGfxPipelineDepthStencilState state{};
        state.enableDepthTest  = enableDepthTest;
        state.enableDepthWrite = enableDepthWrite;
        state.depthCompareOp   = depthCompareOp;
        return state;
    }
};

enum class RHIBlendFactor : uint32_t
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

enum class RHIBlendOp : uint32_t
{
    eAdd              = 0,
    eSubstract        = 1,
    eReverseSubstract = 2,
    eMinimum          = 3,
    eMaximum          = 4,
    eMax              = 5
};

struct RHIGfxPipelineColorBlendState
{
    bool enableLogicOp{false};
    RHIBlendLogicOp logicOp{RHIBlendLogicOp::eClear};

    struct Attachment
    {
        bool enableBlend{false};
        RHIBlendFactor srcColorBlendFactor{RHIBlendFactor::eZero};
        RHIBlendFactor dstColorBlendFactor{RHIBlendFactor::eZero};
        RHIBlendOp colorBlendOp{RHIBlendOp::eAdd};
        RHIBlendFactor srcAlphaBlendFactor{RHIBlendFactor::eZero};
        RHIBlendFactor dstAlphaBlendFactor{RHIBlendFactor::eZero};
        RHIBlendOp alphaBlendOp{RHIBlendOp::eAdd};
        bool writeR{true};
        bool writeG{true};
        bool writeB{true};
        bool writeA{true};
    };

    static RHIGfxPipelineColorBlendState CreateColorWriteDisabled(int count = 1)
    {
        RHIGfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            Attachment attachment{};
            attachment.writeR = false;
            attachment.writeG = false;
            attachment.writeB = false;
            attachment.writeA = false;
            bs.attachments.emplace_back(attachment);
        }
        return bs;
    }

    static RHIGfxPipelineColorBlendState CreateDisabled(int count = 1)
    {
        RHIGfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            bs.attachments.emplace_back();
        }
        return bs;
    }

    static RHIGfxPipelineColorBlendState CreateBlend(int count = 1)
    {
        RHIGfxPipelineColorBlendState bs;
        for (int i = 0; i < count; i++)
        {
            Attachment ba;
            ba.enableBlend         = true;
            ba.srcColorBlendFactor = RHIBlendFactor::eSrcAlpha;
            ba.dstColorBlendFactor = RHIBlendFactor::eOneMinusSrcAlpha;
            ba.srcAlphaBlendFactor = RHIBlendFactor::eSrcAlpha;
            ba.dstAlphaBlendFactor = RHIBlendFactor::eOneMinusSrcAlpha;

            bs.attachments.emplace_back(ba);
        }
        return bs;
    }

    std::vector<Attachment> attachments; // One per render target texture.
    Color blendConstants;
};

enum class RHIDynamicState : uint32_t
{
    eViewPort  = 0,
    eScissor   = 1,
    eLineWidth = 2,
    eDepthBias = 3,
    eMax       = 4
};

struct RHIGfxPipelineStates
{
    RHIDrawPrimitiveType primitiveType{RHIDrawPrimitiveType::eTriangleList};
    RHIGfxPipelineRasterizationState rasterizationState;
    RHIGfxPipelineMultiSampleState multiSampleState;
    RHIGfxPipelineDepthStencilState depthStencilState;
    RHIGfxPipelineColorBlendState colorBlendState;
    std::vector<RHIDynamicState> dynamicStates;
};

/*****************************/
/********* Buffers ***********/
/*****************************/
enum class RHIBufferUsageFlagBits : uint32_t
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

enum class RHIBufferUsage : uint32_t
{
    eNone           = 0,
    eTransferSrc    = 1,
    eTransferDst    = 2,
    eTextureBuffer  = 3,
    eImageBuffer    = 4,
    eUniformBuffer  = 5,
    eStorageBuffer  = 6,
    eIndexBuffer    = 7,
    eVertexBuffer   = 8,
    eIndirectBuffer = 9,
    eMax            = 10
};

struct RHIBufferCopyRegion
{
    uint64_t srcOffset{0};
    uint64_t dstOffset{0};
    uint64_t size{0};
};

struct RHIBufferCopySource
{
    RHIBuffer* buffer{nullptr};
    RHIBufferCopyRegion region;
};

enum class RHIBufferAllocateType : uint32_t
{
    eNone = 0,
    eCPU  = 1,
    eGPU  = 2,
    eMax  = 3
};

/*****************************/
/********** Sampler **********/
/*****************************/
enum class RHISamplerFilter : uint32_t
{
    eNearest = 0,
    eLinear  = 1,
};

enum class RHISamplerRepeatMode : uint32_t
{
    eRepeat            = 0,
    eMirroredRepeat    = 1,
    eClampToEdge       = 2,
    eClampToBorder     = 3,
    eMirrorClampToEdge = 4,
    eMax               = 0x7FFFFFFF
};

enum class RHISamplerBorderColor : uint32_t
{
    eFloatTransparentBlack = 0,
    eIntTransparentBlack   = 1,
    eFloatOpaqueBlack      = 2,
    eIntOpaqueBlack        = 3,
    eFloatOpaqueWhite      = 4,
    eIntOpaqueWhite        = 5,
    eMax                   = 0x7FFFFFFF
};

/*****************************/
/********* Textures **********/
/*****************************/
enum class RHITextureUsageFlagBits : uint32_t
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

enum class RHITextureUsage : uint32_t
{
    eNone                   = 0,
    eTransferSrc            = 1,
    eTransferDst            = 2,
    eSampled                = 3,
    eStorage                = 4,
    eColorAttachment        = 5,
    eDepthStencilAttachment = 6,
    eTransientAttachment    = 7,
    eInputAttachment        = 8,
    eMax                    = 9
};

enum class RHITextureAspectFlagBits : uint32_t
{
    eColor   = 1 << 0,
    eDepth   = 1 << 1,
    eStencil = 1 << 2,
    eMax     = 0x7FFFFFFF
};

enum class RHITextureLayout : uint32_t
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

enum class RHITextureType : uint32_t
{
    e1D   = 0,
    e2D   = 1,
    e3D   = 2,
    eCube = 3,
    eMax  = 4
};

struct RHITextureSubResourceRange
{
    BitField<RHITextureAspectFlagBits> aspect;
    uint32_t baseMipLevel{0};
    uint32_t levelCount{1};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{1};

    static RHITextureSubResourceRange Color(uint32_t baseMiplevel   = 0,
                                            uint32_t baseArrayLayer = 0,
                                            uint32_t levelCount     = 1,
                                            uint32_t layerCount     = 1)
    {
        RHITextureSubResourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eColor);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMiplevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static RHITextureSubResourceRange Depth(uint32_t baseMipLevel   = 0,
                                            uint32_t baseArrayLayer = 0,
                                            uint32_t levelCount     = 1,
                                            uint32_t layerCount     = 1)
    {
        RHITextureSubResourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static RHITextureSubResourceRange Stencil(uint32_t baseMipLevel   = 0,
                                              uint32_t baseArrayLayer = 0,
                                              uint32_t levelCount     = 1,
                                              uint32_t layerCount     = 1)
    {
        RHITextureSubResourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }

    static RHITextureSubResourceRange DepthStencil(uint32_t baseMipLevel   = 0,
                                                   uint32_t baseArrayLayer = 0,
                                                   uint32_t levelCount     = 1,
                                                   uint32_t layerCount     = 1)
    {
        RHITextureSubResourceRange range{};
        range.aspect.SetFlag(RHITextureAspectFlagBits::eDepth);
        range.aspect.SetFlag(RHITextureAspectFlagBits::eStencil);
        range.layerCount     = layerCount;
        range.levelCount     = levelCount;
        range.baseMipLevel   = baseMipLevel;
        range.baseArrayLayer = baseArrayLayer;

        return range;
    }
};

struct RHITextureSubresourceLayers
{
    BitField<RHITextureAspectFlagBits> aspect;
    uint32_t mipmap{0};
    uint32_t baseArrayLayer{0};
    uint32_t layerCount{1};
};

struct RHITextureCopyRegion
{
    Vec3i srcOffset;
    Vec3i dstOffset;
    Vec3i size;
    RHITextureSubresourceLayers srcSubresources;
    RHITextureSubresourceLayers dstSubresources;
};

struct RHIBufferTextureCopyRegion
{
    uint64_t bufferOffset{0};
    RHITextureSubresourceLayers textureSubresources;
    Vec3i textureOffset;
    Vec3i textureSize;
};

struct RHIBufferTextureCopySource
{
    RHIBuffer* buffer{nullptr};
    RHIBufferTextureCopyRegion region;
};

inline RHITextureLayout RHITextureUsageToLayout(RHITextureUsage usage)
{
    switch (usage)
    {
        case RHITextureUsage::eNone: return RHITextureLayout::eUndefined;
        case RHITextureUsage::eTransferSrc: return RHITextureLayout::eTransferSrc;
        case RHITextureUsage::eTransferDst: return RHITextureLayout::eTransferDst;
        case RHITextureUsage::eSampled:
        case RHITextureUsage::eInputAttachment: return RHITextureLayout::eShaderReadOnly;
        case RHITextureUsage::eStorage: return RHITextureLayout::eGeneral;
        case RHITextureUsage::eColorAttachment: return RHITextureLayout::eColorTarget;
        case RHITextureUsage::eDepthStencilAttachment: return RHITextureLayout::eDepthStencilTarget;
        default: return RHITextureLayout::eUndefined;
    }
}

/*****************************/
/** Renderpass and subpass ***/
/*****************************/
enum class RHIRenderTargetLoadOp : uint32_t
{
    eLoad  = 0,
    eClear = 1,
    eNone  = 2,
    eMax   = 3
};

enum class RHIRenderTargetStoreOp : uint32_t
{
    eStore = 0,
    eNone  = 1,
    eMax   = 2
};

struct RHIRenderTargetClearValue
{
    union
    {
        Color color;
        struct
        {
            float depth;
            uint32_t stencil;
        };
    };

    RHIRenderTargetClearValue() {}

    RHIRenderTargetClearValue(Color c) : color(c) {}

    RHIRenderTargetClearValue(float depth, uint32_t stencil) : depth(depth), stencil(stencil) {}

    static RHIRenderTargetClearValue ColorClear(Color c)
    {
        return RHIRenderTargetClearValue(c);
    }

    static RHIRenderTargetClearValue DefaultColorClear()
    {
        return RHIRenderTargetClearValue(Color(0.8f, 0.8f, 0.8f, 1.0f));
    }

    static RHIRenderTargetClearValue DepthStencilClear(float depth, uint32_t stencil)
    {
        return RHIRenderTargetClearValue(depth, stencil);
    }

    static RHIRenderTargetClearValue DefaultDepthStencilClear()
    {
        return RHIRenderTargetClearValue(1.0f, 0.0f);
    }
};

inline RHIRenderTargetClearValue DEFAULT_COLOR_CLEAR_VALUE =
    RHIRenderTargetClearValue(Color(0.8f, 0.8f, 0.8f, 0.0f));

inline RHIRenderTargetClearValue DEFAULT_DS_CLEAR_VALUE = RHIRenderTargetClearValue(1.0f, 0);

struct RHIRenderTarget
{
    DataFormat format{DataFormat::eUndefined};
    SampleCount numSamples{SampleCount::e1};
    RHIRenderTargetLoadOp loadOp{RHIRenderTargetLoadOp::eNone};
    RHIRenderTargetStoreOp storeOp{RHIRenderTargetStoreOp::eStore};
    RHITexture* texture{nullptr};
    RHIRenderTargetClearValue clearValue;
};

union RHIRenderPassClearValue
{
    Color color = {};

    struct
    {
        float depth;
        uint32_t stencil;
    };

    RHIRenderPassClearValue() {}
};

class RHIRenderPassLayout
{
public:
    RHIRenderPassLayout() = default;

    ~RHIRenderPassLayout()
    {
        // m_rtHandles.clear();
        m_colorRTs.clear();
        // m_rtSubResRanges.clear();
    }

    void AddColorRenderTarget(DataFormat format,
                              RHITexture* texture,
                              const RHITextureSubResourceRange& subResourceRange,
                              RHIRenderTargetLoadOp loadOp,
                              RHIRenderTargetStoreOp storeOp,
                              SampleCount numSamples = SampleCount::e1)
    {
        RHIRenderTarget colorRT;
        colorRT.format  = format;
        colorRT.texture = texture;
        // colorRT.subresourceRange = subResourceRange;

        colorRT.numSamples = numSamples;
        colorRT.loadOp     = loadOp;
        colorRT.storeOp    = storeOp;

        m_colorRTs.emplace_back(colorRT);
        m_numColorRT++;
        // m_rtHandles.push_back(handle);
        // m_rtSubResRanges.emplace_back(subResourceRange);
    }

    void SetDepthStencilRenderTarget(DataFormat format,
                                     RHITexture* texture,
                                     RHITextureSubResourceRange subResourceRange,
                                     RHIRenderTargetLoadOp loadOp,
                                     RHIRenderTargetStoreOp storeOp)
    {
        if (!m_hasDepthStencilRT)
        {
            m_depthStencilRT.format  = format;
            m_depthStencilRT.texture = texture;
            // m_depthStencilRT.subresourceRange = subResourceRange;
            m_depthStencilRT.loadOp  = loadOp;
            m_depthStencilRT.storeOp = storeOp;
            // m_depthStencilRT.usage  = RHITextureUsage::eDepthStencilAttachment;
            m_hasDepthStencilRT = true;
            // m_rtHandles.push_back(handle);
            // m_rtSubResRanges.emplace_back(subResourceRange);
        }
    }

    // void SetColorTargetLoadStoreOp(RHIRenderTargetLoadOp loadOp, RHIRenderTargetStoreOp storeOp)
    // {
    //     m_colorRToadOp   = loadOp;
    //     m_colorRTStoreOp = storeOp;
    // }
    //
    // void SetDepthStencilTargetLoadStoreOp(RHIRenderTargetLoadOp loadOp, RHIRenderTargetStoreOp storeOp)
    // {
    //     m_depthStencilRTLoadOp  = loadOp;
    //     m_depthStencilRTStoreOp = storeOp;
    // }
    //
    // void SetNumSamples(SampleCount sampleCount)
    // {
    //     m_numSamples = sampleCount;
    // }

    const auto GetNumColorRenderTargets() const
    {
        return m_numColorRT;
    }

    // const auto GetNumSamples() const
    // {
    //     return m_numSamples;
    // }

    const auto HasDepthStencilRenderTarget() const
    {
        return m_hasDepthStencilRT;
    }

    const auto HasColorRenderTarget() const
    {
        return m_numColorRT > 0;
    }

    // const auto GetColorRenderTargetLoadOp() const
    // {
    //     return m_colorRToadOp;
    // }
    //
    // const auto GetDepthStencilRenderTargetLoadOp() const
    // {
    //     return m_depthStencilRTLoadOp;
    // }
    //
    // const auto GetColorRenderTargetStoreOp() const
    // {
    //     return m_colorRTStoreOp;
    // }
    //
    // const auto GetDepthStencilRenderTargetStoreOp() const
    // {
    //     return m_depthStencilRTStoreOp;
    // }

    const auto& GetColorRenderTargets() const
    {
        return m_colorRTs;
    }

    const auto& GetDepthStencilRenderTarget() const
    {
        return m_depthStencilRT;
    }

    // const TextureHandle& GetDepthStencilRenderTargetHandle() const
    // {
    //     return m_rtHandles.back();
    // }
    //
    // const TextureHandle* GetRenderTargetHandles() const
    // {
    //     return m_rtHandles.data();
    // }
    //
    // TextureHandle* GetRenderTargetHandles()
    // {
    //     return m_rtHandles.data();
    // }
    //
    // const auto& GetRTSubResourceRanges()
    // {
    //     return m_rtSubResRanges;
    // }

    // auto GetNumRenderTargets() const
    // {
    //     return m_hasDepthStencilRT ? m_numColorRT + 1 : m_numColorRT;
    // }

    void ClearRenderTargetInfo()
    {
        // m_rtHandles.clear();
        m_colorRTs.clear();
        m_hasDepthStencilRT = false;
        m_numColorRT        = 0;
    }

private:
    uint32_t m_numColorRT{0};
    std::vector<RHIRenderTarget> m_colorRTs;
    RHIRenderTarget m_depthStencilRT;
    // SampleCount m_numSamples{SampleCount::e1};
    // RHIRenderTargetLoadOp m_colorRToadOp{RHIRenderTargetLoadOp::eNone};
    // RHIRenderTargetStoreOp m_colorRTStoreOp{RHIRenderTargetStoreOp::eNone};
    // RHIRenderTargetLoadOp m_depthStencilRTLoadOp{RHIRenderTargetLoadOp::eNone};
    // RHIRenderTargetStoreOp m_depthStencilRTStoreOp{RHIRenderTargetStoreOp::eNone};
    // std::vector<TextureHandle> m_rtHandles;
    // std::vector<RHITextureSubResourceRange> m_rtSubResRanges;
    bool m_hasDepthStencilRT{false};
};

struct RHIFramebufferInfo
{
    uint32_t numRenderTarget{0};
    RHITexture** renderTargets{nullptr};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t depth{1};
};

enum class RHIPipelineType : uint32_t
{
    eNone     = 0,
    eGraphics = 1,
    eCompute  = 2,
    eMax      = 3
};

/**********************************************/
/********** Context for RHICommandList ********/
/*********************************************/
// struct VertexBufferBinding
// {
//     BufferHandle buffer{nullptr};
//     uint64_t offset{0};
//     uint32_t slot;
//
//     bool operator==(const VertexBufferBinding& b) const
//     {
//         return buffer == b.buffer && slot == b.slot && offset == b.offset;
//     }
//
//     bool operator!=(const VertexBufferBinding& b) const
//     {
//         return !(*this == b);
//     }
// };
//
// struct IndexBufferBinding
// {
//     BufferHandle buffer{nullptr};
//     uint64_t offset{0};
//     DataFormat format{DataFormat::eUndefined};
//
//
//     bool operator==(const IndexBufferBinding& b) const
//     {
//         return buffer == b.buffer && format == b.format && offset == b.offset;
//     }
//
//     bool operator!=(const IndexBufferBinding& b) const
//     {
//         return !(*this == b);
//     }
// };


/*****************************/
/********* Barriers **********/
/*****************************/
enum class RHIAccessMode
{
    eNone      = 0,
    eRead      = 1,
    eReadWrite = 2
};

enum class RHIAccessFlagBits : uint32_t
{
    eNone                        = 0,
    eIndirectCommandRead         = 1 << 0,
    eIndexRead                   = 1 << 1,
    eVertexAttributeRead         = 1 << 2,
    eUniformRead                 = 1 << 3,
    eInputAttachmentRead         = 1 << 4,
    eShaderRead                  = 1 << 5,
    eShaderWrite                 = 1 << 6,
    eColorAttachmentRead         = 1 << 7,
    eColorAttachmentWrite        = 1 << 8,
    eDepthStencilAttachmentRead  = 1 << 9,
    eDepthStencilAttachmentWrite = 1 << 10,
    eTransferRead                = 1 << 11,
    eTransferWrite               = 1 << 12,
    eHostRead                    = 1 << 13,
    eHostWrite                   = 1 << 14,
    eMemoryRead                  = 1 << 15,
    eMemoryWrite                 = 1 << 16,
    eMax                         = 0x7FFFFFFF
};

enum class RHIPipelineStageBits : uint32_t
{
    eNone                         = 0,
    eTopOfPipe                    = 1 << 0,
    eDrawIndirect                 = 1 << 1,
    eVertexInput                  = 1 << 2,
    eVertexShader                 = 1 << 3,
    eTessellationControlShader    = 1 << 4,
    eTessellationEvaluationShader = 1 << 5,
    eGeometryShader               = 1 << 6,
    eFragmentShader               = 1 << 7,
    eEarlyFragmentTests           = 1 << 8,
    eLateFragmentTests            = 1 << 9,
    eColorAttachmentOutput        = 1 << 10,
    eComputeShader                = 1 << 11,
    eTransfer                     = 1 << 12,
    eBottomOfPipe                 = 1 << 13,
    eHost                         = 1 << 14,
    eAllGraphics                  = 1 << 15,
    eAllCommands                  = 1 << 16,
    eMax                          = 0x7FFFFFFF
};

inline BitField<RHIAccessFlagBits> RHITextureUsageToAccessFlagBits(RHITextureUsage usage,
                                                                   RHIAccessMode mode)
{
    BitField<RHIAccessFlagBits> result;
    switch (usage)
    {
        case RHITextureUsage::eNone: break;
        case RHITextureUsage::eTransferSrc: result.SetFlag(RHIAccessFlagBits::eTransferRead); break;
        case RHITextureUsage::eTransferDst:
            result.SetFlag(RHIAccessFlagBits::eTransferWrite);
            break;
        case RHITextureUsage::eSampled: result.SetFlag(RHIAccessFlagBits::eShaderRead); break;
        case RHITextureUsage::eStorage:
        {
            result.SetFlag(RHIAccessFlagBits::eShaderRead);
            if (mode == RHIAccessMode::eReadWrite)
            {
                result.SetFlag(RHIAccessFlagBits::eShaderWrite);
            }
        }
        break;
        case RHITextureUsage::eColorAttachment:
            result.SetFlag(mode == RHIAccessMode::eRead ? RHIAccessFlagBits::eColorAttachmentRead :
                                                          RHIAccessFlagBits::eColorAttachmentWrite);
            break;
        case RHITextureUsage::eDepthStencilAttachment:
            result.SetFlag(mode == RHIAccessMode::eRead ?
                               RHIAccessFlagBits::eDepthStencilAttachmentRead :
                               RHIAccessFlagBits::eDepthStencilAttachmentWrite);
            break;
        case RHITextureUsage::eInputAttachment:
            result.SetFlag(RHIAccessFlagBits::eInputAttachmentRead);
            break;
        default: break;
    }
    return result;
}

inline BitField<RHIPipelineStageBits> RHITextureUsageToPipelineStage(RHITextureUsage usage)
{
    BitField<RHIPipelineStageBits> result;
    switch (usage)
    {
        case RHITextureUsage::eNone: result.SetFlag(RHIPipelineStageBits::eTopOfPipe); break;

        case RHITextureUsage::eTransferSrc:
        case RHITextureUsage::eTransferDst: result.SetFlag(RHIPipelineStageBits::eTransfer); break;

        case RHITextureUsage::eSampled:
        case RHITextureUsage::eInputAttachment:
        case RHITextureUsage::eStorage:
            result.SetFlag(RHIPipelineStageBits::eFragmentShader);
            break;

        case RHITextureUsage::eColorAttachment:
            result.SetFlag(RHIPipelineStageBits::eColorAttachmentOutput);
            break;

        case RHITextureUsage::eDepthStencilAttachment:
            result.SetFlag(RHIPipelineStageBits::eEarlyFragmentTests);
            result.SetFlag(RHIPipelineStageBits::eLateFragmentTests);
        default: break;
    }
    return result;
}

inline BitField<RHIPipelineStageBits> RHIBufferUsageToPipelineStage(RHIBufferUsage usage)
{
    BitField<RHIPipelineStageBits> result;
    switch (usage)
    {
        case RHIBufferUsage::eNone: result.SetFlag(RHIPipelineStageBits::eTopOfPipe); break;

        case RHIBufferUsage::eTransferSrc:
        case RHIBufferUsage::eTransferDst: result.SetFlag(RHIPipelineStageBits::eTransfer); break;

        case RHIBufferUsage::eTextureBuffer:
        case RHIBufferUsage::eImageBuffer:
        case RHIBufferUsage::eUniformBuffer:
        case RHIBufferUsage::eStorageBuffer:
        case RHIBufferUsage::eIndirectBuffer:
            result.SetFlags(RHIPipelineStageBits::eAllGraphics, RHIPipelineStageBits::eAllCommands);
            break;

        case RHIBufferUsage::eIndexBuffer:
        case RHIBufferUsage::eVertexBuffer:
            result.SetFlag(RHIPipelineStageBits::eVertexShader);
            break;

        default: break;
    }
    return result;
}

inline BitField<RHIAccessFlagBits> RHIBufferUsageToAccessFlagBits(RHIBufferUsage usage,
                                                                  RHIAccessMode mode)
{
    BitField<RHIAccessFlagBits> result;
    switch (usage)
    {
        case RHIBufferUsage::eTransferSrc: result.SetFlag(RHIAccessFlagBits::eTransferRead); break;
        case RHIBufferUsage::eTransferDst: result.SetFlag(RHIAccessFlagBits::eTransferWrite); break;

        case RHIBufferUsage::eUniformBuffer:
        case RHIBufferUsage::eTextureBuffer: result.SetFlag(RHIAccessFlagBits::eShaderRead); break;

        case RHIBufferUsage::eStorageBuffer:
        case RHIBufferUsage::eImageBuffer:
        {
            result.SetFlag(RHIAccessFlagBits::eShaderRead);
            if (mode == RHIAccessMode::eReadWrite)
            {
                result.SetFlag(RHIAccessFlagBits::eShaderWrite);
            }
        }
        break;

        case RHIBufferUsage::eIndexBuffer: result.SetFlag(RHIAccessFlagBits::eIndexRead); break;
        case RHIBufferUsage::eVertexBuffer:
            result.SetFlag(RHIAccessFlagBits::eVertexAttributeRead);
            break;
        case RHIBufferUsage::eIndirectBuffer:
            result.SetFlag(RHIAccessFlagBits::eIndirectCommandRead);
            break;
        default: break;
    }
    return result;
}

struct RHIMemoryTransition
{
    BitField<RHIAccessFlagBits> srcAccess;
    BitField<RHIAccessFlagBits> dstAccess;
};

struct RHITextureTransition
{
    RHIAccessMode oldAccessMode;
    RHIAccessMode newAccessMode;
    RHITexture* texture{nullptr};
    RHITextureUsage oldUsage;
    RHITextureUsage newUsage;
    RHITextureSubResourceRange subResourceRange;
};

struct RHIBufferTransition
{
    RHIAccessMode oldAccessMode;
    RHIAccessMode newAccessMode;
    RHIBuffer* buffer{nullptr};
    RHIBufferUsage oldUsage;
    RHIBufferUsage newUsage;
    uint64_t offset{0};
    uint64_t size{ZEN_BUFFER_WHOLE_SIZE};
};
} // namespace zen