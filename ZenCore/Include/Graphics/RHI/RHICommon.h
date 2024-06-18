#pragma once
#include "Common/BitField.h"
#include <vector>

namespace zen::rhi
{
template <typename E> constexpr std::underlying_type_t<E> ToUnderlying(E e) noexcept
{
    return static_cast<std::underlying_type_t<E>>(e);
}

enum DataFormat : uint32_t
{
    DATA_FORMAT_UNDEFINED           = 0,   // = VK_FORMAT_UNDEFINED
    DATA_FORMAT_R16_UINT            = 74,  // = VK_FORMAT_R16_UINT
    DATA_FORMAT_R16_SINT            = 75,  // = VK_FORMAT_R16_SINT
    DATA_FORMAT_R16_SFLOAT          = 76,  // = VK_FORMAT_R16_SFLOAT
    DATA_FORMAT_R16G16_UINT         = 81,  // = VK_FORMAT_R16G16_UINT
    DATA_FORMAT_R16G16_SINT         = 82,  // = VK_FORMAT_R16G16_SINT
    DATA_FORMAT_R16G16_SFLOAT       = 83,  // = VK_FORMAT_R16G16_SFLOAT
    DATA_FORMAT_R16G16B16_UINT      = 88,  // = VK_FORMAT_R16G16B16_UINT
    DATA_FORMAT_R16G16B16_SINT      = 89,  // = VK_FORMAT_R16G16B16_SINT
    DATA_FORMAT_R16G16B16_SFLOAT    = 90,  // = VK_FORMAT_R16G16B16_SFLOAT
    DATA_FORMAT_R16G16B16A16_UINT   = 95,  // = VK_FORMAT_R16G16B16A16_UINT
    DATA_FORMAT_R16G16B16A16_SINT   = 96,  // = VK_FORMAT_R16G16B16A16_SINT
    DATA_FORMAT_R16G16B16A16_SFLOAT = 97,  // = VK_FORMAT_R16G16B16A16_SFLOAT
    DATA_FORMAT_R32_UINT            = 98,  // = VK_FORMAT_R32_UINT
    DATA_FORMAT_R32_SINT            = 99,  // = VK_FORMAT_R32_SINT
    DATA_FORMAT_R32_SFLOAT          = 100, // = VK_FORMAT_R32_SFLOAT
    DATA_FORMAT_R32G32_UINT         = 101, // = VK_FORMAT_R32G32_UINT
    DATA_FORMAT_R32G32_SINT         = 102, // = VK_FORMAT_R32G32_SINT
    DATA_FORMAT_R32G32_SFLOAT       = 103, // = VK_FORMAT_R32G32_SFLOAT
    DATA_FORMAT_R32G32B32_UINT      = 104, // = VK_FORMAT_R32G32B32_UINT
    DATA_FORMAT_R32G32B32_SINT      = 105, // = VK_FORMAT_R32G32B32_SINT
    DATA_FORMAT_R32G32B32_SFLOAT    = 106, // = VK_FORMAT_R32G32B32_SFLOAT
    DATA_FORMAT_R32G32B32A32_UINT   = 107, // = VK_FORMAT_R32G32B32A32_UINT
    DATA_FORMAT_R32G32B32A32_SINT   = 108, // = VK_FORMAT_R32G32B32A32_SINT
    DATA_FORMAT_R32G32B32A32_SFLOAT = 109, // = VK_FORMAT_R32G32B32A32_SFLOAT
    DATA_FORMAT_R64_UINT            = 110, // = VK_FORMAT_R64_UINT
    DATA_FORMAT_R64_SINT            = 111, // = VK_FORMAT_R64_SINT
    DATA_FORMAT_R64_SFLOAT          = 112, // = VK_FORMAT_R64_SFLOAT
    DATA_FORMAT_R64G64_UINT         = 113, // = VK_FORMAT_R64G64_UINT
    DATA_FORMAT_R64G64_SINT         = 114, // = VK_FORMAT_R64G64_SINT
    DATA_FORMAT_R64G64_SFLOAT       = 115, // = VK_FORMAT_R64G64_SFLOAT
    DATA_FORMAT_R64G64B64_UINT      = 116, // = VK_FORMAT_R64G64B64_UINT
    DATA_FORMAT_R64G64B64_SINT      = 117, // = VK_FORMAT_R64G64B64_SINT
    DATA_FORMAT_R64G64B64_SFLOAT    = 118, // = VK_FORMAT_R64G64B64_SFLOAT
    DATA_FORMAT_R64G64B64A64_UINT   = 119, // = VK_FORMAT_R64G64B64A64_UINT
    DATA_FORMAT_R64G64B64A64_SINT   = 120, // = VK_FORMAT_R64G64B64A64_SINT
    DATA_FORMAT_R64G64B64A64_SFLOAT = 121, // = VK_FORMAT_R64G64B64A64_SFLOAT
};

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

} // namespace zen::rhi